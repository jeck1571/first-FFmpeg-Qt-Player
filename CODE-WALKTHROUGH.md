# FFmpeg Qt Player 完整源码详解

> 版本提示（2026-11-12）：本文保留了早期逐模块讲解，部分渲染和时钟章节仍描述旧实现。当前源码已加入 `AudioClock` 音频主时钟、`atempo` 保音高倍速、D3D11VA 硬解、D3D11 VideoProcessor + Qt RHI 零拷贝、`D3d11ContextSync`、热切轨和有界缓存。阅读当前播放路径时以 [TECH-STACK.md](TECH-STACK.md)、[README.md](README.md) 和源码为准；文中“墙钟主时钟”“纯软件解码”“每帧 QImage”等描述属于历史基线。

本文不是简单的“函数列表”，而是按一条媒体从磁盘或网络进入程序、经过 FFmpeg 解封装和解码、被重采样或像素转换、再交给 Qt 输出，最后又被界面控制的完整路径来讲解。

建议配合以下文件阅读：

1. `main.cpp`
2. `MainWindow.h`
3. `MainWindow.cpp`
4. `FFmpegPlayer.h`
5. `FFmpegPlayer.cpp`
6. `VideoCanvas.h`
7. `VideoCanvas.cpp`
8. `SubtitleTrack.h`
9. `SubtitleTrack.cpp`
10. `CMakeLists.txt`

源码行号会随着后续修改移动，因此本文优先使用“文件名 + 类名 + 函数名 + 数据成员”定位，避免只给行号后很快失效。

---

## 1. 这个工程到底实现了什么

程序的目标是做一个 Windows 桌面播放器，但不借助 libVLC、QMediaPlayer 或系统播放器。它自己完成以下工作：

- 识别媒体容器。
- 找到最佳视频流和最佳音频流。
- 打开 FFmpeg 解码器。
- 持续读取 AVPacket。
- 将不同流的数据包送入不同阻塞队列。
- 在音频线程和视频线程中分别解码。
- 用媒体时钟决定当前帧和音频帧什么时候输出。
- 用 `swr_convert` 将任意 FFmpeg 音频格式转成 Qt 可播放的 16 位小端交错 PCM。
- 用 `QAudioSink` 把 PCM 写入扬声器。
- 用 `sws_scale` 将视频帧转成 `QImage::Format_ARGB32` 可用的 BGRA 像素。
- 用 Qt Widgets 和 `QPainter` 等比显示视频。
- 在视频画面上叠加纯文本字幕。
- 提供播放列表、URL、M3U/PLS、循环、随机、全屏等桌面产品功能。
- 提供中英文界面，运行时切换并持久化。

程序不实现编码，也不写音视频文件。截图只保存当前显示帧。

---

## 2. 总体架构

### 2.1 分层

```text
┌─────────────────────────────────────────────────────────────┐
│ main.cpp                                                    │
│ QApplication + MainWindow                                   │
└──────────────────────────────┬──────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────┐
│ MainWindow                                                  │
│ 菜单、快捷键、播放列表、文件/URL、字幕、设置、全屏、截图       │
└───────────────┬──────────────────────────────┬──────────────┘
                │ Qt signal/slot               │ setFrame/setSubtitle
┌───────────────▼──────────────┐  ┌────────────▼──────────────┐
│ FFmpegPlayer                 │  │ VideoCanvas               │
│ 解封装、解码、时钟、QAudioSink │  │ 等比绘制、字幕叠加、交互     │
└───────────────┬──────────────┘  └───────────────────────────┘
                │
┌───────────────▼─────────────────────────────────────────────┐
│ FFmpeg                                                      │
│ libavformat / libavcodec / libswresample / libswscale       │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 线程模型

播放开始后，一个媒体通常会同时存在：

```text
主线程
  ├─ Qt 事件循环
  ├─ 每 100 ms 刷 UI
  ├─ 读取按钮、菜单、列表操作
  └─ 绘制 VideoCanvas

controlThread_
  ├─ av_read_frame
  ├─ 处理 seek
  ├─ 决定 AVPacket 进入音频队列还是视频队列
  ├─ 等待音频和视频线程结束
  └─ 关闭输入上下文

audioThread_
  ├─ avcodec_receive_frame / avcodec_send_packet
  ├─ swr_convert
  ├─ QAudioSink::write
  └─ 推进播放位置

videoThread_
  ├─ avcodec_receive_frame / avcodec_send_packet
  ├─ waitUntil
  ├─ sws_scale
  └─ 将 QImage 交给 VideoCanvas
```

Qt 主线程不直接做解码和阻塞式音频写入，所以拖动窗口时不会因为解码而整体卡死。

`FFmpegPlayer` 的 QObject 生命周期属于主线程，但 `run()`、`audioLoop()`、`videoLoop()` 使用 `std::thread`。信号可以在工作线程中发出，连接到 `MainWindow` 的槽会按 Qt 的跨线程队列语义回到主线程执行。

`VideoCanvas` 是普通 QWidget，只能在主线程创建和绘制。视频线程不能直接调用它的 `paintEvent`，所以 `setFrame()` 只负责在互斥锁保护下替换 QImage，再用原子标志合并 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 的 `update()` 请求——一帧未画完之前不再重复排队，帧率高时不会把主线程事件队列撑满。

---

## 3. 程序入口与启动顺序

### 3.1 `main.cpp`

入口流程：

```cpp
QApplication application(argc, argv);
QApplication::setOrganizationName("FFmpegQtPlayer");
QApplication::setApplicationName("FFmpeg Qt Player");
QApplication::setApplicationVersion("2.0.0");
application.setStyle(QStyleFactory::create("Fusion"));

MainWindow window;
window.show();
window.openCommandLineMedia(application.arguments().mid(1));
return application.exec();
```

关键点：

- `QApplication` 必须先于任何 QWidget 构造。
- `OrganizationName` 和 `ApplicationName` 决定 `QStandardPaths::AppDataLocation` 的目录。
- `Fusion` 样式让浅色 Windows 主题下也保持一致的深色控件外观。
- `arguments().mid(1)` 去掉 exe 自身路径，剩下的第一个媒体路径会被打开。
- `application.exec()` 进入 Qt 事件循环，程序生命到这里开始等待用户操作和定时器。

### 3.2 `MainWindow` 构造顺序

`MainWindow` 构造函数依次调用：

1. `qRegisterMetaType<FFmpegPlayer::State>()`
2. `setAcceptDrops(true)`
3. `setMinimumSize(820, 500)`
4. `buildInterface()`
5. `buildMenus()`
6. `configureStyle()`
7. `connectPlayer()`
8. `loadSettings()`（含 `ui/language`）
9. `recordUiTexts()` 记录英文原文，设置画布占位文字
10. 若语言为中文，安装翻译器并 `retranslateUi()`
11. 设置播放器画布、音量、静音和速度
12. 创建并启动 100 ms UI 定时器
13. `updateState(Idle)`
14. 刷新播放列表按钮和状态文本

`qRegisterMetaType` 用于跨线程信号参数的类型识别。虽然 `Q_ENUM` 已经提供元对象信息，显式注册可以避免关闭自动注册的环境下出现队列连接警告。

注意第 9、10 步的顺序：界面构建时 translator 尚未安装，所以 `tr()` 返回英文，`recordUiTexts()` 记录到的就是英文原文；随后安装翻译并统一重刷，这也是之后能切回英文的前提。

---

## 4. 主窗口界面结构

### 4.1 中央区域

中央 QWidget 内部是：

```text
QVBoxLayout
├─ QSplitter（水平）
│  ├─ VideoCanvas
│  └─ playlistPanel_
└─ controlBar_
   ├─ seekLayout
   │  ├─ seekSlider_
   │  └─ timeLabel_
   └─ transportLayout
      ├─ 上一项、播放、快退 5 秒、快进 5 秒、下一项、停止
      ├─ speedCombo_
      ├─ loopButton_
      ├─ shuffleButton_
      ├─ stretch
      ├─ muteButton_
      ├─ volumeSlider_
      ├─ volumeLabel_
      └─ mediaLabel_
```

视频区和播放列表使用 `QSplitter`。分割条不能把任何一侧完全折叠。

播放列表有：

- 标题。
- 项目数量。
- 搜索 QLineEdit（按标题或路径过滤）。
- QListWidget。
- 播放选中项按钮。
- 删除按钮。
- 保存按钮。

### 4.2 视频画布尺寸

`VideoCanvas` 最小尺寸为 `320x180`，播放列表最小宽度为 250，主窗口最小尺寸为 `820x500`。

控制器在 820 宽度下仍然把所有运输按钮、速度、循环、随机、静音、音量和当前媒体名放在一行。截图验证没有发生文字覆盖或控件重叠。

### 4.3 样式

`configureStyle()` 使用 Qt Style Sheet 设置：

- 深色背景。
- 菜单、列表、按钮、滑块、状态栏配色。
- 主播放按钮使用橙色强调。
- 列表选中项使用暖色。
- 圆角控制在 3 至 4 像素，避免弹窗和工具按钮显得过度装饰。

设置样式表不会改变播放内核，只影响绘制。

`configureStyle()` 根据 `darkTheme_` 在两套完整 QSS 之间选择。`applyTheme()` 重新应用样式并调用 `updatePlaylistButtons()`，因为列表项前景色不是由 QSS 控制，而是按当前主题手动设置。主题状态写入 `ui/theme`，`Tools > Light Theme` 是唯一的切换入口。

---

## 5. `FFmpegPlayer` 对外接口

`FFmpegPlayer` 的公开方法可以分成五组。

### 5.1 媒体控制

```cpp
void open(const QString& target);
void play();
void pause();
void togglePause();
void stop();
```

`open()` 负责完整打开新媒体。

`play()` 有两个含义：

- 当前状态是 `Paused`：恢复媒体时钟和音频设备。
- 当前状态是 `Stopped`、`Ended`、`Idle`：重新打开当前目标。

`pause()` 只对 `Playing` 状态生效。

`togglePause()` 是播放器主按钮使用的统一入口。

`setPosition()` 在 `Playing`/`Paused` 下走正常 seek；在 `Ended` 下则以“在目标位置重新打开媒体、保持 Paused”的方式处理，避免播放结束后进度条失效。

### 5.2 位置和速度

```cpp
void setPosition(qint64 milliseconds);
void seekRelative(qint64 deltaMilliseconds);
void setPlaybackRate(double rate);
```

`setPosition()` 不在主线程里直接调用 `av_seek_frame`，因为那会和 `av_read_frame` 以及解码器状态竞争。它只记录请求，让 controlThread 在自己的循环中执行。

### 5.3 音频控制

```cpp
void setVolume(int volume);
void setMuted(bool muted);
```

音量范围为 0 至 125。100 以上是软件增益，可能产生削波，因此 `applyVolume()` 把每个 sample 限制在 `[-32768, 32767]`。

### 5.4 单帧控制

```cpp
void stepFrame();
```

`stepFrame()` 仅在暂停且有视频时生效。它不直接解码，而是给视频线程增加一次“允许发布下一帧”的请求。

### 5.5 查询

```cpp
State state() const;
qint64 position() const;
qint64 duration() const;
double playbackRate() const;
int volume() const;
bool isMuted() const;
bool hasAudio() const;
bool hasVideo() const;
double videoFrameRate() const;
QString currentTarget() const;
QString mediaInfo() const;
QString lastError() const;
```

大多数高频状态用 `std::atomic` 保存，UI 每 100 ms 读取一次，不需要加锁。

字符串状态由互斥锁保护：

- `targetMutex_` 保护当前媒体路径。
- `infoMutex_` 保护媒体信息文本。
- `errorMutex_` 保护最近错误。

---

## 6. 播放状态机

状态定义为：

```cpp
enum class State {
    Idle,
    Opening,
    Playing,
    Paused,
    Stopped,
    Ended,
    Error
};
```

正常路径：

```text
Idle
  │ open()
  ▼
Opening
  │ 打开输入、流和编解码器成功
  ▼
Playing ──pause()──► Paused
  │                    │
  │                    └──play()──► Playing
  │
  ├──stop()──► Stopped
  │               │
  │               └──play()──► 重新 open()
  │
  └──读到 EOF、两个输出线程结束──► Ended
                                      │
                                      └──play()──► 重新 open()

任何打开/读取错误 ──► Error
```

`setState()` 做两件事：

1. 用原子变量保存状态。
2. 发出 `stateChanged` 信号。

它不直接操作 UI，保持播放内核与界面解耦。

---

## 7. 打开媒体的完整流程

`open(target)` 先停止旧会话，再清理状态：

```text
stopThreads()
abort_ = false
paused_ = false
seekRequestMs_ = -1
seekPending_ = false
videoStepCount_ = 0
positionMs_ = 0
durationMs_ = 0
hasAudio_ = false
hasVideo_ = false
target_ = target
info_.clear()
lastError_.clear()
state = Opening
创建 MediaClock(0)
启动 controlThread_ = run(target)
```

为什么要先 `stopThreads()`：

- 旧控制线程可能还在 `av_read_frame`。
- 旧音频线程可能还在写 QAudioSink。
- 旧视频线程可能还把帧交给画布。
- 先关闭旧线程，才能安全复用音频/视频解码器指针和队列。

---

## 8. `run()`：解封装和初始化

### 8.1 本地路径编码

```cpp
const QByteArray encodedTarget = target.toUtf8();
avformat_open_input(&format, encodedTarget.constData(), nullptr, nullptr);
```

FFmpeg 3.4 的 Win32 文件协议能接受 UTF-8 路径，并在内部处理系统文件名转换。这里不能使用 `QString::toLocal8Bit()` 后假定所有系统代码页一致。

### 8.2 打开容器和流信息

```cpp
avformat_open_input(...)
avformat_find_stream_info(...)
```

第一步识别容器、建立 AVFormatContext。

第二步读取足够的头信息，填充每个 AVStream 的编码参数、时间基、时长和帧率。

### 8.3 选择最佳流

```cpp
av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
```

FFmpeg 会综合分辨率、码率、默认流标记、语言等因素推荐流。当前程序不提供多音轨或多字幕轨选择。

### 8.4 视频解码器

```cpp
AVCodecContext* codec = format->streams[videoIndex]->codec;
const AVCodec* decoder = avcodec_find_decoder(codec->codec_id);
avcodec_open2(codec, decoder, nullptr);
```

这是 FFmpeg 3.4 的旧式 `AVStream::codec` API。较新的 FFmpeg 通常使用 `avcodec_parameters_to_context()` 和独立分配的 AVCodecContext。这里保留旧 API 是为了跟工程自带 SDK 完全匹配。

打开成功后记录：

- 流索引 `index`。
- 流时间基 `time_base`。
- 流起始时间 `start_time`。
- 解码器上下文指针。
- 视频宽度、高度和平均帧率。

### 8.5 音频解码器

音频流程与视频类似，还额外记录：

- 采样率。
- 声道数。

### 8.6 没有可播放流

如果视频和音频都没有成功打开：

1. 设置错误文本。
2. 关闭已打开的 codec。
3. 关闭 format。
4. 发出 `errorOccurred`。
5. 状态改为 `Error`。

### 8.7 时长计算

优先使用容器时长：

```cpp
format->duration * 1000 / AV_TIME_BASE
```

容器没有有效时长时，遍历每个流的 duration，用各自 time_base 换算成毫秒。

### 8.8 媒体信息

`mediaInfo()` 字符串包含：

- 容器名。
- 视频 codec、宽高、fps、像素格式、码率。
- 音频 codec、采样率、声道数、码率。
- 时长、整体码率、流数量。
- 可用的 `title` / `artist` 元数据。

`MainWindow::showMediaInfo()` 把这段文本放进只读 `QPlainTextEdit` 对话框，附带当前来源路径和本地文件大小，并提供“复制到剪贴板”。

---

## 9. 包队列：解封装与解码的解耦

### 9.1 为什么需要队列

`av_read_frame` 的顺序是容器中的交错顺序，而音频和视频解码速度不同。如果主循环自己解码再等待输出，网络抖动和音频阻塞会把视频读取也拖死。

包队列让 controlThread 只负责读取和分发：

```text
AVPacket(video) ──► videoQueue_ ──► videoThread_
AVPacket(audio) ──► audioQueue_ ──► audioThread_
```

音频容量为 48 包，视频容量为 24 包。容量不宜太大，否则 seek 时要清掉太多旧数据。

### 9.2 队列状态

`PacketQueue` 有三个布尔标志：

- `finished_`：生产者已到 EOF。
- `aborted_`：停止/替换媒体。
- `blocked_`：正在 seek，希望生产者暂停入队。

还有两个条件变量：

- `changed_`：消费者等待“有包、结束或中止”。
- `wake_`：生产者等待“队列有空位、解除阻塞或中止”。

### 9.3 `push()`

生产者调用：

```cpp
bool push(AVPacket* packet)
```

等待条件：

```text
aborted_ || blocked_ || finished_ || packets_.size() < capacity_
```

如果等待过程中被 abort 或 blocked，函数会负责释放传入的 packet。

这样 AVPacket 的所有权很清楚：

- 调用 `push()` 前：controlThread 拥有。
- `push()` 成功后：PacketQueue 拥有。
- `push()` 返回 false：`push()` 已经释放。
- `pop()` 后：解码线程拥有。

### 9.4 `pop()`

消费者最多等待 50 ms，允许线程定期检查 `abort_` 和 `paused_`。

队列为空并且 `finished_` 为真时，返回 nullptr；消费者会继续 receive，以排空解码器内部可能还在的帧。

### 9.5 `clear()` 和 `finish()`

- `clear()` 删除全部旧包，主要用于 seek。
- `finish()` 表示生产结束，不再允许 push。
- `abort()` 表示程序主动停止。

---

## 10. 读取包和流分发

`readPackets()` 位于 controlThread。

每次循环：

1. 原子交换 `seekRequestMs_`。
2. 如果存在 seek，调用 `handleSeekLocked()`。
3. 分配 AVPacket。
4. 调用 `av_read_frame()`。
5. 音频包送音频队列。
6. 视频包送视频队列。
7. 其他流包立即释放。

### 10.1 EAGAIN

某些网络或设备输入可能暂时没有数据。程序释放临时包，休眠 2 ms 后继续。

### 10.2 读到 EOF

`readPackets()` 退出主循环。

### 10.3 解码器尾部帧

编码器可能把延迟帧藏在解码器里。到 EOF 后，程序为音频和视频队列各放一个空 AVPacket，再调用 `finish()`：

```cpp
AVPacket* drainPacket = av_packet_alloc(); // 没有数据
queue->push(drainPacket);
queue->finish();
```

`avcodec_send_packet(codec, null_packet)` 是 FFmpeg 官方推荐的 decoder drain 方式。解码线程收到后会持续 `receive_frame`，直到得到 `AVERROR_EOF`。

### 10.4 控制线程何时结束

controlThread 在 `readPackets()` 返回后：

1. 发送 flush 包。
2. 等待 audioThread join。
3. 等待 videoThread join。
4. 关闭 codec。
5. 关闭 AVFormatContext。
6. 清空队列。
7. 如果没有 abort，则状态改为 Ended，并发出 playbackFinished。

---

## 11. 解码循环的标准顺序

音频和视频解码都遵循同一套 FFmpeg send/receive 协议：

```text
1. 先 receive_frame
2. 如果 receive == 0，得到一帧，处理它
3. 如果 receive == EAGAIN，需要更多 packet
4. 从队列拿 packet
5. send_packet(packet)
6. 下一轮继续 receive_frame
7. 如果 send_packet 收到空 packet 或读到 EOF，继续 receive 到 AVERROR_EOF
```

为什么不“先 send 一包，再 receive”：

- 一个 packet 可能产生多个 frame。
- 解码器可能暂时不需要 packet。
- receive 返回 EAGAIN 才说明输出队列为空。
- receive 返回帧时，不应该再强行塞入 packet。

当前实现严格采用 receive-first 顺序。

如果 `avcodec_send_packet()` 或 `avcodec_receive_frame()` 返回不可恢复错误，循环会丢弃当前输入继续工作，防止 UI 永久卡住。实际产品可以进一步把这些错误上报到界面上。

---

## 12. 媒体时钟

### 12.1 为什么不用 QTimer 当播放时钟

音视频同步需要一个持续、单调、可暂停、可倍速的媒体时间。QTimer 受事件循环调度影响，窗口拖动或系统繁忙时可能抖动。

`MediaClock` 使用 `std::chrono::steady_clock`：

```text
mediaTime = mediaMsAtReset + (now - wallAtReset) * rate
```

`steady_clock` 不会因为系统时间修改而倒退。

### 12.2 暂停

暂停时先把当前已经流逝的媒体时间结算进 `mediaMs_`，再把 `paused_` 设为 true。

恢复时把 wall reset 时间更新到现在，然后继续以新的起点计算。

### 12.3 倍速

调用 `setRate(rate)` 前先把当前速度下已经流逝的时间结算，再用新 rate 继续。

例如：

```text
0 ms 开始 1x
1000 ms 时切到 2x

媒体位置仍然是 1000 ms
墙钟再过 500 ms 后，媒体位置约 2000 ms
```

### 12.4 线程安全

clock 的 mutex 保护四个字段：

- `mediaMs_`
- `wallAtResetMs_`
- `rate_`
- `paused_`

音频线程、视频线程和主线程都可能读取 clock，所以不能省略锁。

---

## 13. 音视频同步

### 13.1 `waitUntil(mediaTime)`

视频线程在发布一帧前调用：

```cpp
waitUntil(frameMs, true);
```

该函数先按“媒体时间差 ÷ 倍速”换算出需要等待的墙钟时间，再以 1 至 10 ms 为上限分段睡眠，每次醒来重新检查：

1. 是否 abort。
2. 是否 seek。
3. 是否暂停。
4. 当前媒体时钟是否已经到达目标帧时间。

到达后才 `return true`。旧实现固定睡 2 至 20 ms，高倍速时等待时间超过媒体帧间隔，视频会被拖慢并抖动；按墙钟折算后，2 倍速下每帧只睡一半时间。

### 13.2 音频为什么也能 waitUntil

音频在送入 QAudioSink 前也检查自身 PTS。它不会像视频那样显示队列中的旧帧，而是确保音频块与媒体时钟大致同步。

### 13.3 暂停时的单帧例外

这里有一个重要细节：

```cpp
if (allowFrameStep && videoStepCount_.load() > 0) {
    return false;
}
```

如果视频线程已经在一个未来帧的 `waitUntil()` 中暂停，仅增加单帧计数不会让它醒来。`allowFrameStep` 让视频等待器在单帧请求出现时立即返回，丢弃当前尚未显示的帧，然后下一次循环以 `stepping == true` 绕过等待，发布下一帧。

这个修复解决了冒烟测试中“单帧请求后位置不变化”的问题。

### 13.4 位置更新

`updatePosition()` 只允许位置变大：

```cpp
if (milliseconds > current) {
    positionMs_ = milliseconds;
}
```

音频帧和视频帧可能同时推进位置。这个单调规则防止视频线程的较旧 PTS 把音频已推进的位置拉回去。

不插值的 UI 每 100 ms 读取一次位置，因此进度条不需要每帧更新。

---

## 14. Seek

### 14.1 主线程只发请求

`setPosition()`：

1. 检查状态。
2. clamp 到 `[0, duration]`。
3. 写 `seekRequestMs_`。
4. 设置 `seekPending_ = true`。
5. 立即把 UI 位置设置到目标值。
6. 阻塞音频和视频队列。

### 14.2 控制线程执行 seek

`handleSeekLocked()`：

```cpp
av_seek_frame(formatContext_, -1, targetTimestamp, AVSEEK_FLAG_BACKWARD);
avcodec_flush_buffers(audioCodecContext_);
avcodec_flush_buffers(videoCodecContext_);
queue->clear();
queue->setBlocked(false);
clock_->reset(targetMs);
```

使用 `-1` 表示按全局时间基 seek。`AVSEEK_FLAG_BACKWARD` 会尽量定位到目标时间之前的关键帧。之后解码器会从关键帧开始解到目标附近。

### 14.3 解码器 flush

不 flush 解码器会产生“seek 后还显示 seek 前缓存帧”的问题。音频和视频 codec 都必须 flush。

### 14.4 队列阻塞和解除

`setPosition()` 将队列 blocked，正在 push 的控制线程会醒来并释放当前包，回到读取循环顶部处理 seek。

seek 完成后清空两个队列并解除 blocked。

### 14.5 旧帧过滤

`videoLoop()` 在 `waitUntil()` 之后再次检查：

```cpp
if (seekPending_) {
    av_frame_unref(frame);
    continue;
}
```

这样已经在 seek 前解码出来、但尚未显示的帧不会越过 seek 覆盖新画面。

如果媒体时钟已经领先该帧超过 150 ms（高倍速、系统卡顿或解码追不上时），这一帧会被直接丢弃而不是补显示，避免越播越落后；尚未到达显示时间的帧仍正常等待。

---

## 15. 视频解码与像素转换

### 15.1 解码帧

得到 AVFrame 后：

1. 计算 PTS，转换为毫秒。
2. 用媒体时钟等待。
3. 检查 seek。
4. 按像素格式和尺寸创建或复用 `SwsContext`。
5. 创建 BGRA QImage。
6. `sws_scale` 转换。
7. 交给 VideoCanvas。
8. 推进位置。
9. `av_frame_unref()`。

### 15.2 复用 SwsContext

`SwsContext` 保存滤波器、颜色空间和 CPU 优化状态。如果尺寸、格式没有改变，就复用现有 context，避免每帧初始化。

只有 width、height 或 pixel format 改变时才释放旧 context 并重建。

### 15.3 QImage 格式

`sws_scale` 的目标格式是：

```text
AV_PIX_FMT_BGRA -> QImage::Format_ARGB32
```

在 little-endian Windows 上，32 位 ARGB32 的内存布局是 B、G、R、A，和 FFmpeg 的 BGRA 输出一致。

QImage 每行可能有 padding，所以 destination stride 使用：

```cpp
image.bytesPerLine()
```

而不是简单使用 `width * 4`。

### 15.4 QImage 线程安全

QImage 使用隐式共享。视频线程创建 `image` 后调用：

```cpp
canvas->setFrame(image);
```

VideoCanvas 在互斥锁内赋值：

```cpp
frame_ = frame;
```

随后视频线程离开函数，不需要等待绘制。主线程拿到的是 QImage 共享数据的引用，真正的绘制由 Qt 主线程完成。

### 15.5 清理

视频线程退出时：

- 释放不是由解码器返回的临时 packet。
- `sws_freeContext`。
- `av_frame_free`。

---

## 16. 音频解码、重采样与输出

### 16.1 输出格式

首选 QAudioFormat：

```text
sampleRate = 源采样率（设备不支持时用设备首选采样率）
channelCount = 2
sampleFormat = Int16
```

倍速不改变输出采样率，因此播放过程中切倍速不会重建 QAudioSink。只有源格式或输出设备变化时才重新创建 sink。

如果设备不支持源采样率，回退到设备 preferredFormat，再确保 sampleFormat 为 Int16。若仍不支持，则报错。

### 16.2 为什么倍速改的是输入采样率

音频每帧的样本数固定，若直接把 44.1 kHz 源按原采样率塞给设备，设备会按原速播放。现在把 swresample 的输入采样率按倍速虚拟放大：2 倍速时声明输入为 88.2 kHz、输出仍为 44.1 kHz。同一源帧因此只产生一半的输出样本，设备按固定采样率播完这些样本只需一半时间，音频就真正加速了。

反过来提高输出采样率而样本数不变，只会让设备花更长时间播放更多样本（变慢），并不能加速——这是之前倍速音频“听不出变化、时钟却跑得快”的原因。

当前实现没有做变调补偿，所以高倍速会有“快放”音高变化。

### 16.3 swresample

`swr_alloc_set_opts` 配置：

- 输入声道布局。
- 输入 sample format。
- 输入 sample rate：`源采样率 × 倍速`（倍速只作用在输入侧）。
- 输出声道布局。
- 输出 S16。
- 输出 sample rate：固定的设备采样率。

倍速或源格式变化时会重建 swresample 上下文；上下文参数未变时复用。

`swr_init` 后，`swr_convert` 可以同时完成：

- 采样率转换。
- 声道布局转换。
- sample format 转成 16 位整数。

输出缓冲区大小用：

```cpp
swr_get_delay(resampler, swrInputRate) + frame->nb_samples
```

再按 `sinkRate / swrInputRate` 折算后 `av_rescale_rnd(..., AV_ROUND_UP)` 计算，避免输出被截断。延迟查询的基准速率必须与输入侧一致，否则高倍速下缓冲区会算大或算小。

### 16.4 QAudioSink

每个音频会话启动时：

1. 获取默认输出设备。
2. 创建 QAudioSink。
3. 设置 32768 字节缓冲。
4. `start()` 拿到 QIODevice。
5. 循环 `write()` 写入 PCM。
6. 如果写入为 0，睡 2 ms 后重试。

### 16.5 暂停和恢复

暂停时：

```cpp
sink->suspend();
```

恢复时：

```cpp
sink->resume();
```

同时媒体时钟也暂停，防止 UI 进度继续走。

### 16.6 音量和静音

`applyVolume()` 对每个 `qint16` sample 做：

```text
out = clamp(round(in * gain), -32768, 32767)
```

静音等价于 gain = 0。

### 16.7 位置更新

成功转换并写入一个音频帧后：

```cpp
updatePosition(frameMs + convertedSamplesToMilliseconds);
```

音频输出通常比视频更稳定，所以它是位置推进的主要来源之一。

---

## 17. 暂停和逐帧

### 17.1 暂停

`pause()`：

1. 只允许从 Playing 进入。
2. `paused_ = true`。
3. clock pause。
4. 状态改 Paused。

音频线程看到 paused 后 suspend QAudioSink。

视频线程看到 paused 且没有 step 请求时每 5 ms 休眠。

### 17.2 单帧

`stepFrame()`：

```cpp
if (!paused_ || !hasVideo_ || videoStepCount_ >= 8) {
    return;
}
videoStepCount_.fetch_add(1);
```

最多累积 8 次请求，防止用户长按 E 后一次醒来解码大量帧。

视频线程顶部：

```cpp
const bool stepping = paused_ && videoStepCount_ > 0;
```

如果 stepping：

- 不等待 clock。
- 正常 receive/send。
- 转换并显示一帧。
- 重置 clock 到新帧 PTS。
- 保持 paused。
- 将 step count 减一。

因此逐帧不会自动恢复播放。

### 17.3 UI 行为

`MainWindow::stepOneFrame()`：

- 无视频时提示。
- 正在播放时先 pause。
- 非 Paused 状态提示先暂停。
- 调用 player->stepFrame。

工具栏、播放菜单、`E` 快捷键和视频右键菜单使用同一个入口。

### 17.4 后退单帧

`MainWindow::stepPreviousFrame()` 不新增内核线程逻辑，而是复用暂停预览：

1. 无视频时提示。
2. 正在播放时先 `pause()`。
3. 用 `videoFrameRate()` 计算 `1000 / fps`，钳制到 `10..100 ms`。
4. `player_->setPosition(position - interval)`。

暂停状态下 seek 会设置 `previewPending_`，视频线程解码并在画布上显示目标位置帧后重新暂停时钟，因此用户看到的是上一帧而不是黑屏。

### 17.5 跳转时间和 A-B 循环

`goToTime()` 用 `QInputDialog` 读取文本，`parseTimeText()` 接受：

- 纯秒数：`90`、`1.5`
- `MM:SS`
- `HH:MM:SS`

解析为毫秒后调用 `player_->setPosition()`。

A-B 循环完全在界面层实现，不修改内核：

- `setRepeatPointA()` 记录当前位置；`setRepeatPointB()` 要求 B 比 A 至少晚 200 ms。
- `updateUi()` 每 100 ms 检查 `position >= repeatEndMs_` 且状态为 Playing，满足时 `setPosition(repeatStartMs_)`。
- `repeatJumping_` 锁存防止 seek 尚未生效时重复触发，位置回到 A 点附近后复位。
- 切换媒体时清空 A/B 点，避免跨文件误用。

---

## 18. 停止和退出

### 18.1 `stopThreads()`

顺序：

1. `abort_ = true`
2. 清除 seek request。
3. 清除单帧请求。
4. 恢复 clock（避免等待器永远睡眠）。
5. abort 音频和视频队列。
6. join controlThread。

controlThread 的 `run()` 在 `readPackets()` 返回后负责 join 音频和视频线程并关闭 FFmpeg 资源。

### 18.2 为什么只 join controlThread

子线程由 controlThread 在正常结束和 abort 两条路径上负责 join。主线程只 join controlThread，可以避免重复 join 和错误的等待顺序。

### 18.3 析构

`FFmpegPlayer::~FFmpegPlayer()`：

```cpp
stopThreads();
avformat_network_deinit();
```

`MainWindow::~MainWindow()`：

```cpp
player_->setVideoCanvas(nullptr);
player_->stop();
delete subtitleTrack_;
```

必须先把 VideoCanvas 指针清空，再停止播放器，避免视频线程在 MainWindow 析构过程中仍尝试访问画布。

`closeEvent()` 还会：

- 停止 100 ms UI timer。
- 保存设置。
- 停止播放器。
- 接受关闭事件。

---

## 19. `VideoCanvas`

### 19.1 帧缓存

成员：

```cpp
QImage frame_;
QString subtitleText_;
QString placeholder_;
```

由 `QMutex mutex_` 保护。

### 19.2 绘制

`paintEvent()`：

1. 复位重绘合并标记。
2. 在锁内复制一份 QImage 引用、字幕和占位文本。
3. 释放锁。
4. 填充深色背景。
5. 按 `FitMode` 调用 `targetRectFor()` 得到整数对齐的居中矩形。
6. 有视频帧时 `drawImage` 到该矩形，再叠加字幕。
7. 没有帧时绘制 placeholder。

缩放期间 `resizeEvent()` 会临时关闭 `SmoothPixmapTransform` 并整帧重绘（拖动窗口时用最近邻采样跟手，避免只重绘新暴露条带造成的边缘残影），停止缩放 120 ms 后再恢复平滑采样重绘一次。

QImage 的复制是隐式共享，因此锁只覆盖引用替换，不覆盖整个缩放和绘制，避免阻塞视频线程。

### 19.3 字幕绘制

字幕字体大小：

```text
std::clamp(width / 42 * subtitleScale, 14, 72)
```

`SubtitleStyle` 提供字体族、文字颜色、描边颜色/宽度、背景颜色/透明度，由 `paintEvent` 在锁内复制后传给 `drawSubtitle()`。绘制顺序：

1. 计算文本框，底部居中，最大宽度为视频显示宽度减 72。
2. 背景不透明度大于 0 时绘制圆角底板。
3. 描边宽度大于 0 时在 `±outlineWidth` 的偏移位置用描边颜色重复绘制文字。
4. 用正文颜色绘制最终文字。

字体为空时使用 `Segoe UI`，样式默认白字黑底、无描边。主窗口的字幕样式对话框实时调用 `setSubtitleStyle()`，取消时恢复原样式。

### 19.4 滚轮音量

`wheelEvent()` 把滚轮 delta 转成 steps：

```text
delta / 120
```

高精度触控板可能出现小于 120 的 delta，此时按方向至少算一档。

发送：

```cpp
volumeWheelSteps(steps)
```

MainWindow 每档调整 5%。

### 19.5 双击全屏

左键双击发出 `doubleClicked()`，MainWindow 连接它到 `toggleFullscreen()`。

### 19.6 右键菜单

`contextMenuEvent()` 发全局坐标给 MainWindow，让 QMenu 在正确屏幕位置弹出。

### 19.7 拖放

`dragEnterEvent` 接受 URL 列表。

`dropEvent` 只把本地文件 URL 转成路径；MainWindow 也支持 URL 文本拖入。

---

## 20. 字幕解析

### 20.1 编码

先尝试 UTF-8；若有错误，回退到系统本地编码：

```cpp
QStringDecoder utf8(QStringDecoder::Utf8);
...
QString::fromLocal8Bit(content)
```

支持 UTF-8 BOM。

### 20.2 通用清理

`cleanSubtitleText()` 删除：

- HTML 风格标签。
- ASS 花括号样式标签。

并转换：

- `\N` 和 `\n` 为换行。
- `\h` 为空格。

### 20.3 时间解析

`parseClock()` 支持：

- `HH:MM:SS.mmm`
- `MM:SS.mmm`
- 冒号或逗号作为毫秒分隔。

一位小数按十分之一秒，两位按百分之一秒，三位按毫秒。

### 20.4 SRT/VTT

按空行分块，搜索包含 `-->` 的时间行，后面的行作为文本。

每块需要：

```text
0 <= start < end
```

### 20.5 ASS/SSA

只处理 `[Events]` 段中的 `Dialogue:`。

字段：

```text
0 Layer
1 Start
2 End
3 Style
4 Name
5 MarginL
6 MarginR
7 MarginV
8 Effect
9+ Text
```

字幕文本本身可能包含逗号，所以从索引 9 到最后重新 join。

### 20.6 查询

`textAt(positionMs)` 使用 `std::upper_bound` 找到第一个 start 大于当前位置的 cue，再检查前一个 cue 是否仍在有效区间。

这是 O(log n) 查询，100 ms UI 刷新足够快。

### 20.7 自动加载

播放本地文件时，用 `QFileInfo::completeBaseName()` 去找同目录同名：

```text
movie.mp4
movie.srt
movie.ass
movie.vtt
```

顺序为 srt、ass、ssa、vtt，找到第一个就停止。

---

## 21. 播放列表

### 21.1 数据模型

```cpp
struct PlaylistEntry {
    QString target;
    QString title;
    bool isUrl;
};
```

同时存在：

- `QVector<PlaylistEntry> playlist_`：真正的数据。
- `QListWidget`：显示层。

列表 item 的 `Qt::UserRole` 保存目标路径，`UserRole + 1` 保存是否 URL，tooltip 显示完整路径。

### 21.2 打开模式

```cpp
enum class OpenMode { Replace, Append };
```

- Replace：清空旧列表，播放第一项新条目。
- Append：保留旧列表，只在原来没有当前项时开始播放。

### 21.3 文件夹展开

`expandPath()` 对目录递归遍历，只加入已知媒体扩展名，并按 locale-aware 路径排序。

### 21.4 M3U/M3U8

读取文本后：

- 跳过空行。
- `#EXTINF:` 记录下一项标题。
- 其他 `#` 行忽略。
- 普通行作为路径或 URL。
- 相对路径按 M3U 文件所在目录解析。

### 21.5 PLS

匹配：

```text
File1=...
Title1=...
```

按编号排序，没有标题时自动生成。

### 21.6 导出 M3U

输出 UTF-8：

```text
#EXTM3U
#EXTINF:-1,标题
目标
```

### 21.7 双击和 Enter

`itemActivated` 覆盖 Enter 和平台激活行为，`itemDoubleClicked` 覆盖鼠标双击。两者最终都调用 `playIndex()`。

### 21.8 Delete

删除 QAction 设置为 WindowShortcut，并加到主窗口 action。焦点在列表时 Delete 会删除选中项；即使焦点在其他控件上，只要在同一窗口也会触发。

### 21.9 循环和随机

`nextPlaylistIndex(forward)`：

- 空列表返回 -1。
- 随机模式从其他所有项目中等概率取一个。
- 普通模式取前后相邻项。
- 越界时只有 loop 打开才回绕。

### 21.10 搜索、排序和去重

搜索使用 `QListWidgetItem::setHidden()`，不重建列表，因此 `playlist_` 与列表行号始终一一对应，`playIndex(row)` 不需要额外映射：

- 同时匹配标题和完整路径，大小写不敏感。
- 过滤时计数标签显示 `可见数 / 总数 item(s)`。

排序和去重会重写 `playlist_`，再调用 `rebuildPlaylistView()`：

1. 记录当前媒体的 target。
2. 用 `std::stable_sort` 按标题（次级键路径）或路径（次级键标题）排序，使用 `QString::localeAwareCompare`。
3. 去重时本地路径按小写比较（Windows 不区分大小写），URL 保持原样。
4. 清空并重建 `QListWidget`，按 target 找回 `currentIndex_`，然后重新应用过滤文本。
5. 重排期间设置 `syncingPlaylistOrder_`，防止 `rowsMoved` 信号回写。

排序/去重只改变列表顺序，不会重新打开正在播放的媒体。

### 21.11 单文件续播

`resumePositions_` 是 `QHash<target, 毫秒>`：

- `rememberCurrentPosition()` 只在 Playing/Paused、位置大于 5 秒且距片尾超过 3 秒时记录；其它情况删除该文件的记录。
- `playIndex()` 在没有显式起始位置且不是“启动恢复”时查询这个表，命中后把 `pendingResumeMs_` 传给 `mediaOpened`，由 `MainWindow` 弹出 OSD 提示。
- `settings.ini` 的 `resume/items` 数组保存最多 100 条；超出时淘汰最早插入的条目。
- `mediaOpened` 中若记录位置距片尾不足 1 秒，归零从片头播放，避免打开就结束。

---

## 22. 菜单、快捷键和上下文菜单

菜单包括：

- File
- Playback
- Playlist
- Audio
- Video
- Subtitle
- Tools
- Language
- Help

`Language` 菜单固定显示 `English` 与 `简体中文` 两个互斥勾选项，设置 `noTranslate` 属性避免被翻译，点击后调用 `switchLanguage()`。

关键设计是：菜单 QAction、工具栏按钮和视频右键菜单都调用同一个 MainWindow 方法，而不是复制播放逻辑。

例如单帧最终都进入：

```cpp
MainWindow::stepOneFrame()
```

这样快捷键、按钮和右键菜单的行为不会分叉。

`Help > Keyboard Shortcuts`（`F1`）用 `showShortcuts()` 构造一个只读 `QPlainTextEdit`，按播放、A-B、字幕、播放列表、视频、文件分组列出按键。分组标题通过 `UiLanguageSupport::dictionaryText()` 翻译，条目描述使用普通 `tr()`。

---

## 23. 全屏

进入全屏：

1. 记录是否最大化。
2. 隐藏 menuBar、statusBar 和 playlistPanel。
3. 把 controlBar 从布局中移除，重新挂到 centralWidget，固定高度。
4. 设置 fullscreen 标志并 `showFullScreen()`。
5. 记录控制条为叠加模式，手动放在窗口底部。
6. 鼠标移动时显示控制条，2.5 秒无操作后隐藏并隐藏光标。

退出：

1. 停止自动隐藏计时器。
2. `showNormal()`，必要时恢复最大化。
3. 把 controlBar 放回布局，恢复菜单、状态栏和播放列表面板。
4. 恢复鼠标指针。

`Esc`、`F11`、`F` 和视频双击都进入同一个 `toggleFullscreen()`。

`resizeEvent` 在全屏叠加模式下重新计算控制条位置，避免窗口尺寸变化后控制条留在旧位置。

---

## 24. 置顶

切换 `Qt::WindowStaysOnTopHint`：

1. 记录 visible 状态。
2. 修改 window flag。
3. 如果之前可见，重新 show。

修改 Windows flag 会导致原生窗口重建，因此必须保存可见状态并重新显示。

### 24.1 迷你播放器

`setMiniMode(true)`：

1. 如果正在全屏，先退出全屏。
2. 保存 `saveGeometry()` 和当前置顶状态。
3. 隐藏 `menuBar`、`statusBar` 和 `playlistPanel_`。
4. 如果未置顶，设置 `Qt::WindowStaysOnTopHint` 并重新 `show()`。
5. 最小尺寸改为 400×260，窗口缩到 560×340。

退出时恢复菜单、状态栏、播放列表、最小尺寸与保存的几何；只有进入前没有置顶时才清除置顶标志，避免覆盖用户原有的置顶选择。

`saveSettings()` 在迷你模式下保存的是进入前的几何和置顶状态，所以下次启动仍是正常窗口大小。`toggleFullscreen()` 进入全屏前也会先退出迷你模式；`Esc` 的优先级是全屏 → 迷你。

---

## 25. 截图

`VideoCanvas::snapshot(bool withSubtitles)` 在锁内返回旋转后帧的副本。

`MainWindow::takeSnapshot()`：

1. 没有帧则提示。
2. 根据 `snapshotWithSubtitles_` 决定是否在离屏 QImage 上绘制字幕。
3. 默认保存到 Pictures。
4. 创建 `FFmpeg Qt Player` 子目录。
5. 文件名带年月日和毫秒。
6. QImage 保存为 PNG。
7. 在状态栏显示完整保存路径。

截图保存的是画面内容，不包含菜单、控制条等 UI。

---

## 26. 设置持久化

`QStandardPaths::AppDataLocation` 根据 application/org 名生成路径，最终文件为：

```text
%APPDATA%\FFmpegQtPlayer\FFmpeg Qt Player\settings.ini
```

使用 `QSettings(..., QSettings::IniFormat)` 直接读写，不经过注册表。`loadSettings()` 在构建 UI 后调用，所以控件和 action 已经存在；`saveSettings()` 在 `closeEvent`、单实例打开、播放结束等时机调用，最后 `sync()` 落盘。

键按功能分组，读取时统一做范围钳制，避免手改配置文件后出现非法值：

| 分组/键 | 类型 | 默认值 | 用途 |
| --- | --- | --- | --- |
| `playback/volume` | int | `100` | 音量，钳制到 `0..125` |
| `playback/muted` | bool | `false` | 静音状态 |
| `playback/rate` | double | `1.0` | 倍速，钳制到 `0.25..4.0`，加载时按 `findData` 匹配下拉项 |
| `playback/loop` | bool | `false` | 列表循环 |
| `playback/shuffle` | bool | `false` | 随机播放 |
| `view/playlist` | bool | `true` | 播放列表面板是否显示 |
| `window/topmost` | bool | `false` | 窗口置顶，加载为 true 时重新设置窗口 flag 并 `show()` |
| `window/geometry` | QByteArray | 空 | `saveGeometry()` 原始字节，`restoreGeometry()` 恢复 |
| `ui/language` | QString | 系统语言 | 界面语言，`en` 或 `zh`；空值按 `QLocale::system()` 判断 |
| `ui/theme` | QString | `dark` | 主题，`dark` 或 `light` |
| `video/fitMode` | int | `0` | 画面适配模式，钳制到 `0..5`，同步勾选 fit mode 菜单 |
| `subtitle/visible` | bool | `true` | 字幕总开关 |
| `subtitle/delay` | int | `0` | 字幕延迟毫秒，可为负 |
| `subtitle/scale` | double | `1.0` | 字号比例，钳制到 `0.5..2.5` |
| `subtitle/margin` | double | `0.055` | 底边距占画面高度比例，钳制到 `0.02..0.45` |
| `subtitle/font` | QString | 空 | 字幕字体族；空表示 `Segoe UI` |
| `subtitle/textColor` | QString | `#ffffff` | 文字颜色名 |
| `subtitle/outlineColor` | QString | `#000000` | 描边颜色名 |
| `subtitle/outlineWidth` | int | `0` | 描边宽度，钳制到 `0..8` |
| `subtitle/backgroundColor` | QString | `#000000` | 字幕底板颜色名 |
| `subtitle/backgroundOpacity` | int | `180` | 底板透明度，钳制到 `0..255` |
| `snapshot/subtitles` | bool | `false` | 截图是否叠加字幕 |
| `recent/files` | QStringList | 空 | 最近打开文件，菜单据此重建 |
| `resume/items` | array | 空 | 单文件续播位置，每项含 `target`、`position`，最多 100 条 |
| `session/items` | array | 空 | 播放列表，每项含 `target`、`title`、`isUrl` |
| `session/current` | int | `-1` | 上次当前项索引，越界时归零无效 |
| `session/position` | qint64 | `0` | 上次播放位置毫秒 |

会话恢复流程：

1. `beginReadArray("session/items")` 逐项重建播放列表，跳过 `target` 为空的记录，标题缺失时用 `makeTitle()` 补全。
2. 读取 `session/current` 和 `session/position`；索引非法或超出列表范围时按“无会话”处理。
3. 启动后若目标仍存在，调用 `playIndex(index, position, true)`：打开媒体、跳到保存位置并以暂停状态显示首帧，不自动出声；文件已不存在时只选中列表项。
4. 关闭时若播放器处于 `Idle/Stopped/Ended/Error`，保存位置写 `0`，避免下次从片尾恢复。

`resume/items` 与 `session/*` 相互独立：会话恢复负责“上次在播什么”，续播表负责“每个文件看到哪里”。`saveSettings()` 先调用 `rememberCurrentPosition()` 更新续播表，再序列化；`playIndex()` 只有在没有显式起始位置且不是启动恢复时才应用续播位置。

`recent/files` 在每次成功打开媒体时由 `addRecentTarget()` 前插并去重，最多保留 12 条；`Recent Files` 菜单在 `aboutToShow` 时重建，并提供清空入口。

恢复窗口 geometry 可能让窗口出现在之前屏幕的位置。如果显卡配置变化导致窗口落点不可见，需要额外做可见屏幕检查；目前只保证 initial size 按主屏可用区域钳制。

---

## 27. 错误处理

错误来源：

- `avformat_open_input`
- `avformat_find_stream_info`
- 找不到可用音视频流
- `av_read_frame`
- 没有音频设备
- QAudioFormat 不支持
- swresample 初始化失败
- seek 失败
- 字幕读取/解析失败
- 截图保存失败
- 播放列表保存失败

播放器内核通过 `errorOccurred(QString)` 发信号，MainWindow 使用 `Qt::QueuedConnection` 显示 QMessageBox，确保弹窗发生在主线程。

`lastError()` 由 errorMutex 保护。`updateStatus()` 在 Error 状态下直接返回，避免定时刷新把错误文本覆盖成普通状态。

---

## 28. 所有权表

| 对象 | 创建者 | 所有者/释放 |
| --- | --- | --- |
| QApplication | main 栈 | main 退出 |
| MainWindow | main 栈 | main 退出 |
| FFmpegPlayer | MainWindow | QObject parent |
| VideoCanvas | MainWindow | Qt parent + splitter |
| SubtitleTrack | MainWindow | MainWindow 析构手动 delete |
| AVFormatContext | avformat_open_input | run() 关闭 |
| AVCodecContext | AVStream::codec | avformat_close_input 负责释放 |
| AVPacket（读取） | readPackets | 队列或解码线程释放 |
| AVFrame | 解码循环 | av_frame_free |
| SwsContext | videoLoop | sws_freeContext |
| SwrContext | audioLoop | swr_free |
| QAudioSink | audioLoop | closeOutput 中 delete |
| QImage 帧 | videoLoop | 共享引用，最终自动释放 |

所有权原则：

- FFmpeg 对象不使用 C++ `new` 和 `delete`，使用配套的 `av_*_free`。
- Qt parent 管 QObject 控件。
- `SubtitleTrack` 不是 QObject，所以显式删除。
- 队列内 AVPacket 必须在 abort/clear/析构时逐个释放。

---

## 29. 锁和竞态

### 29.1 `queueMutex_`

保护 `audioQueue_` 和 `videoQueue_` 指针的替换/访问。

### 29.2 `codecMutex_`

保护对 AVCodecContext 的 receive/send、flush 和 close。防止 seek 在主线程 flush 时，解码线程同时 receive。

### 29.3 `targetMutex_`

保护 QString target。

### 29.4 `infoMutex_` / `errorMutex_`

保护字符串读写。

### 29.5 `VideoCanvas::mutex_`

保护 QImage 和文本引用。

### 29.6 原子变量

用于简单状态和数字：

- state
- abort
- paused
- hasAudio / hasVideo
- position / duration
- seek request
- step count
- rate / volume / muted
- canvas pointer

原子变量不能替代复杂对象锁。例如 AVCodecContext 不能用原子指针保护，因为解码过程不是单条原子指令。

---

## 30. 典型时序

### 30.1 打开本地视频

```text
MainWindow::playIndex
  ├─ clearFrame
  ├─ player->open
  │    ├─ stopThreads
  │    ├─ state=Opening
  │    └─ controlThread.run
  │         ├─ open input
  │         ├─ find stream info
  │         ├─ open video codec
  │         ├─ open audio codec
  │         ├─ create queues
  │         ├─ emit mediaOpened
  │         ├─ start videoThread
  │         ├─ start audioThread
  │         ├─ state=Playing
  │         ├─ read packets
  │         └─ EOF drain/join/close
  └─ load sidecar subtitle
```

### 30.2 暂停

```text
MainWindow button
  └─ FFmpegPlayer::pause
       ├─ paused_=true
       ├─ clock.pause
       └─ state=Paused

audioThread
  └─ sink->suspend()

videoThread
  └─ sleep 5 ms loop
```

### 30.3 单帧

```text
MainWindow::stepOneFrame
  ├─ 如果 Playing，先 pause
  └─ player->stepFrame
       └─ videoStepCount++

videoThread
  ├─ stepping=true
  ├─ receive frame
  ├─ sws_scale
  ├─ canvas.setFrame
  ├─ clock.reset(frame PTS)
  └─ videoStepCount--
```

### 30.4 Seek

```text
MainWindow slider release
  └─ setPosition
       ├─ seekRequestMs=target
       ├─ seekPending=true
       └─ queues blocked

controlThread
  ├─ handleSeekLocked
  ├─ av_seek_frame
  ├─ avcodec_flush_buffers
  ├─ queue.clear
  ├─ clock.reset
  └─ queue.unblock

videoThread
  └─ discard frames while seekPending
```

### 30.5 EOF

```text
av_read_frame -> EOF
  ├─ push empty flush packet
  ├─ queue.finish
  ├─ audio thread drains -> AVERROR_EOF
  ├─ video thread drains -> AVERROR_EOF
  ├─ join both
  ├─ close codecs/input
  ├─ state=Ended
  └─ emit playbackFinished

MainWindow
  ├─ sequential: open next playlist item
  ├─ shuffle: open random item
  ├─ loop: wrap
  └─ otherwise: remain Ended
```

---

## 31. 关键函数阅读顺序

第一次阅读建议：

1. `MainWindow::MainWindow`
2. `MainWindow::buildInterface`
3. `MainWindow::buildMenus`
4. `MainWindow::connectPlayer`
5. `FFmpegPlayer::open`
6. `FFmpegPlayer::run`
7. `FFmpegPlayer::readPackets`
8. `FFmpegPlayer::audioLoop`
9. `FFmpegPlayer::videoLoop`
10. `FFmpegPlayer::waitUntil`
11. `FFmpegPlayer::handleSeekLocked`
12. `VideoCanvas::paintEvent`
13. `SubtitleTrack::parseSrt` / `parseAss`
14. `MainWindow::playbackFinished`
15. `MainWindow::closeEvent`

第二次阅读建议专注于数据所有权和线程边界：

- 每个 AVPacket 在哪里释放。
- 每个 AVFrame 在哪里 unref/free。
- 哪些操作必须拿 codecMutex。
- 哪些操作必须回到 Qt 主线程。
- 停止时哪个线程先退出。

---

## 32. 构建系统

### 32.1 CMake

`find_package(Qt6 6.5 ... Core Gui Widgets Multimedia)` 找到 Qt。

`FFMPEG_ROOT` 固定在源码目录下的 `ffmpeg3.4-x64-debug`。

链接：

```text
avformat
avcodec
avutil
swresample
swscale
```

这五个库对应：

- 解封装。
- 编解码。
- 基础工具/时间基/像素格式。
- 音频重采样。
- 视频像素转换。

### 32.2 运行时 DLL

FFmpeg 主 DLL：

```text
avcodec-57.dll
avdevice-57.dll
avfilter-6.dll
avformat-57.dll
avutil-55.dll
swresample-2.dll
swscale-4.dll
```

这个 SDK 由 MinGW 构建，主 DLL 还依赖：

```text
libbz2-1.dll
libiconv-2.dll
liblzma-5.dll
libwinpthread-1.dll
zlib1.dll
```

缺少这些“传递依赖”时，Windows 加载器会在进入 main 前直接返回 `0xC0000135`，看起来像“程序闪退”。CMake POST_BUILD 和 `.pro` 都明确复制这些 DLL。

### 32.3 windeployqt

`windeployqt` 复制：

- Qt Core/Gui/Widgets/Multimedia/Network/Svg。
- qwindows 平台插件。
- multimedia 插件。
- imageformats、styles、tls 等插件。
- Qt Multimedia 插件自身使用的 FFmpeg 6.x DLL。

Qt Multimedia 插件使用的 `avcodec-61.dll` 与播放器使用的 `avcodec-57.dll` 文件名不同，可以在同一个目录共存。播放器核心只调用 57 系列。

另外，CMake 在 Qt 安装目录的 `translations/` 下查找 `qtbase_zh_CN.qm`，复制到输出目录的 `translations/`。中文模式只加载这一个 Qt 翻译文件，用于标准对话框按钮；`windeployqt` 使用 `--no-translations`，不会覆盖它。

### 32.4 qmake

`FFmpegQtPlayer.pro` 是备用构建入口，CMake 是主入口。

---

## 33. `build.ps1`

脚本步骤：

1. 定位 Qt 的 `Qt6Config.cmake`。
2. 定位 VsDevCmd。
3. 定位 CMake 和 Ninja。
4. 可选安全清理 build 子目录。
5. 在 Visual Studio Developer Command Prompt 环境中调用 CMake。
6. 失败时抛出退出码。
7. 检查 exe 是否生成。

`-Clean` 会检查解析后的 build 路径仍然位于项目目录内，防止误删外部目录。

---

## 34. 测试方法

### 34.1 基础播放

```powershell
.\build\Debug\FFmpegQtPlayer.exe .\sample.mp4
```

检查：

- 出现测试图。
- 时间从 0 前进到 8 秒。
- 播放结束状态正确。

### 34.2 暂停

按 Space：

- 图标变回播放。
- 时间停止。
- 音频停止。
- 再按 Space 继续。

### 34.3 单帧

播放时按 E：

1. 自动暂停。
2. 显示下一帧。
3. 时间小幅前进。
4. 状态仍为 Paused。

### 34.4 Seek

拖动进度条到中间：

- 画面更新。
- 时间接近目标。
- 不继续播放旧位置。

点击进度条槽的任意位置也应立即跳转，不必先按到滑块。

播放结束（Ended）后拖动进度条：

- 媒体在目标位置重新打开。
- 状态为 Paused。
- 再按 Space 可继续播放。

### 34.5 倍速

选择 2x：

- 状态栏显示 2x。
- 8 秒媒体大约 4 秒播完。
- 视频播放平滑，没有明显卡顿或越来越落后。
- 音频保持连续，但音调升高。

### 34.6 音量

- 鼠标滚轮在视频上滚动每档 5%。
- 菜单 Up/Down 调整。
- M 静音和恢复。
- 100% 以上是软件增益。

### 34.7 播放列表

- 拖入两个文件。
- 双击或 Enter 切换。
- Delete 删除选中项。
- 保存 M3U。
- 重新打开 M3U。
- 测试 PLS 相对路径。

### 34.8 字幕

准备：

```text
movie.mp4
movie.srt
```

打开 movie.mp4 后应自动显示字幕。手动加载 VTT、ASS 也要验证。

### 34.9 全屏

- 双击视频。
- F11/F。
- Esc 退出。
- 最大化进入全屏后，退出时恢复最大化。
- 进入全屏后控制条应浮在画面底部，鼠标静止约 2.5 秒后连同光标一起隐藏。
- 移动鼠标后控制条重新出现；播放列表、菜单栏、状态栏在全屏隐藏。
- 退出全屏后控制条回到主布局，播放列表显示状态恢复。

### 34.10 关闭

在播放时关闭窗口。程序应先停止播放线程再销毁窗口，不应死锁或崩溃。

### 34.11 中英文切换

在播放状态下依次执行 `Language > English`、`Language > 简体中文`，每步检查：

- 窗口标题在 `FFmpeg Qt Player` 与 `FFmpeg Qt 播放器` 之间切换。
- 菜单栏、工具栏提示、循环/随机按钮、状态栏信息跟随切换。
- 语言菜单自身始终显示 `English` 和 `简体中文`，不随界面语言变化。
- 打开一个 `QMessageBox`，确定/取消按钮在中文模式下显示中文。
- 切换后关闭程序，`settings.ini` 中 `ui/language` 写入对应代码；再次启动应恢复上次语言。

自动验证用界面级 harness 完成：打开 `sample.mp4`，触发两个语言 action 各两次，断言标题、菜单、提示、状态栏和对话框按钮文本，并检查语言持久化后重新启动为中文，全部通过。

---

## 35. 已发现并修复的问题

### 35.1 Windows 中文路径

早期使用 `QFile::encodeName()` 可能受本地编码影响。现在使用 `target.toUtf8()`，由 FFmpeg 的 win32 文件协议处理 UTF-8 到宽字符。

### 35.2 解码器 send/receive 顺序

早期先 send packet，再 receive frame，容易在多个输出帧或 EAGAIN 时阻塞或丢帧。现在固定 receive-first。

### 35.3 EOF 尾帧

只读到 EOF 而不发送空 flush packet 会丢掉解码器缓存的尾帧。现在音视频队列各发送空包，再 drain 到 AVERROR_EOF。

### 35.4 暂停时单帧无法唤醒

视频线程可能卡在暂停状态的 waitUntil。现在 waitUntil 在视频线程收到 step 请求时返回，随后进入 stepping 路径。

### 35.5 缺少 MinGW 传递依赖

只复制 av*.dll/sw*.dll 不够。补入 bz2、iconv、lzma、pthread、zlib 后，程序才能在干净环境中启动。

### 35.6 错误文本被定时刷新覆盖

`updateStatus()` 在 Error 状态直接返回，保留真实错误。

### 35.7 音轨文件占位文字

无视频只有音频时，画布显示 “Audio playback”，而不是永远显示“解码视频”。

### 35.8 播放列表交互

补齐了双击、Enter、Delete 和视频区右键菜单。

---

## 36. 已知限制和下一步扩展

### 36.1 多音轨切换

已经实现音轨枚举和切换，入口是 `FFmpegPlayer::audioTracks()` 和 `selectAudioStream()`。

实现方式：

- `run()` 打开媒体后枚举所有 `AVMEDIA_TYPE_AUDIO` 流，记录流索引和描述。
- 菜单选择后设置 `requestedAudioStream_`，调用 `reloadAtCurrentPosition()`。
- 重新打开媒体时从当前位置继续，保持原来的暂停/播放状态。

剩余限制：

- 切换发生在文件重开级别，网络流会重新建立连接。
- 没有实现同一解封装上下文内的即时切换。

### 36.2 内嵌字幕轨

已经实现内嵌文本字幕轨的枚举、切换和显示。

实现方式：

- `run()` 枚举 `AVMEDIA_TYPE_SUBTITLE` 流，只对 `isTextSubtitleCodec()` 返回 true 的编码建立解码器。
- 解封装线程遇到字幕包时调用 `decodeSubtitlePacket()`，不进入音视频队列。
- `AVSubtitle` 的 rect 文本写入按开始时间排序的 `embeddedCues_`，查询时二分查找。
- 跳转时不清空已有 cue，重新解码出的相同 cue 会去重，避免跳转后字幕短暂消失。

剩余限制：

- 只支持能还原为文本的字幕编码，PGS/VobSub 等图形字幕不显示。
- 内嵌 ASS 只保留文本，不渲染特效和定位。

### 36.3 硬件解码

可扩展点：

- 选择 D3D11VA/QSV decoder。
- 创建 hw frames context。
- 下载纹理到系统内存再 sws_scale。
- 或直接走 D3D11/Qt RHI 纹理显示。

### 36.4 音频无缝变速

当前倍速会改变音高。可引入 soundtouch、rubberband 或自定义 WSOLA 时间拉伸。

### 36.5 更精确的 A/V sync

当前以媒体时钟和 PTS 等待为主，没有独立的音频硬件延迟补偿。可以进一步：

- 使用 QAudioSink processedUSecs 校正。
- 建立音频主时钟。
- 做视频帧丢弃/重复策略。

### 36.6 网络缓冲

已经加入可中断打开和停止：

- `AVFormatContext` 使用 `AVIOInterruptCB`，`abort_` 置位时中断阻塞读取。
- 打开阶段有 30 秒截止时间，避免网络源长时间卡在 `avformat_open_input`。
- `stop()` 会置位中断标志并关闭队列。

剩余可扩展点：

- 按网络码率动态调整队列长度。
- 断线重试和 HLS/DASH 专门控制。

### 36.7 进度条长媒体

QSlider 使用 int 毫秒。超过约 24 天的媒体无法完整表示。普通媒体不受影响，未来可以改为秒或 qint64 映射。

### 36.8 截图与字幕

`VideoCanvas::snapshot(bool withSubtitles)` 支持两种模式：

- 默认只保存旋转后的视频帧。
- 勾选 `Include Subtitles in Snapshot` 后，在离屏 QImage 上执行与画布相同的字幕绘制。

---

## 37. 2.0 增补实现

### 37.1 暂停跳转预览

暂停状态下跳转需要显示目标位置的一帧，而不是继续停在旧画面。

- `handleSeekLocked()` 设置 `previewPending_`，同时把 `dropUntilMs_` 设为跳转目标。
- 视频线程在暂停时只允许三种情况继续：单帧步进、预览请求、正常播放。
- 预览帧解码后关闭 `previewPending_`，把媒体时钟重置为目标位置并保持暂停。
- 目标时间之前的帧会被丢弃，避免关键帧画面或音频瞬间闪回。

### 37.2 视频旋转

- `displayRotation()` 读取 `AV_PKT_DATA_DISPLAYMATRIX`，用 `av_display_rotation_get()` 得到逆时针角度。
- `VideoCanvas` 缓存旋转后的 `QImage`，每次新帧或旋转角变化时失效。
- `QTransform::rotate()` 的正角方向与 display matrix 相反，因此使用 `rotate(-rotation_)`。
- 截图和画面绘制共用同一张旋转后的图像。

### 37.3 字幕延迟、字号和位置

- 字幕查询位置统一减去 `subtitleDelayMs_`，外挂和内嵌字幕走同一个偏移。
- 字号按视频显示区域宽度计算，再乘以 `subtitleScale_`。
- 底部距离使用可用高度的比例 `subtitleMargin_`，窗口缩放时保持相对位置。
- 参数保存在 `settings.ini` 的 `[subtitle]` 组。

### 37.4 会话恢复和最近文件

- 关闭窗口时把播放列表、当前索引和位置写入 `[session]`。
- 启动时先恢复播放列表（不触发播放），再调用 `playIndex(index, position, true)`，以暂停状态显示上次画面。
- 播放位置在 `Ended`、`Stopped`、`Error` 状态写 0，避免下次启动停在片尾。
- 最近文件保存在 `[recent] files`，去重后最多 12 条。

### 37.5 播放列表拖动排序

`QListWidget` 使用 `InternalMove`。`rowsMoved` 触发 `syncPlaylistOrder()`：

- 从每个 item 的 `UserRole` 和 `UserRole + 1` 读回 target 和 isUrl。
- 重建 `playlist_`，按 target 重新定位 `currentIndex_`。
- 使用 `syncingPlaylistOrder_` 防止重入。

### 37.6 全屏浮动控制条

- 进入全屏时把 `controlBar_` 从布局中移除并重新挂到 `centralWidget()`。
- 手动设置底部几何位置，`resizeEvent` 和显示时重新计算。
- 鼠标移动重新显示控制条，2.5 秒后隐藏；鼠标悬停在控制条上或正在拖动进度时不隐藏。
- 退出全屏时重新放回布局，恢复菜单栏、状态栏和播放列表面板。

### 37.7 画面比例、进度和 OSD

- `VideoCanvas::FitMode` 提供适应、填充裁切、拉伸、16:9、4:3 和原始尺寸。
- 状态栏 `QProgressBar` 显示 0-1000 的播放进度。
- 音量、静音、倍速、字幕和画面比例变化通过画布右上角 OSD 提示。

### 37.8 窗口自适应和网络中断

- 初始窗口尺寸按 `screen()->availableGeometry()` 收缩并居中，避免高 DPI 小屏上控制条落到屏幕外。
- 打开媒体使用可中断的回调上下文和 30 秒截止时间，`stop()` 可以打断阻塞读取。

### 37.9 中英文界面切换

`Language.h/.cpp` 实现语言层，不引入 `.ts` / `lrelease` 工具链：

```cpp
enum class UiLanguage { English, Chinese };
UiLanguageSupport::setCurrent(language);   // 安装/移除翻译器
UiLanguageSupport::current();
UiLanguageSupport::dictionaryText(english); // 当前语言下的文本
```

词典由 `Entry { const char* english; const char* chinese; }` 静态表提供，键就是源码里的英文原文。`DictionaryTranslator::translate()` 只查词典，查不到返回空串，因此 Qt 会继续询问下层的 `qtbase_zh_CN.qm`。

`setCurrent()` 的安装顺序：

1. 中文模式先安装 `qtbase_zh_CN.qm`（从 `applicationDirPath()/translations/` 或 `QLibraryInfo::TranslationsPath` 加载）。
2. 再安装词典翻译器；Qt 按“后安装先查询”的顺序调用，所以词典优先。
3. 切回英文时移除两个翻译器，`QCoreApplication` 自动发送 `QEvent::LanguageChange`。

`MainWindow` 的两段配合：

- `recordUiTexts()` 在构造完成后遍历子对象，记录 `QAction` 的 text/toolTip/statusTip 和 `QLabel`、`QAbstractButton`、`QGroupBox` 的文本；时间、音量、媒体标题、播放项计数和状态栏这些动态标签不记录，`noTranslate` 属性为真的对象（`English`、`简体中文` 菜单项）也跳过。
- `changeEvent()` 收到 `LanguageChange` 后调用 `retranslateUi()`：对每条记录用 `dictionaryText()` 取当前语言文本重新赋值，然后刷新窗口标题、时长、播放按钮、音量、轨道菜单、最近文件，并按播放器状态重刷状态栏。
- 运行时字符串（例如 `Playback speed: %1x`、错误信息）直接 `tr()`，由已安装的翻译器即时翻译，不需要记录。
- 画布占位文字通过 `setCanvasPlaceholder()` 保存英文原文，切换时重新套用。

`switchLanguage()` 只在语言真正变化时执行，负责同步两个菜单项的勾选、调用 `setCurrent()`、`retranslateUi()` 并写回 `ui/language`。启动时 `loadSettings()` 读到空值则使用 `UiLanguageSupport::systemDefault()`（`QLocale::system()` 为中文时选中文），随后在构造函数里安装翻译并执行一次 `retranslateUi()`。

CMake 用 `find_file()` 定位 `qtbase_zh_CN.qm`，POST_BUILD 复制到输出目录的 `translations/`；找不到时中文界面仍可工作，只是标准对话框按钮保持英文。

---

## 38. 代码审阅要点

如果要继续修改，优先检查以下不变量：

1. 不在主线程调用阻塞式 `av_read_frame`、QAudioSink::write。
2. 不在未持有 codecMutex 时 flush/close codec。
3. 不在 VideoCanvas 析构后让视频线程访问 canvas。
4. 每个成功 push 的 AVPacket 只由队列释放一次。
5. seek 后必须 flush 两个 codec 并清两个队列。
6. EOF 必须发送空 flush packet。
7. receive EAGAIN 后才 send packet。
8. paused 后 clock 和 QAudioSink 都要暂停。
9. 单帧后状态仍然必须是 Paused。
10. 播放结束自动切下一项时，旧播放线程必须已经结束。

---

## 39. 一句话总结

这个工程的本质是：

> 用 FFmpeg 把媒体还原成“按时钟到达的音频样本和视频帧”，再用 Qt 把这些样本和帧稳定地输出到扬声器、窗口和控件上。

界面功能很多，但播放内核最终只围绕三件事：

```text
Packet -> Frame -> Time-aligned Output
```

理解了 AVPacket、AVFrame、PTS、媒体时钟和线程所有权，就理解了整个工程。

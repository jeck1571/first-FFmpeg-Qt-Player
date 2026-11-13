# 面试讲解手册 · FFmpeg Qt Player

> 用途：把这个项目讲清楚、讲出深度，用于实习面试与简历准备。
> 配套阅读：[PROJECT-MAP.md](PROJECT-MAP.md)（结构导图 + 接口索引）、[CODE-WALKTHROUGH.md](CODE-WALKTHROUGH.md)（逐模块源码详解）。

---

## 目录

1. [30 秒介绍模板](#1-30-秒介绍模板)
2. [全局心智模型](#2-全局心智模型)
3. [播放内核](#3-播放内核)
4. [渲染层](#4-渲染层)
5. [字幕体系](#5-字幕体系)
6. [UI 层](#6-ui-层)
7. [国际化与构建](#7-国际化与构建)
8. [五个可讲 10 分钟的故事](#8-五个可讲-10-分钟的故事)
9. [高频追问 Q&A](#9-高频追问-qa)
10. [简历写法](#10-简历写法)
11. [如何回答“有什么不足”](#11-如何回答有什么不足)
12. [自测题](#12-自测题)
13. [30 分钟上手路线](#13-30-分钟上手路线)

---

## 1. 30 秒介绍模板

> 这是一个不依赖 `QMediaPlayer` 或 libVLC 的 Windows 桌面播放器：自己用 FFmpeg 做解封装、D3D11VA 硬解和软件回退，自己开三线程做播放控制，正常路径通过 D3D11 VideoProcessor + Qt RHI 直接呈现 GPU 纹理，失败时回退到 `QImage/QPainter`，再在 Qt Widgets 上实现播放列表、字幕、会话恢复、中英切换和全屏/迷你形态。

不要只说“做了个播放器”。要说清楚**实现了播放器内核闭环**：

```
解码 → 时钟 → 同步 → seek → 渲染 → 状态机
```

这六件事能自己讲通，实习面试的技术深度就足够了。

### 1.1 关键数据（背下来）

| 项 | 数值 |
| --- | --- |
| 代码量 | 7048 行（MainWindow 3462 / FFmpegPlayer 1790 / VideoCanvas 476 / SubtitleTrack 227 / Language 449） |
| 模块 | 播放内核、RHI 视频表面、软件画布、帧池、字幕解析、主窗口、国际化 |
| 播放状态 | Idle / Opening / Buffering / Playing / Stalled / Reconnecting / Paused / Stopped / Ended / Error |
| 缩放模式 | 6 种：Fit / Fill / Stretch / 16:9 / 4:3 / Original |
| 倍速 | 0.25×–4.0×，运行时切换，不重建音频设备 |
| 词典 | 261 条词条覆盖全部 320 处 `tr()` |
| 快捷键 | 30+，含 A-B 循环、逐帧、字幕延时 |
| UI 刷新 | 100 ms 定时器；全屏控制条 2.5 s 自动隐藏；缩放 120 ms 稳定后恢复高质量采样 |

---

## 2. 全局心智模型

播放器本质是**四条执行流 + 两个有界队列 + 一条 GPU 零拷贝呈现路径**。把这张图画在白板上，后面所有问题都能顺着讲。

```text
┌────────────── UI 线程（Qt 事件循环，唯一能碰控件） ──────────────┐
│  MainWindow：菜单/快捷键/拖动条/列表                               │
│  QTimer(100ms) → updateUi() → 进度、字幕、A-B、状态                │
│  player_->open/setPosition/play/pause  ← 只写原子量、不阻塞        │
│  ← stateChanged / mediaOpened / playbackFinished（自动排队回 UI）   │
└───────────────▲───────────────────────────────┬───────────────────┘
                │ 信号                           │ 方法调用
┌───────────────┴──── 控制线程 run()────────────▼───────────────────┐
│ avformat_open_input → find_stream_info → 开三个解码器 → 建队列      │
│ readPackets()：av_read_frame                                       │
│    音频包 → audioQueue(容量48)   视频包 → videoQueue(容量24)        │
│    字幕包 → 当场解码进有序缓存（不进队列）                           │
│ 循环顶部检查 seekRequestMs_ → av_seek_frame + flush + 清空队列      │
└──────┬──────────────────────────────────┬─────────────────────────┘
       │ 满则阻塞（背压，内存有上界）      │
┌──────▼──── 音频线程 ─────────────┐ ┌────▼──── 视频线程 ───────────────┐
│ 收包→解码→swr 重采样→atempo 倍速   │ │ 收包→D3D11VA→对齐时钟→丢过期帧    │
│ →QAudioSink 输出→音频主时钟        │ │ →VideoProcessor→QRhiWidget      │
│ 暂停时 sink->suspend()            │ │ 失败→回读→sws_scale→QImage       │
└───────────────────────────────────┘ └──────────────────────────────────┘
```

三条铁律：

1. **UI 线程永不阻塞**：打开、seek、解码、读网络全在后台，UI 只写原子变量。
2. **只有 UI 线程碰控件**：解码线程只能把 `QImage` 交给画布。
3. **内存必须有上界**：队列容量固定，满了阻塞读包线程，这就是背压。

---

## 3. 播放内核

### 3.1 状态机

```
Idle ──open()──► Opening ──成功──► Playing ⇄ Paused
                    │                 │
                    └──失败──► Error  ├──stop()──► Stopped
                                      └──EOF─────► Ended ──play()/seek──► 重开
```

关键约定：

- `Opening` 是给**网络流**留的。`avformat_open_input` 可能卡几十秒，所以有中断回调 `FFmpegPlayer.cpp:717`：`abort_` 为真、或打开超过 30 秒就返回 1。没有它，关窗口会挂死。
- `Ended` 后按播放键会重开；`setPosition()` 在 `Ended` 会**以暂停态重开到目标位置**（`FFmpegPlayer.cpp:483`），这样播完后进度条还能拖。
- 状态是 `std::atomic<State>`，谁都能读；写统一走 `setState()` 并发信号。

### 3.2 `open()` 的完整时序

`FFmpegPlayer.cpp:382`：

```
stopThreads()          确保旧的三条线程全部退出并 join（幂等）
清空全部原子状态         seek 请求、丢帧阈值、单帧计数、已打开标志
新建 PlaybackClock/AudioClock  锚在 startPosition，写入当前倍速/暂停态
spawn 控制线程          run(target) —— open() 立刻返回，UI 不卡
```

`run()` 内部顺序（`FFmpegPlayer.cpp:838`）：

1. `avformat_open_input`（带中断回调）→ `avformat_find_stream_info`
2. 枚举可解码的音轨 / 文本字幕轨（`avcodec_find_decoder` 过滤不支持的）
3. 选流：视频用 `av_find_best_stream`；音频优先用户请求，否则 best；字幕 `-1` 表示关，未指定则默认第一条
4. 读时长（容器没有就逐流累加）、旋转角、帧率、像素格式
5. **初始 seek**：`startPositionMs > 0` 时 `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)` + flush 三个解码器
6. 建队列（音频 48 包 / 视频 24 包，约 1 秒缓冲）
7. `emit mediaOpened` / `mediaInfoChanged`（媒体信息文本此时已拼好）
8. 启动音频线程、视频线程
9. **当前线程变成 demux 线程**，进 `readPackets()`
10. 读到 EOF → 每个队列压一个**空包当哨兵** → `finish()` → join 两个 worker → 关解码器/容器 → `Ended` + `playbackFinished`

> **为什么控制线程复用为读包线程**：打开阶段它需要独占；播放阶段读包本身是串行的，没必要再多一条线程，省一条线程和一轮同步。

### 3.3 为什么是三条线程

| 线程 | 阻塞在哪 | 合并会怎样 |
| --- | --- | --- |
| 控制/读包 | `av_read_frame`（网络/磁盘） | 阻塞解码，画面卡住 |
| 音频 | `QAudioSink::write`（等声卡消费） | 阻塞视频，音画互相拖累 |
| 视频 | 等时钟 `msleep` | 阻塞音频，声音断续 |

音视频必须分开：**音频节奏由声卡决定，视频节奏由屏幕决定**，两者天然不同步，只能靠时钟协调。

### 3.4 `AudioClock`：一行公式解释同步

`FFmpegPlayer.cpp:193`：

```
audioMediaMs = 锚点媒体 PTS + QAudioSink::processedUSecs() / 1000 × rate
mediaTime    = audioMediaMs                                      // 有音频
mediaTime    = 墙钟外推                                           // 无音频回退
```

三个动作都要重新建立主时钟基准：

- 暂停：冻结当前 `processedUSecs` 对应的媒体时间，视频线程停止推进；
- 恢复：重新记录音频设备时间和媒体 PTS 锚点；
- 变速：先结束旧 `atempo` 时间轴，再改 `rate` 并重锚音频主时钟。

**加分回答**：有音频时以 `QAudioSink::processedUSecs()` 和最近写入帧的媒体 PTS 建立主时钟，视频跟随实际音频消费进度；无音频或音频设备不可用时才回退到墙钟。这样解决的是“墙钟与声卡实际消费速度长期不一致”的问题。

### 3.5 音视频同步：两条规则

**视频对齐音频主时钟**：

```cpp
if (clock_->mediaTime() - frameMs > dynamicThreshold)
    drop frame;                            // 落后过多则丢帧追赶
waitUntil(frameMs);                        // 否则等到目标显示时刻
```

为什么“丢”而不是“快放”：快放会出现肉眼可见的加速抖动；丢帧只是少几帧，观感平滑。阈值根据视频帧间隔和近期抖动动态计算，而不是永远固定 150 ms。

**音频驱动时钟**：音频线程主要受 `QAudioSink` 可用空间和写入阻塞控制；已写入帧的媒体 PTS 和 `processedUSecs()` 共同更新主时钟。声音的连续性优先，视频为音频让路。

### 3.6 seek 全链路（最容易被追问）

```
UI: setPosition(ms)
  ├─ 夹在 [0, duration]
  ├─ seekRequestMs_ = ms；seekPending_ = true；positionMs_ = ms   ← UI 立刻有反馈
  └─ 两个队列 setBlocked(true)                                    ← 读包线程下一包会阻塞

读包线程（循环顶部）:
  ├─ av_seek_frame(-1, ms×AV_TIME_BASE/1000, AVSEEK_FLAG_BACKWARD)
  ├─ avcodec_flush_buffers × 3                                    ← 丢掉解码器内部旧帧
  ├─ 两个队列 clear() + setBlocked(false)                          ← 丢掉已缓存的旧包
  ├─ clock_->reset(ms)（保持当前暂停态）
  ├─ dropUntilMs_ = ms                                            ← 之后 PTS < ms 的帧一律丢
  └─ 若当前是暂停态：previewPending_ = true                        ← 放一帧出来当预览
```

五个“为什么”：

1. **为什么 `AVSEEK_FLAG_BACKWARD`**：`av_seek_frame` 只能定位到关键帧，BACKWARD 保证落在目标**之前**，再靠解码追到目标；配合 `dropUntilMs_` 丢掉关键帧到目标之间的帧，画面才精确。
2. **为什么 flush**：解码器有 B 帧重排和内部缓存，不 flush 会吐出 seek 前的旧帧。
3. **为什么清队列**：队列里是旧位置的包，不清就会“先闪一下旧画面再跳”。
4. **为什么阻塞队列而不是加锁**：阻塞的是 `push()`（读包线程），`pop()` 照常超时返回，worker 不会卡死；seek 完成后立即解除。
5. **为什么暂停态也要放一帧**：暂停时视频线程本该休眠，不放行就永远黑屏；`previewPending_` 放行**一帧**后立刻重置时钟并继续暂停。

### 3.7 倍速：三处配合

```cpp
rate = clamp(rate, 0.25, 4.0);
clock_->setRate(rate);        // 时钟走快/走慢
```

1. **视频**：`waitUntil` 把媒体时间差折算成墙钟等待，同时检查音频主时钟，迟到过多则丢帧追赶。
2. **音频**：进入 `abuffer -> atempo -> abuffersink` 滤镜图；`swresample` 只负责格式和采样率转换，不再通过改变输入采样率制造变速。
3. **位置推进**：按源帧 PTS/采样数推进媒体位置，倍速由音频滤镜和主时钟统一处理。

**代价（主动说）**：`atempo` 会引入一定算法延迟和 CPU 成本；极端倍速需要串联滤镜，长时切换仍要验证时钟重锚和音画漂移。

### 3.8 暂停、单帧、预览帧

- 暂停：`paused_ = true` → 时钟冻结 + `sink->suspend()`（不是 stop，避免重建）
- 视频线程暂停时 `msleep(5)` 空转，**只有两种情况放行**：`videoStepCount_ > 0`（单帧）或 `previewPending_`（seek 预览）
- 单帧：`stepFrame()` 最多累积 8 次；每放一帧就 `clock_->reset(position)` 并保持暂停，位置不漂
- 后退单帧：主窗口按帧率算出 `1/fps` 再 seek，因为解码器不能倒着解

### 3.9 切音轨 / 字幕轨 = 局部热切换

音轨切换只重建目标音频 decoder、队列、`QAudioSink` 和音频时钟；字幕轨只重建字幕 decoder 并清空 cue。demux 线程、视频线程和当前媒体打开序号保持不变。

- 优点：切换不需要重新打开媒体，避免整段黑屏和重新 seek。
- 代价：音频设备重建、队列清理和字幕 cue 更新仍需要短暂过渡，必须用 `openSerial` 和自动化测试证明没有误重开。

### 3.10 结束、错误、内存

- **EOF 的哨兵包**：推一个空 `AVPacket` 再 `finish()`。空包发给解码器等价于 drain，能把 B 帧/延迟帧吐完；`receive_frame` 返回 `AVERROR_EOF` 才退出循环。
- **谁拥有包**：`av_read_frame` 分配的包，push 成功后归队列；push 失败（abort/blocked）当场 `av_packet_free`；pop 出来的包解码后立即 free；队列 `clear()` 统一释放。
- **帧与图像**：正常 GPU 路径由 `GpuVideoFrame` 延长 `AVFrame` 生命周期，纹理交给 `QRhiWidget`；回退路径使用三槽 `QImage` 帧池，GUI 仍持有的槽位不会被覆盖。
- **销毁顺序**：`~MainWindow` 先 `setVideoCanvas(nullptr)`（新帧直接丢弃）再 `player_->stop()`（join 所有线程），之后子控件才销毁——这是防 use-after-free 的关键顺序。

### 3.11 线程安全清单

| 数据 | 保护方式 | 理由 |
| --- | --- | --- |
| 状态、暂停、音量、倍速、seek 请求、位置、时长 | `std::atomic` | 单写多读标量，无锁开销 |
| `formatContext_` / 三个解码器 | `codecMutex_` | 读包线程和两个 worker 都会碰 |
| 两个队列指针 | `queueMutex_` | 可能被替换/销毁 |
| 画布指针 | `std::atomic<VideoCanvas*>` | 跨线程，析构前置空 |
| 轨道列表、媒体信息、错误、目标路径 | 各自 mutex + 返回拷贝 | 避免外部持有内部引用 |
| 字幕 cue 缓存 | `subtitleMutex_` + 有序插入 | 读包线程写、UI 线程查 |
| 画布帧/字幕/OSD | 画布内 `QMutex`，**只在拷贝引用时持锁** | 绘制/缩放不能阻塞解码线程 |

---

## 4. 渲染层

### 4.1 为什么不用 `QVideoWidget`

1. **控制权**：`QVideoWidget` 把“帧怎么画”交给 Qt/平台后端，做不了自定义黑边、OSD、字幕描边、快照合成、整数像素对齐。
2. **依赖**：Qt 的 FFmpeg 后端版本绑死在 Qt 里，而项目要直连 FFmpeg 3.4 SDK、自己控制 seek/倍速语义。
3. **学习目标**：自绘才能讲清 YUV→RGB、缩放、重绘、DPI。

### 4.2 一帧的旅程

```
视频线程: AVFrame(YUV) --sws_scale--> QImage(ARGB32) --setFrame--> 画布
主线程:   paintEvent --drawImage--> QPainter --合成--> 屏幕
```

`sws_scale` 的目标格式是 **BGRA**（小端即 `Format_ARGB32`），`destinationStride` 必须传 `QImage::bytesPerLine()`，否则宽不是 4 的倍数时会花屏。这个细节能讲出来很加分。

### 4.3 合并重绘：`scheduleRepaint()`

```cpp
if (!repaintQueued_.exchange(true))                      // 最多一个在飞
    QMetaObject::invokeMethod(this, "update", QueuedConnection);
// paintEvent 第一行:
repaintQueued_.store(false);                             // 画完才允许下一次
```

没有它，60 fps 解码会往主线程事件队列塞 60 个 `update`；缩放时主线程一卡，队列爆炸、内存上涨、越拖越慢。价值是“**生产者（解码）与消费者（绘制）解耦，且消费者只保留最新帧**”。

### 4.4 `paintEvent` 的加锁策略

`VideoCanvas.cpp:348`：

```
锁内：拷贝 QImage 句柄、字幕、占位、OSD、模式、样式   ← 几十纳秒
锁外：缩放、绘制文字、画背景                        ← 几毫秒
```

`QImage` 隐式共享，拷贝句柄不搬数据；绘制期间解码线程可以继续换帧（旧帧靠引用计数活着）。这是“**缩小临界区**”的标准示范。

### 4.5 黑边怎么算（六种模式）

`VideoCanvas.cpp:241`：先算 `drawSize`，再按 `(viewport - drawSize)/2` 居中，最后 `qRound` 对齐整像素。

| 模式 | 行为 |
| --- | --- |
| Fit | 等比缩放，装得下 → 上下或左右黑边 |
| Fill | 等比放大铺满 → 裁掉溢出 |
| Stretch | 拉伸变形 |
| 16:9 / 4:3 | 先按视口宽度算，超高了再按高度反推 |
| Original | 1:1 像素 |

**整数对齐**是缩放 bug 修复的核心：不 `qRound` 时，1920×1080 在 1480 宽窗口的居中偏移是 168.5，黑边一边 168 一边 169，缩放过程中在两者间跳，出现一条会“呼吸”的亮线。

### 4.6 缩放快速路径

```cpp
// resizeEvent()
smoothScaling_ = false;      // 拖动期间用最近邻，省 CPU
resizeSettleTimer_->start(); // 120 ms 后设为 true 并重绘
update();                    // 整帧重绘
```

为什么必须整帧重绘：`WA_OpaquePaintEvent` 告诉 Qt“这块我自己画满”，Qt 缩小时只重绘新暴露的条带，结果旧帧边缘残留，看起来像“黑边在爬”。

### 4.7 字幕排版

`VideoCanvas.cpp:286`：

- 字号 = `clamp(视频显示宽度 / 42 × 缩放, 14, 72)`，跟着**视频矩形**而非窗口走
- 最大宽度 = 视频宽 - 72，自动换行、水平居中
- 背景：圆角矩形 + 可调透明度
- 描边：在 `±outlineWidth` 的 9 个偏移位置重复画同色文字，最后画正文
- 底部边距 = 视频高 × 比例，夹在 `[8, 高/2]`

### 4.8 旋转与快照

- 旋转按 `(frame_, rotation)` 缓存：FFmpeg 的 display matrix 是**逆时针**，`QTransform::rotate()` 正角是**顺时针**，所以取 `-rotation`
- 快照：锁内复制**已旋转**的帧，锁外用同一个 `drawSubtitle` 把字幕烧进图片，所见即所得

---

## 5. 字幕体系

优先级：**外挂文件 > 内嵌文本轨 > 无**。

### 5.1 外挂（`SubtitleTrack`）

- 编码：去 UTF-8 BOM → 严格 UTF-8 解码 → 失败降级 `fromLocal8Bit`
- SRT：`\r\n` 归一 → 按空行分块 → 找含 `-->` 的行 → 之后都是文本
- ASS：只进 `[Events]` 段，`Dialogue:` 按逗号切 10 段，第 1/2 段是时间，**第 9 段之后合并回文本**（文本里本身有逗号）
- 清洗：正则去掉 `<font>`、`{\pos(...)}` 等标记，`\N` 转换行
- 时间：正则 `(h:)?m:s[.,:]fff`，小数位补零到 3 位
- 存 `QVector<Cue>` 后按开始时间排序
- 查询：`upper_bound(startMs)` 找第一条开始时间 > 当前时间的前一条，再判 `position <= endMs` → **O(log n)**

### 5.2 内嵌（在播放内核里）

字幕包不进队列，读包线程**直接解码**，转成 `{startMs, endMs, text}` 插入有序 `vector`（`upper_bound` 插入 + 按 `(start, text)` 去重）。UI 线程通过 `embeddedSubtitleTextAt()` 二分查询。

为什么不用队列/线程：字幕包量小、解码便宜；而且字幕必须和 `positionMs_` 同源，放在读包线程最自然。

### 5.3 时间轴

```
显示文本 = 查表(位置 - 字幕延时)      // 正延时 = 字幕晚出现
字号     = UI 缩放 0.5–2.5
位置     = 底部边距 0.02–0.45
```

---

## 6. UI 层

### 6.1 纯代码建界面

没有 `.ui` 文件。好处：QSS 可用 `#controlBar`、`#panelTitle` 这类 id 选择器；布局关系一眼可见；不用维护 uic 生成物。代价：`buildInterface()` + `buildMenus()` 很长，是项目最大的可维护性债。

### 6.2 100 ms 轮询 vs 逐帧信号

内核是 C++ 线程，没有“位置变化”信号。UI 用 100 ms 定时器拉取 `position()`：简单、无锁、10 Hz 足够读时间码。代价是进度条不逐帧平滑，可用 60 fps 插值动画优化。**能说清“为什么轮询而不是推送”，比会写信号槽更值钱。**

### 6.3 拖动进度条：为什么要 `eventFilter`

Qt 的 `QSlider` 按下会立刻改 `value` 并触发 `valueChanged`，100 ms 后 `updateUi()` 又把值拉回真实位置，于是“拖不动 / 回弹”。

解法（`MainWindow.cpp:1688`）：

1. `MouseButtonPress`：**吞掉事件**，手动 `setSliderDown(true)` + `grabMouse()`（事件被吞，QSlider 不会自己抓鼠标），立即跳到点击位置预览
2. `MouseMove`：更新预览位置与时间标签，**不 seek**
3. `MouseButtonRelease`：`setSliderDown(false)` 触发 `sliderReleased` → 真正 `setPosition()`
4. `UngrabMouse`（Alt+Tab / 弹窗抢走鼠标）：兜底提交，避免滑块卡死
5. `seeking_` 标志保持 150 ms，阻止 `updateUi` 在 seek 生效前回拉

### 6.4 播放列表：模型与视图严格 1:1

`QVector<PlaylistEntry> playlist_` ↔ `QListWidget` 行号一一对应：

- 搜索过滤用 `setHidden()`，**不删除项**，行号才不会错位
- 拖拽排序 `rowsMoved` → `syncPlaylistOrder()` 把视图顺序写回模型
- 排序 / 去重后才 `rebuildPlaylistView()` 重建
- 删除当前项、删除前面项，两种 `currentIndex_` 修正逻辑不同

### 6.5 会话与续播（两个独立机制）

| 机制 | 键 | 行为 |
| --- | --- | --- |
| 会话恢复 | `session/items`、`session/current`、`session/position` | 重启后恢复整个列表，**以暂停态**打开上次那一项 |
| 单文件续播 | `resume/items`（最多 100 条） | 打开任意文件时，若记录 > 5 s 且不在结尾 3 s 内，自动跳到该处并 OSD 提示 |

细节：记录时要求 `position > 5000 && position < duration - 3000`；恢复时若 `>= duration - 1000` 则从 0 开始（防止“一打开就结束”）。

### 6.6 全屏 / 迷你 / 紧凑 / 置顶

- **全屏**：隐藏 `normalChrome_`；把控制栏从布局取出挂到 `centralWidget()` 当浮层；`showFullScreen()` 之后 **`singleShot(0)`** 再算几何（窗口管理器改完尺寸才算得准）；鼠标移动显示、2.5 s 自动隐藏；退出按 `wasMaximized_` 只切一次窗口状态。
- **迷你**：隐藏菜单/状态栏/列表、强制置顶、560×340、最小 400×260；退出还原几何与置顶，最小尺寸回到 640×400。
- **紧凑**：< 820 隐藏倍速、< 780 隐藏循环/随机、< 740 隐藏音量、< 700 隐藏媒体名；媒体名用 `elidedText` 中段省略。
- **置顶**：`setWindowFlag(Qt::WindowStaysOnTopHint)` 后必须 `show()` 一次才生效（Windows 平台特性）。

### 6.7 快捷键

`Space` 播放暂停 · `E` 单帧 · `S` 停止 · `←/→` ±5 s · `P/N` 上/下一项 · `[` `]` 调速 · `F11/F` 全屏 · `Esc` 退出全屏/迷你 · `Ctrl+P` 快照 · `Ctrl+G` 跳转 · `M` 静音 · `↑/↓` 音量 · `Ctrl+J` 播放列表 · `Ctrl+Shift+M` 迷你 · `Ctrl+Shift+A/B/X` A-B 循环 · `Ctrl+Alt+←/→` 字幕延时。

---

## 7. 国际化与构建

### 7.1 三层回退，零重启

1. **内置词典** `kEntries[261]`（`Language.cpp:23`）
2. **自定义 `DictionaryTranslator`**：`translate()` 查词典，**查不到返回空串**，Qt 就会继续找下一个翻译器（返回空串会截断回退链）
3. **`qtbase_zh_CN.qm`**：本地化 OK/Cancel 等标准按钮

切换流程：菜单 → `setCurrent()` 装/卸翻译器 → `retranslateUi()` 重设所有文本 → `QEvent::LanguageChange` 到达时 `changeEvent()` 兜底一次 → 写 `ui/language`。

难的是**哪些文本要登记**：`recordUiTexts()` 只登记静态文本（QAction 的 text/toolTip/statusTip、QLabel、按钮、GroupBox），动态内容（时间、音量、媒体名、计数）排除，否则运行时的“12:34”会被当成英文原文存进词典。

**为什么不用 `.ts/.qm`**：零重启即时切换、无 lupdate/lrelease 构建步骤；代价是没有复数/上下文/占位符安全校验，261 条词典要手工维护。

### 7.2 构建

- CMake ≥ 3.24，C++17，Qt 6.5+（Core / Gui / Widgets / Multimedia），AUTOMOC
- FFmpeg 3.4 动态链接，POST_BUILD 拷 14 个 DLL
- `windeployqt` 自动部署 Qt 运行时；复制 `qtbase_zh_CN.qm` 到 `translations/`
- MSVC `/utf-8 /permissive- /W3`

```powershell
cmake -S . -B build-vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build-vs2022 --config Release
```

---

## 8. 五个可讲 10 分钟的故事

统一结构：**现象 → 假设 → 怎么验证 → 根因 → 修法 → 学到什么**。

### 故事 1：进度条拖不动、回弹

- 现象：拖动后 100 ms 跳回去
- 验证：在 `valueChanged` 和 `updateUi` 打日志，确认是 UI 轮询覆盖用户输入
- 根因：`QSlider` 按下即改 value；轮询无差别写回
- 修：接管 press（吞事件 + grab）、拖动只预览、释放才 seek、`seeking_` 150 ms 抑制、Ungrab 兜底
- 学到：**“输入优先级”是 UI 状态机问题，不是信号槽写错**

### 故事 2：倍速明显卡顿与变调

- 现象：2× 下画面一顿一顿、声音断续
- 假设：位置推进用错时间基 / 睡眠没有按倍率折算 / 音频设备重建 / 重采样制造变速
- 验证：打印 `position` 增速与墙钟增速比值，确认 2× 时睡眠过长
- 根因：三处叠加（输出时长当源时长、`waitUntil` 用媒体时间睡眠、用输入采样率伪装倍速）
- 修：位置按源帧 PTS/采样数推进、等待按倍率折算、`swr` 只做格式转换、接入 `atempo` 保音高，并用音频主时钟控制丢帧
- 学到：**倍速是“时间基”问题，不是“播放速度”问题**

### 故事 3：快进一按就暂停

- 现象：点前进 5 s，状态变暂停
- 根因：seek 路径上状态与时钟不一致（flush / 清队列 / 时钟重置没有在一个序列里完成，UI 读到中间态）
- 修：把 seek 全部收进 `handleSeekLocked()`，在**读包线程**里一次性完成“定位→flush→清队列→重置时钟→设丢帧阈值”，并保持 `paused_` 原值；UI 不再参与
- 学到：**并发 bug 要用“单点收敛”解决，不是到处加锁**

### 故事 4：缩放闪烁、残影、黑边不对称

- 现象：四类症状同时出现
- 验证：先解决“看不见”——DPI 感知截图 + 纯色像素扫描，拿到真实客户区边界（踩过坑：不声明 DPI 感知，`GetWindowRect` 返回虚拟坐标，截图裁错，差点误判“视频没居中”）
- 根因：① 只重绘条带 + opaque 画布 → 残影；② 亚像素居中 → 黑边宽度跳变；③ 全屏浮层用旧尺寸算几何 + 退出双切窗口 → 闪黑/错位；④ 解码线程刷屏 `update` → 拖动卡
- 修：全量重绘 + 整数对齐 + 120 ms 快速采样 + 合并重绘 + `singleShot(0)` 摆浮层 + 单次状态切换
- 学到：**性能问题先建可观测性，再谈优化**；截图/像素扫描也是测试手段

### 故事 5：关闭窗口偶发崩溃

- 现象：退出时偶发访问违例
- 根因：视频线程可能在画布销毁后调用 `setFrame`
- 修：析构顺序 `setVideoCanvas(nullptr)` → `stop()`（join）→ 才轮到子控件析构；`stopThreads()` 置 abort → 中止队列 → join
- 学到：**线程生命周期必须由拥有者显式收尾，不能交给 Qt 父子树**

---

## 9. 高频追问 Q&A

**Q1：为什么不用 QMediaPlayer？**
控制力和依赖。要做自绘、精确 seek、倍速语义、字幕烧录、快照合成，而且要直连 FFmpeg 3.4。代价是重复造轮子——我清楚这一点，收获是完整掌握解码/时钟/同步闭环。

**Q2：音画不同步怎么排查？**
先打印“PTS、时钟、差值”三条曲线：差值为正且持续增长说明视频慢（解码慢/阈值太高），为负说明音频慢（sink 欠载/重采样配置错）。再查 seek 后是否漏了 reset 时钟。最后看 UI 卡顿是否导致 `update` 堆积。

**Q3：为什么队列要设容量？**
内存上界 + 背压。不设容量，网络流会把内存吃光；设了容量，读包线程自然被解码速度牵制，形成流水线。

**Q4：`AVPacket` 谁释放？**
读包线程分配，push 成功归队列；pop 后解码完立即 free；队列 clear/析构统一释放；push 失败当场释放。`AVFrame` 只 alloc 一次、循环 `unref`。`AVSubtitle` 每条路径都 `avsubtitle_free`（包括解码失败但 `got` 为真的分支）。

**Q5：`av_seek_frame` 之后为什么画面会“闪回”一下？**
因为定位到关键帧，关键帧可能在目标之前几百毫秒。修法是 `dropUntilMs_`：视频丢 `PTS < target` 的帧，音频容忍 40 ms 误差。

**Q6：暂停时为什么用 `suspend()` 而不是 `stop()`？**
`stop()` 会丢弃缓冲并需要重启设备，恢复时有爆音和延迟；`suspend()` 保留设备，恢复快。

**Q7：为什么 `emit` 能跨线程安全？**
`FFmpegPlayer` 在 UI 线程创建，信号从工作线程发出；接收者 `MainWindow` 属于 UI 线程，Qt 自动用队列连接。`errorOccurred` / `playbackFinished` 还显式写了 `Qt::QueuedConnection`。`State` 跨线程传参前 `qRegisterMetaType`。

**Q8：画布的锁会不会拖慢解码？**
不会。锁只覆盖引用拷贝（微秒级），缩放和绘制都在锁外；`QImage` 隐式共享，拷贝不搬像素。

**Q9：GPU 渲染为什么还要 QImage 帧池？**
正常路径不需要 CPU 图像，D3D11VA 纹理直接交给 VideoProcessor 和 `QRhiWidget`。`QImage` 三槽帧池只服务于 RHI/VideoProcessor 初始化失败后的硬件回读或软件解码回退，避免回退路径每帧分配；GUI 仍持有的槽位通过 `QImage` cleanup owner 检测，不会被覆盖。

**Q10：主窗口 3462 行，怎么维护？**
这是已知债务。拆法：`PlaybackController`（播放/列表逻辑）、`ChromeController`（全屏/迷你/紧凑）、`SubtitleController`、`SettingsStore`，MainWindow 只做装配。

**Q11：怎么测的？**
Debug/Release 各多轮；用 30 秒测试片验证拖动/倍速/逐帧/跳转/结束重开；UI 层用 PowerShell 发送按键 + DPI 感知截图 + 像素扫描做断言；字幕用带两条音轨和内嵌 mov_text 的样本。当前 Debug CTest 为 6/6，通过 `PlaybackCoreTests`、`CrashReporterSmoke`、`PlayerSmoke`、`NetworkRecoverySmoke`、`TrackSwitchSmoke` 和 `GpuRenderSmoke`；GPU smoke 连续 5 次验证 `d3d11va-zero-copy -> VideoProcessor -> QRhiWidget` 和 `readbacks=0`。

**Q12：高 DPI 怎么处理？**
Qt6 默认按 DPI 缩放，布局全用逻辑像素；截图验证时必须声明 per-monitor DPI 感知，否则拿到虚拟坐标。这个坑我踩过并写进了测试脚本。

**Q13：硬件解码和 Qt 渲染如何共享 GPU 帧？**
用 `AVD3D11VADeviceContext` 复用 `QRhiWidget` 暴露的 D3D11 device/context，`get_format` 选择 `AV_PIX_FMT_D3D11`，硬件帧的 `NV12/P010` 纹理交给 D3D11 VideoProcessor 转成 BGRA 输出纹理，再由 RHI 绘制。FFmpeg 解码和 Qt RHI 共用即时上下文，因此用递归锁、FFmpeg `lock/unlock` 和 `ID3D11Multithread` 建立统一同步边界；失败时回读到 `QImage` 或软件解码。

**Q14：如果要支持 ASS 特效字幕？**
现在只取纯文本；完整方案是集成 libass，把字幕渲染成 RGBA 图层再合成到画布。

**Q15：项目最大的技术难点是什么？**
不是某个 API，而是**三条线程 + 一个时钟 + 一个 seek 请求**的状态一致性：任何一处顺序错了都会表现成“偶尔卡一下 / 偶尔黑一下”。做法是把 seek 收敛到单点、把跨线程共享量降到原子标量、把重绘合并成一个“最新帧”通道。

---

## 10. 简历写法

### 版本 A：C++ 客户端 / 音视频（推荐）

> **FFmpeg Qt 播放器（个人项目，C++17 / Qt6 / FFmpeg 3.4，约 7k 行）**
>
> - 基于 FFmpeg 自研播放内核：解封装、解码、音视频同步、seek 全链路；三线程 + 有界包队列实现背压，UI 全程不阻塞。
> - 以 `QAudioSink::processedUSecs()` 建立音频主时钟，视频按 PTS 和动态阈值丢帧；用 FFmpeg `atempo` 实现 0.25×–4× 保音高变速。
> - Windows 视频路径接入 D3D11VA，并与 Qt RHI 共享 D3D11 设备；D3D11 VideoProcessor 完成 NV12/P010→BGRA 零拷贝呈现，失败时回退硬件回读或软件解码。通过递归锁与 `ID3D11Multithread` 解决即时上下文并发。
> - 建立 6 项 CTest、HTTP Range 断线恢复、热切轨、GPU 像素校验和 PlayerBench 性能基准，记录首帧、seek p95、A/V drift、丢帧、CPU、内存、回读和重连指标。
> - 工程化：INI 会话恢复与单文件续播、M3U/PLS 导入导出、261 条词典的零重启中英切换、全屏/迷你/紧凑自适应布局、30+ 快捷键。

### 版本 B：Qt 桌面开发

突出纯代码 UI + QSS 双主题、QSlider 事件接管、模型/视图 1:1、QSettings、i18n、DPI、CMake 构建。

### 版本 C：多媒体 / 图形方向

突出 YUV→RGB 转换、stride 对齐、缩放采样策略、重绘合并、帧生命周期。

### STAR 口述模板（60 秒）

> **S**：播放器在窗口缩放和全屏切换时出现闪烁、残影、黑边不对称和卡顿。
> **T**：在不牺牲画质的前提下让缩放跟手且画面稳定。
> **A**：先做 DPI 感知截图 + 像素扫描建立可观测性，定位到“条带重绘 + 亚像素居中 + 浮层几何时序 + 解码线程刷屏”四个独立原因，分别用全量重绘、整数对齐、`singleShot(0)` 延后布局、原子合并重绘解决。
> **R**：快速连续改尺寸后画面边界与稳定态完全一致，黑边对称，窗口可缩到 640×400 且控制栏自动收窄。

不要写“精通 C++/Qt”“独立完成一个播放器”这种空话。写**你在什么约束下解决了什么问题**。

---

## 11. 如何回答“有什么不足”

按“**我知道 → 代价 → 我会怎么改**”三段说。

| 不足 | 代价 | 改进 |
| --- | --- | --- |
| HDR/tone mapping 未专项验证 | HDR 片源可能偏色 | 解析 HDR 元数据并按显示器能力映射 |
| 4K/8K 和驱动矩阵未充分验证 | 高码率/混合显卡可能回退或卡顿 | 建立 4K soak、显存指标和跨驱动 GPU smoke |
| 真实 RTSP/HLS 弱网未充分验证 | CDN 切换或长断网恢复未知 | 增加真实网络故障注入和 ABR 测试 |
| 完整 ASS 特效和位图字幕未支持 | 复杂字幕降级为纯文本 | 接 `libass`，单独评估 PGS/VobSub |
| FFmpeg 3.4 较旧 | 新 API 和安全修复缺失 | 封装后端后升级 FFmpeg 6/7 并做 A/B 回归 |
| MainWindow 3462 行 | 改一处要读很久 | 拆 4 个 Controller |
| 词典无复数/上下文 | 复杂文案受限 | 迁移 `.ts/.qm` 或加占位符校验 |

---

## 12. 自测题

1. 画出三条线程和两个队列，标出各自阻塞点。
2. 写出 `mediaTime()` 公式，说明暂停时为什么要先结算。
3. seek 的五个步骤按顺序说，解释 `AVSEEK_FLAG_BACKWARD` 与 `dropUntilMs_` 的关系。
4. 为什么暂停态 seek 要 `previewPending_`？
5. 2× 倍速时音频侧改了哪三个量？位置按哪个时长推进？
6. 视频落后 300 ms 时会发生什么？为什么不是快放？
7. EOF 的哨兵包解决什么问题？
8. `scheduleRepaint` 的原子标志防的是什么？
9. 缩放时为什么要关 `SmoothPixmapTransform`？120 ms 后为什么恢复？
10. 黑边为什么要 `qRound`？
11. 外挂字幕查询为什么是 O(log n)？边界条件是什么？
12. 关闭窗口的析构顺序，为什么能防崩溃？

---

## 13. 30 分钟上手路线

1. **跑起来**：

   ```powershell
   cmake -S . -B build-vs2022 -G "Visual Studio 17 2022" -A x64
   cmake --build build-vs2022 --config Release
   .\build-vs2022\Release\FFmpegQtPlayer.exe sample.mp4
   ```

2. **逐个功能点一遍**：拖进度条（标签跟手、松手才跳）→ 按 `]` 到 2×（确认不变调、画面加速）→ 暂停后 `E` 逐帧 → `Ctrl+Shift+A/B` 设 A-B → `F11` 全屏后移动鼠标看控制条。

3. **按这个顺序读码**（每段 15 分钟）：

   `FFmpegPlayer.h`（状态与原子量）→ `PlaybackClock/AudioClock` → `PacketQueue` → `open/run` → `readPackets` → `handleSeekLocked` → `videoLoop` → `audioLoop` → `RhiVideoSurface`/`D3d11ContextSync` → `VideoCanvas` 回退路径 → `SubtitleTrack` → `MainWindow::connectPlayer/updateUi/playIndex` → `Language`。

4. **改一个东西验证理解**：
   - 把丢帧阈值 150 改成 500，观察卡顿后的追帧行为，再改回；
   - 把视频队列容量 24 改成 2，观察卡顿；
   - 把 `smoothScaling_` 初始值改为 `true`，观察缩放时的差异。

   这三个实验能让你真正理解背压、自愈和平滑采样。

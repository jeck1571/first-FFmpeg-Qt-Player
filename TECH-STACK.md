# FFmpeg Qt Player 技术栈说明

## 1. 一句话结论

这个项目是一个使用 **C++17、Qt 6 和 FFmpeg 3.4** 开发的 Windows x64 桌面播放器。

准确地说，它是：

```text
FFmpeg + Qt 6 + C++17
```

其中：

- **FFmpeg** 负责媒体文件或网络流的解封装、音视频选流和解码。
- **Qt 6** 负责窗口、按钮、播放列表、菜单、事件循环、RHI 视频呈现、软件回退绘制和音频设备输出。
- **C++17** 负责播放器对象、线程、队列、状态管理和 FFmpeg C API 调用。

它没有使用 libVLC、GStreamer、Electron、C#、WPF 或 Python。

## 2. 技术栈总表

| 分类 | 技术 | 实际用途 |
| --- | --- | --- |
| 编程语言 | C++17 | 播放器主体、线程、队列、音视频管线和 UI 逻辑 |
| GUI 框架 | Qt 6 Widgets | 主窗口、工具栏、菜单、播放列表、进度条、音量条 |
| 图形显示 | Qt RHI / `QRhiWidget` | 在 Windows D3D11 上直接采样 VideoProcessor 输出的 GPU 纹理 |
| 软件回退 | Qt Gui / `QImage` / `QPainter` | 回读硬件帧或软件解码帧的等比绘制、旋转和字幕叠加 |
| 音频输出 | Qt Multimedia / `QAudioSink` | 将 PCM 数据发送到 Windows 默认音频设备 |
| 媒体解封装 | FFmpeg `libavformat` | 打开文件、URL、读取媒体包、发现音视频流 |
| 媒体解码 | FFmpeg `libavcodec` | H.264、HEVC、AAC 等音视频解码，支持 D3D11VA 与软件回退 |
| 公共工具 | FFmpeg `libavutil` | 帧、包、时间基、错误信息、像素和采样格式 |
| 硬件解码 | FFmpeg D3D11VA | 共享 Qt RHI 的 D3D11 设备，硬件帧进入零拷贝呈现路径 |
| 硬件色彩转换 | D3D11 VideoProcessor | 在 GPU 上把 NV12/P010 转为 BGRA 纹理 |
| 驱动同步 | `ID3D11Multithread`、递归锁 | 串行化解码、VideoProcessor 和 Qt RHI 对即时上下文的使用 |
| 音频重采样 | FFmpeg `libswresample` | 任意采样格式转为 Qt 可播放的 16 位 PCM |
| 视频像素转换 | FFmpeg `libswscale` | YUV 等像素格式转换为 `QImage` 使用的 BGRA |
| 并发 | C++ `std::thread`、`std::mutex`、`std::atomic`、条件变量 | 控制、音频解码和视频解码线程 |
| 定时刷新 | Qt `QTimer` | 定时刷新进度、时间文字和播放状态 |
| 国际化 | Qt `QTranslator` + 自定义词典 | 运行时切换英文/简体中文，`qtbase_zh_CN.qm` 翻译标准对话框 |
| 配置存储 | Qt `QSettings` | 保存音量、倍速、循环、随机、界面语言和窗口状态 |
| 构建系统 | CMake 3.24+ | 查找 Qt、链接 FFmpeg、复制运行时 DLL |
| 编译工具链 | MSVC 2022 + Ninja | 生成 Windows x64 Release/Debug 程序 |
| IDE | Visual Studio 2022 | 源码编辑、CMake 配置和断点调试 |
| 运行平台 | Windows 10/11 x64 | 已验证的运行环境 |

## 3. 核心原则：FFmpeg 负责媒体，Qt 负责交互和展示

可以把它理解成两层：

```text
┌───────────────────────────────────────────────┐
│ Qt 6 表现层                                    │
│ 主窗口、播放列表、菜单、QRhiWidget、QAudioSink │
└───────────────────────┬───────────────────────┘
                        │ 控制命令和媒体数据
┌───────────────────────▼───────────────────────┐
│ FFmpeg 播放内核                                │
│ libavformat -> libavcodec -> D3D11VA/sws/swr │
└───────────────────────┬───────────────────────┘
                        │
                 文件、文件夹、网络 URL
```

播放流程不是由 `QMediaPlayer` 完成的。实际链路是：

```text
媒体文件 / 网络 URL
        ↓
libavformat 解封装
        ↓
音视频 Packet 队列
        ↓
libavcodec 解码
        ├─ 硬件帧 -> D3D11 VideoProcessor -> QRhiWidget -> D3D11 呈现
        ├─ 回退帧 -> swscale -> QImage -> VideoCanvas -> QPainter
        └─ 音频帧 -> swresample -> PCM -> QAudioSink
```

## 4. 使用的 FFmpeg 库

工程内附带 FFmpeg 3.4 x64 SDK，主要使用以下库：

| FFmpeg 库 | 工程中的职责 |
| --- | --- |
| `libavformat` | `avformat_open_input`、`avformat_find_stream_info`、`av_read_frame`、seek |
| `libavcodec` | 查找解码器、发送 Packet、接收 Frame、flush 解码器 |
| `libavutil` | `AVPacket`、`AVFrame`、时间基、错误码、像素格式和采样格式 |
| `libswresample` | `swr_alloc_set_opts`、`swr_convert`，输出 16 位 PCM |
| `libswscale` | `sws_getContext`、`sws_scale`，输出 BGRA 图像 |

工程使用的核心 FFmpeg API 包括：

```cpp
avformat_open_input()
avformat_find_stream_info()
av_find_best_stream()
avcodec_find_decoder()
avcodec_open2()
av_read_frame()
avcodec_send_packet()
avcodec_receive_frame()
av_seek_frame()
avcodec_flush_buffers()
av_hwdevice_ctx_create()
av_hwframe_transfer_data()
sws_scale()
swr_convert()
```

## 5. 使用的 Qt 6 模块

### 5.1 Qt Core

负责：

- `QObject`、信号槽和主事件循环。
- `QString`、`QVector`、`QFile`、`QFileInfo`、`QDir`。
- `QTimer`。
- `QSettings`。
- `QTranslator` 和 `QEvent::LanguageChange`。
- 应用名称、组织名称和命令行参数。

### 5.2 Qt Widgets

负责：

- `QApplication`。
- `QMainWindow`。
- 菜单、工具栏、按钮、标签、滑动条。
- `QListWidget` 播放列表。
- `QSplitter` 主视频区和播放列表分栏。
- 文件选择、文件夹选择、消息框和右键菜单。

### 5.3 Qt Gui

负责：

- `QImage` 视频帧保存。
- `QPainter` 视频等比绘制。
- 字体、画笔和字幕叠加。
- 截图保存。

### 5.4 Qt Multimedia

本工程使用 Qt Multimedia 的原因不是调用 `QMediaPlayer`，而是使用：

- `QMediaDevices::defaultAudioOutput()` 查找默认输出设备。
- `QAudioSink` 输出 PCM。
- `QAudioFormat` 描述采样率、声道数和 `Int16` 采样格式。

换成更直接的话：

```text
FFmpeg 解码音频
    ↓
libswresample 重采样
    ↓
Qt QAudioSink 播放 PCM
```

## 6. 音视频同步和线程

程序有一个 Qt 主线程和三个播放工作线程：

| 线程 | 职责 |
| --- | --- |
| Qt 主线程 | 处理窗口、按钮、菜单、定时刷新和用户事件 |
| 控制线程 | 打开媒体、读取 Packet、分发到音视频队列、执行 Seek |
| 视频线程 | 从视频队列取 Packet，解码后转换并提交 `QImage` |
| 音频线程 | 从音频队列取 Packet，解码、重采样并写入 `QAudioSink` |

工作线程使用 C++ 标准库：

```cpp
std::thread
std::mutex
std::condition_variable
std::atomic
std::deque
```

Qt 的 `QThread` 只用于少量延时调用，例如 `QThread::msleep()`；播放线程本身是 `std::thread`。

媒体时钟负责控制音视频帧的播放时间。暂停、倍速、Seek 和逐帧都通过原子状态和条件变量通知工作线程。

## 7. 各源码文件的技术职责

| 文件 | 技术职责 |
| --- | --- |
| `main.cpp` | 创建 `QApplication`、设置应用信息并显示主窗口 |
| `MainWindow.h/.cpp` | Qt Widgets UI、播放列表、菜单、快捷键、设置和媒体操作 |
| `FFmpegPlayer.h/.cpp` | FFmpeg 解封装、解码、线程、队列、时钟、音频输出和视频帧提交 |
| `VideoCanvas.h/.cpp` | `QPainter` 绘制、等比缩放、字幕、拖放、滚轮和鼠标交互 |
| `RhiVideoSurface.h/.cpp` | `QRhiWidget`、D3D11 VideoProcessor、GPU 纹理呈现和后端失败回退 |
| `D3d11ContextSync.h` | FFmpeg D3D11VA 与 Qt RHI 共用即时上下文的递归锁和驱动同步 |
| `VideoOverlay.h/.cpp` | GPU 画面之上的字幕、OSD 和透明交互叠加层 |
| `StreamingPolicy.h/.cpp` | 网络协议识别、超时、重连和缓存水位策略 |
| `VideoFramePool.h/.cpp` | 三槽 `QImage` 帧池和跨线程存活保护 |
| `SubtitleTrack.h/.cpp` | SRT、VTT、ASS、SSA 文本解析和时间查询 |
| `Language.h/.cpp` | 英中词典、自定义 `QTranslator`、切换语言和系统默认语言判断 |
| `CMakeLists.txt` | 查找 Qt、链接 FFmpeg 库、复制 DLL、复制 `qtbase_zh_CN.qm`、调用 `windeployqt` |
| `FFmpegQtPlayer.pro` | qmake 备用构建入口 |
| `build.ps1` | 自动查找 Qt、VS、CMake 和 Ninja 并构建 |
| `run.ps1` | 构建并启动播放器 |

项目的正式 CMake 构建不包含开发目录中遗留的 `AudioPlayer.h`、`FileDecode.cpp` 等旧实验文件。正式编译入口以 `CMakeLists.txt` 中的源文件列表为准。

## 8. 视频显示技术

Windows 正常路径不是把 GPU 帧回读到内存，而是：

```text
FFmpeg D3D11VA 解码
    ↓
AVFrame.data[0] = ID3D11Texture2D*
    ↓
D3D11 VideoProcessor 把 NV12/P010 转为 BGRA RHI 纹理
    ↓
QRhiWidget 使用自定义图形管线采样该纹理
    ↓
Qt RHI endOffscreenFrame 提交 D3D11 命令
```

关键类：

- `RhiVideoSurface` 继承 `QRhiWidget`，并强制使用 `Direct3D11`。
- `QRhiD3D11NativeHandles` 提供 Qt RHI 实际使用的 `ID3D11Device` 和 `ID3D11DeviceContext`。
- `AVD3D11VADeviceContext` 复用同一个 D3D11 设备，让解码纹理可以直接被 VideoProcessor 读取。
- `ID3D11VideoProcessorEnumerator`、`ID3D11VideoProcessor`、输入视图和输出视图完成色彩空间转换和缩放。
- `GpuVideoFrame` 持有克隆的 `AVFrame`，保证纹理在使用期间不会被 FFmpeg 帧池回收。

同步与回退：

- FFmpeg 的 `lock/unlock` 回调与 `RhiVideoSurface::paintEvent()` 共用递归互斥量。
- 初始化时对 D3D11 设备启用 `ID3D11Multithread::SetMultithreadProtected(TRUE)`。
- `QRhiWidget` 渲染失败时停止提交 GPU 帧，并回退到 `av_hwframe_transfer_data -> sws_scale -> QImage` 或软件解码。
- `gpuFramesPresented`、`gpuFramesRendered` 和 `hardwareReadbacks` 用于区分零拷贝、回读和软件路径。

`VideoCanvas` 仍负责画面比例、旋转、拖放和字幕布局；`VideoOverlay` 是透明子控件，保证字幕与 OSD 能叠加在 `QRhiWidget` 上。软件回退路径继续使用 `QImage::Format_ARGB32` 和 `QPainter`。

## 9. 音频输出技术

音频线程使用 `libswresample` 将各种输入采样格式转换为：

```text
采样格式：Signed 16-bit PCM
声道：优先双声道
采样率：固定为源采样率（或设备首选采样率）
```

倍速由 FFmpeg 滤镜图完成：

```text
abuffer -> atempo -> abuffersink
```

`atempo` 做时间伸缩而保持音高。0.25x 至 4x 超出单级滤镜稳定范围时，播放器会串联两级 `atempo`。`swresample` 只负责采样格式、声道和采样率转换，不再通过伪造输入采样率制造变调。

然后通过 `QAudioSink::start()` 得到 `QIODevice`，把 PCM 数据写入输出设备。

暂停时调用 `QAudioSink::suspend()`，恢复时调用 `resume()`。音量使用 0 至 125 的播放器音量值，静音通过音频缩放实现。

## 10. 播放列表和界面技术

界面完全使用 Qt Widgets 创建，没有使用 QML，也没有使用 WebView。

主要 Qt 类包括：

```text
QApplication
QMainWindow
QWidget
QAction
QMenu
QToolBar
QToolButton
QPushButton
QSlider
QComboBox
QLabel
QLineEdit
QListWidget
QSplitter
QTimer
QSettings
QFileDialog
QMessageBox
QFontComboBox
QColorDialog
QSpinBox
QPlainTextEdit
QDialogButtonBox
```

播放列表支持：

- 文件、文件夹和 URL。
- 多选和拖放。
- M3U、M3U8、PLS 导入。
- M3U 导出。
- 上一项、下一项。
- 循环和随机播放。
- 按标题或路径搜索过滤。
- 按标题或路径排序。
- 一键移除重复条目。

播放控制额外支持：

- 跳转到指定时间（秒、`MM:SS`、`HH:MM:SS`）。
- A-B 区间循环：主窗口 100 ms 定时器检测越过 B 点后跳回 A 点。
- 单文件续播：按目标路径记录位置，写入 `resume/items`，打开文件时自动应用。
- 后退单帧：暂停状态下按 `1000 / fps` 回退位置，复用暂停预览机制显示目标帧。

### 10.1 界面语言

界面文本以英文原文为键，工程内不生成 `.ts` 文件，也不需要 `lrelease`：

- `Language.cpp` 内置 `英文原文 → 中文` 词典表。
- 中文模式安装两个 `QTranslator`：自定义词典翻译器先安装、`qtbase_zh_CN.qm` 后安装，查询时词典优先；词典对未知字符串返回空串，于是标准对话框按钮回落到 Qt 自带翻译。
- 切换语言只调用 `QCoreApplication::installTranslator()` / `removeTranslator()`，Qt 自动向所有部件发送 `QEvent::LanguageChange`。
- 构造界面时 `MainWindow::recordUiTexts()` 记录所有 `QAction`、`QPushButton`、`QLabel`、`QGroupBox` 的英文原文；`retranslateUi()` 在语言变化时按词典重新赋值。
- 运行时生成的文本（状态栏、播放速度、错误信息）直接调用 `tr()`，由已安装的翻译器即时翻译。
- 语言选择写入 `settings.ini` 的 `ui/language`；首次启动按 `QLocale::system()` 决定。

### 10.2 主题、迷你模式与快捷键

- 主题：`configureStyle()` 根据 `darkTheme_` 选择两套完整 Style Sheet，`applyTheme()` 同时刷新播放列表文字颜色。选择写入 `ui/theme`。
- 迷你播放器：隐藏菜单栏、状态栏和播放列表面板，保存原窗口几何与置顶状态，强制 `Qt::WindowStaysOnTopHint` 并缩到 560×340；退出时恢复几何和原置顶状态。关闭程序时若仍在迷你模式，保存的是进入前的几何与置顶值。
- 快捷键总览：`showShortcuts()` 用只读 `QPlainTextEdit` 列出分组快捷键，文本通过 `tr()` / 词典翻译。
- 媒体信息：`QDialog` + 只读 `QPlainTextEdit` 显示完整文本，并提供“复制到剪贴板”按钮。
- 字幕样式对话框：`QFontComboBox`、颜色按钮、`QSpinBox`（描边宽度 0–8）和透明度滑块；控件变化时直接调用 `VideoCanvas::setSubtitleStyle()` 实时预览，取消时恢复原样式。

## 11. 字幕技术

字幕解析由工程自己的 `SubtitleTrack` 实现，不依赖 libVLC 或 FFmpeg 字幕滤镜。

支持：

- SRT。
- WebVTT。
- ASS。
- SSA。

解析结果保存为带开始时间、结束时间和文本的字幕条目。播放位置变化时，由主窗口查询当前字幕文本，再交给 `VideoCanvas` 使用 `QPainter` 绘制。

字幕样式由 `VideoCanvas::SubtitleStyle` 描述：

```text
fontFamily / textColor
outlineColor / outlineWidth
backgroundColor / backgroundOpacity
```

`VideoCanvas::drawSubtitle()` 先按背景不透明度画圆角底板，再以描边颜色在 `±outlineWidth` 偏移处重复绘制文字，最后绘制正文颜色。字体缺省为 `Segoe UI`，字号按画面宽度和 `subtitleScale` 计算。样式由主窗口的字幕样式对话框修改并写入 `settings.ini`。

ASS/SSA 当前只提取文本和时间，不实现完整特效、定位、动画和图形渲染。

## 12. 构建与部署技术

已验证构建组合：

```text
Windows 10/11 x64
Visual Studio 2022 / MSVC v143
CMake 3.24+
Ninja
Qt 6.11.1 msvc2022_64
FFmpeg 3.4 x64
```

CMake 负责：

1. `find_package(Qt6 ...)`。
2. 链接 `Qt6::Core`、`Qt6::Gui`、`Qt6::Widgets`、`Qt6::Multimedia`。
3. 链接 `avformat`、`avcodec`、`avutil`、`swresample`、`swscale`。
4. 构建后复制 FFmpeg DLL。
5. 复制此 MinGW 版 FFmpeg 依赖的 `libbz2-1.dll`、`libiconv-2.dll`、`liblzma-5.dll`、`libwinpthread-1.dll` 和 `zlib1.dll`。
6. 把 Qt 安装目录下的 `qtbase_zh_CN.qm` 复制到输出目录的 `translations/`。
7. 调用 `windeployqt` 部署 Qt DLL 和插件。

便携版还附带 `vc_redist.x64.exe`，用于安装 MSVC 运行库。

## 13. 这个项目没有使用的技术

| 技术 | 是否使用 | 说明 |
| --- | --- | --- |
| libVLC | 否 | 只参考 `vlc-re` 的桌面产品功能，不链接 VLC |
| VLC 插件体系 | 否 | 没有复制 VLC 插件或官方全部功能 |
| `QMediaPlayer` | 否 | 播放核心直接调用 FFmpeg API |
| GStreamer | 否 | 不使用系统 GStreamer |
| DirectShow | 否 | 不通过 DirectShow 解码 |
| Electron / WebView | 否 | 界面是原生 Qt Widgets |
| C# / WPF / WinForms | 否 | 主体是 C++17 |
| Python | 否 | Python 不参与播放器运行 |
| D3D11VA / Qt RHI 零拷贝 | 是 | Windows 正常路径；失败时回退硬件回读或软件解码 |
| CUDA / VideoToolbox / VAAPI | 否 | 当前只实现 Windows D3D11 后端 |
| QML | 否 | 当前 UI 使用 Qt Widgets |

## 14. 与 `vlc-re` 的关系

`vlc-re` 的产品层使用 libVLC 提供播放能力。本工程借鉴的是它的桌面播放器功能范围，例如：

- 文件和文件夹打开。
- URL 播放。
- 播放列表和格式导入导出。
- 播放、暂停、停止、上一项、下一项。
- 循环、随机、倍速、音量和静音。
- 全屏、置顶和截图。
- 外挂字幕、拖放、快捷键和设置持久化。

媒体引擎由本工程使用 FFmpeg 和 Qt 独立实现。因此不能把它描述为“VLC 换皮”或“libVLC 播放器”。

## 15. 当前技术边界

- 零拷贝后端目前只覆盖 Windows D3D11；其他平台需要分别实现 D3D12、VideoToolbox、VAAPI 或 Vulkan 互操作。
- HDR 静态/动态元数据、HDR tone mapping 和广色域映射尚未专项验证；P010 纹理可以进入 VideoProcessor，但不能据此宣称完整 HDR 支持。
- 1080p 已通过自动化零拷贝 smoke；4K 长稳、多 GPU、远程桌面和不同驱动版本仍需扩大测试矩阵。
- 同一时刻只解码一条视频流；音轨和字幕轨可以在当前位置热切换，不需要重新打开媒体。
- 内嵌字幕只支持 FFmpeg 能还原为文本的编码（mov_text、subrip、ass/ssa、webvtt 等），PGS/VobSub 等图形字幕不显示。
- 倍速通过 `atempo` 保持音高，但极端倍速下的音质仍受源内容和滤镜参数影响。
- ASS/SSA 只支持基础文本显示，不支持完整特效。
- 视频旋转按 display matrix 处理后缓存旋转图像，没有做额外的旋转后缩放优化。
- 真实 RTSP/HLS 长时弱网、30 分钟断网恢复和 CDN 切换尚未形成完整测试矩阵。
- 不使用 libVLC 的插件、均衡器、视频滤镜、录像和投屏能力。

## 16. 调试入口建议

使用 Visual Studio 2022 调试时，可以重点关注：

```text
MainWindow::openMediaFile
MainWindow::updateUi
FFmpegPlayer::run
FFmpegPlayer::readPackets
FFmpegPlayer::audioLoop
FFmpegPlayer::videoLoop
FFmpegPlayer::handleSeekLocked
FFmpegPlayer::waitUntil
VideoCanvas::paintEvent
RhiVideoSurface::paintEvent
RhiVideoSurface::render
FFmpegPlayer::openVideoDecoder
```

相关技术细节和逐函数说明见 [CODE-WALKTHROUGH.md](CODE-WALKTHROUGH.md)。

## 17. 最终结论

这个项目的准确定位是：

```text
基于 C++17 的原生 Windows 桌面播放器
界面和音频输出使用 Qt 6
解封装和音视频解码使用 FFmpeg 3.4
视频正常路径使用 D3D11VA + VideoProcessor + Qt RHI
软件回退使用 QImage + QPainter
音频输出使用 QAudioSink
构建使用 CMake + MSVC 2022
不使用 libVLC
```

因此，用 **FFmpeg + Qt** 来描述它是准确的。

#include "Language.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include <iterator>

namespace {

struct Entry
{
    const char* english;
    const char* chinese;
};

// Translations are keyed by the English source text used in tr() calls, which
// keeps the lookup context independent and lets runtime messages translate as
// well as the texts created while building the window.
const Entry kEntries[] = {
    // Window and About
    {"FFmpeg Qt Player", "FFmpeg Qt 播放器"},
    {"%1 - FFmpeg Qt Player", "%1 - FFmpeg Qt 播放器"},
    {"About FFmpeg Qt Player", "关于 FFmpeg Qt 播放器"},
    {"FFmpeg Qt Player\n\n"
     "A Qt 6 desktop player whose demuxing, decoding, resampling, scaling and clock "
     "control are implemented with FFmpeg.\n\n"
     "FFmpeg libavformat %1.%2\n"
     "FFmpeg libavcodec %3.%4\n"
     "FFmpeg libavutil %5.%6\n"
     "Build: %7",
     "FFmpeg Qt 播放器\n\n"
     "一个使用 FFmpeg 完成解封装、解码、重采样、缩放和时钟控制的 Qt 6 桌面播放器。\n\n"
     "FFmpeg libavformat %1.%2\n"
     "FFmpeg libavcodec %3.%4\n"
     "FFmpeg libavutil %5.%6\n"
     "构建时间：%7"},

    // Menus
    {"File", "文件"},
    {"Playback", "播放"},
    {"Playlist", "播放列表"},
    {"Audio", "音频"},
    {"Video", "视频"},
    {"Subtitle", "字幕"},
    {"Tools", "工具"},
    {"Help", "帮助"},
    {"Language", "语言"},

    // File menu
    {"Open Files...", "打开文件..."},
    {"Add Files...", "添加文件..."},
    {"Open Folder...", "打开文件夹..."},
    {"Open Network Stream...", "打开网络流..."},
    {"Recent Files", "最近打开"},
    {"Open Playlist...", "打开播放列表..."},
    {"Save Playlist...", "保存播放列表..."},
    {"Exit", "退出"},
    {"No recent files", "没有最近打开的文件"},
    {"Clear Recent Files", "清空最近打开"},
    {"Recent files cleared", "已清空最近打开记录"},

    // Playback menu
    {"Play / Pause", "播放 / 暂停"},
    {"Next Video Frame", "下一视频帧"},
    {"Stop", "停止"},
    {"Previous", "上一项"},
    {"Next", "下一项"},
    {"Back 5 seconds", "后退 5 秒"},
    {"Forward 5 seconds", "前进 5 秒"},
    {"Playback Speed", "播放速度"},
    {"Slower", "减速"},
    {"Faster", "加速"},
    {"Normal Speed", "正常速度"},
    {"Loop Playlist", "循环播放列表"},
    {"Shuffle Playlist", "随机播放列表"},
    {"Previous Video Frame", "上一视频帧"},
    {"Go to Time", "跳转到时间"},
    {"Go to Time...", "跳转到时间..."},
    {"Enter a time (MM:SS or seconds):", "输入时间（MM:SS 或秒数）："},
    {"Chapters", "章节"},
    {"Previous Chapter", "上一章节"},
    {"Next Chapter", "下一章节"},
    {"No chapters", "没有章节"},
    {"Last chapter", "已经是最后一章"},
    {"Chapter %1", "章节 %1"},
    {"Chapter %1  %2", "章节 %1  %2"},
    {"Invalid time.", "时间格式无效。"},
    {"Jumped to %1", "已跳转到 %1"},
    {"A-B Repeat", "A-B 循环"},
    {"Set A Point", "设置 A 点"},
    {"Set B Point", "设置 B 点"},
    {"Clear A-B Repeat", "清除 A-B 循环"},
    {"Set the A point first.", "请先设置 A 点。"},
    {"The B point must be after the A point.", "B 点必须在 A 点之后。"},
    {"A point set at %1", "A 点已设在 %1"},
    {"B point set at %1", "B 点已设在 %1"},
    {"A: %1", "A：%1"},
    {"B: %1", "B：%1"},
    {"A-B repeat cleared", "已清除 A-B 循环"},
    {"A-B %1-%2", "A-B %1-%2"},
    {"Stepping back one video frame", "已后退一帧"},
    {"Resumed at %1", "从 %1 继续播放"},

    // Playlist menu and panel
    {"Play Selected", "播放选中项"},
    {"Remove Selected", "移除选中项"},
    {"Clear Playlist", "清空播放列表"},
    {"Play selected item", "播放选中项"},
    {"Remove selected item", "移除选中项"},
    {"Save playlist", "保存播放列表"},
    {"Remove", "移除"},
    {"Clear", "清空"},
    {"%1 item(s)", "%1 项"},
    {"%1 / %2 item(s)", "%1 / %2 项"},
    {"Search Playlist", "搜索播放列表"},
    {"Search playlist...", "搜索播放列表..."},
    {"Sort Playlist", "排序播放列表"},
    {"Sort by Title", "按标题排序"},
    {"Sort by Path", "按路径排序"},
    {"Remove Duplicates", "移除重复项"},
    {"Removed %1 duplicate(s)", "已移除 %1 个重复项"},
    {"No duplicate items found", "没有发现重复项"},
    {"Playlist sorted by title", "播放列表已按标题排序"},
    {"Playlist sorted by path", "播放列表已按路径排序"},

    // Audio menu and transport bar
    {"Mute", "静音"},
    {"Volume Up", "增大音量"},
    {"Volume Down", "减小音量"},
    {"Volume 50%", "音量 50%"},
    {"Volume 100%", "音量 100%"},
    {"Volume 125%", "音量 125%"},
    {"Audio Track", "音轨"},
    {"Mute (M)", "静音 (M)"},
    {"Volume: 0-125%", "音量：0-125%"},
    {"Playback speed", "播放速度"},
    {"Playback speed: %1x", "播放速度：%1x"},
    {"Speed %1x", "倍速 %1x"},
    {"Volume %1%", "音量 %1%"},
    {"Muted", "已静音"},
    {"Previous (P)", "上一项 (P)"},
    {"Play / pause (Space)", "播放 / 暂停 (空格)"},
    {"Back 5 seconds (Left)", "后退 5 秒 (←)"},
    {"Forward 5 seconds (Right)", "前进 5 秒 (→)"},
    {"Next (N)", "下一项 (N)"},
    {"Stop (S)", "停止 (S)"},
    {"Loop", "循环"},
    {"Loop playlist", "循环播放列表"},
    {"Shuffle", "随机"},
    {"Shuffle playlist", "随机播放列表"},

    // Video menu
    {"Fullscreen", "全屏"},
    {"Exit Fullscreen", "退出全屏"},
    {"Always on Top", "窗口置顶"},
    {"Take Snapshot", "截图"},
    {"Rotate Clockwise", "顺时针旋转"},
    {"Rotate Counterclockwise", "逆时针旋转"},
    {"Reset Rotation", "重置旋转"},
    {"Rotation: %1°", "旋转角度：%1°"},
    {"Rotation %1°", "旋转 %1°"},
    {"Rotation reset", "旋转已重置"},
    {"Aspect Ratio", "画面比例"},
    {"Fit to Window", "适应窗口"},
    {"Fill and Crop", "填充裁剪"},
    {"Stretch", "拉伸"},
    {"Original Size", "原始大小"},
    {"Fit to window", "适应窗口"},
    {"Fill and crop", "填充裁剪"},
    {"Original size", "原始大小"},
    {"Aspect ratio: %1", "画面比例：%1"},
    {"Back 5 Seconds", "后退 5 秒"},
    {"Forward 5 Seconds", "前进 5 秒"},
    {"HLS Quality", "HLS 清晰度"},
    {"No HLS variants", "没有可切换的 HLS 码率"},
    {"HLS quality: %1", "HLS 清晰度：%1"},
    {"HLS quality: Auto", "HLS 清晰度：自动"},

    // Subtitle menu
    {"Load Subtitle File...", "加载字幕文件..."},
    {"Show Subtitles", "显示字幕"},
    {"Embedded Subtitle Track", "内嵌字幕轨"},
    {"Subtitle Delay", "字幕延迟"},
    {"Delay -100 ms", "延迟 -100 毫秒"},
    {"Reset Delay", "重置延迟"},
    {"Delay +100 ms", "延迟 +100 毫秒"},
    {"Subtitle Size", "字幕大小"},
    {"Smaller", "减小"},
    {"Default Size", "默认大小"},
    {"Larger", "增大"},
    {"Subtitle Position", "字幕位置"},
    {"Lower", "下移"},
    {"Default Position", "默认位置"},
    {"Higher", "上移"},
    {"Include Subtitles in Snapshot", "截图包含字幕"},
    {"Load Subtitle...", "加载字幕..."},
    {"Subtitle loaded: %1", "字幕已加载：%1"},
    {"Subtitle delay reset", "字幕延迟已重置"},
    {"Subtitle delay 0 ms", "字幕延迟 0 毫秒"},
    {"Subtitle delay: %1 ms", "字幕延迟：%1 毫秒"},
    {"Subtitle delay %1 ms", "字幕延迟 %1 毫秒"},
    {"Subtitle size 100%", "字幕大小 100%"},
    {"Subtitle size: %1%", "字幕大小：%1%"},
    {"Subtitle size %1%", "字幕大小 %1%"},
    {"Subtitle position reset", "字幕位置已重置"},
    {"Subtitle position %1%", "字幕位置 %1%"},
    {"Subtitles hidden", "字幕已隐藏"},
    {"Subtitles shown", "字幕已显示"},
    {"Subtitle Style", "字幕样式"},
    {"Subtitle Style...", "字幕样式..."},
    {"Font", "字体"},
    {"Text Color", "文字颜色"},
    {"Outline Color", "描边颜色"},
    {"Outline Width", "描边宽度"},
    {"Background Color", "背景颜色"},
    {"Background Opacity", "背景不透明度"},
    {"Subtitle style updated", "字幕样式已更新"},
    {"Copy Current Subtitle", "复制当前字幕"},
    {"Subtitle copied", "已复制字幕"},
    {"Subtitle copied to the clipboard", "字幕已复制到剪贴板"},
    {"No subtitle is showing right now.", "当前没有显示字幕。"},

    // Tools and Help
    {"Media Information", "媒体信息"},
    {"Media information", "媒体信息"},
    {"Export Diagnostics...", "导出诊断..."},
    {"Export Diagnostics", "导出诊断"},
    {"Open Crash Reports Folder", "打开崩溃报告目录"},
    {"Text files (*.txt)", "文本文件 (*.txt)"},
    {"All files (*)", "所有文件 (*)"},
    {"Diagnostics exported: %1", "诊断信息已导出：%1"},
    {"Diagnostics path is empty.", "诊断文件路径为空。"},
    {"Cannot write diagnostics: %1", "无法写入诊断信息：%1"},
    {"Cannot commit diagnostics: %1", "无法保存诊断信息：%1"},
    {"Buffering...", "正在缓冲..."},
    {"Network stalled, retrying...", "网络连接停滞，正在重试..."},
    {"Reconnecting...", "正在重新连接..."},
    {"AI Speech and Video Recognition...", "AI 语音与画面识别..."},
    {"AI Recognition (Running)...", "AI 识别（运行中）..."},
    {"AI Speech and Video Recognition", "AI 语音与画面识别"},
    {"AI Service", "AI 服务"},
    {"API Base URL", "API 基础地址"},
    {"API Key", "API 密钥"},
    {"Transcription Model", "语音转写模型"},
    {"Summary / Vision Model", "摘要 / 画面模型"},
    {"Core Content Model", "核心内容模型"},
    {"Audio Language", "音频语言"},
    {"Output Language", "输出语言"},
    {"Vision Model", "画面识别模型"},
    {"Audio Chunk", "音频分块"},
    {"Frame Interval", "画面间隔"},
    {"Speech recognition", "语音识别"},
    {"Video frame recognition", "视频画面识别"},
    {"Live Transcript", "实时转写"},
    {"Core Content", "核心内容"},
    {"No transcript yet", "尚无转写内容"},
    {"Core content will appear here", "核心内容将显示在这里"},
    {"Not started", "尚未启动"},
    {"Start Recognition", "开始识别"},
    {"Auto Detect", "自动检测"},
    {"Chinese", "中文"},
    {"English", "英语"},
    {"Simplified Chinese", "简体中文"},
    {" s", " 秒"},
    {"Error: %1", "错误：%1"},
    {"AI recognition is running", "AI 识别正在运行"},
    {"AI recognition is listening", "AI 识别正在监听"},
    {"AI recognition stopped", "AI 识别已停止"},
    {"Open media before starting AI recognition.", "请先打开媒体再启动 AI 识别。"},
    {"Enable audio recognition or video recognition first.", "请先启用语音识别或视频画面识别。"},
    {"The current media has no audio or video stream to recognize.",
     "当前媒体没有可供识别的音频或视频流。"},
    {"Enter a valid OpenAI-compatible API base URL.",
     "请输入有效的 OpenAI 兼容 API 基础地址。"},
    {"The AI audio queue is full; one segment was skipped.",
     "AI 音频队列已满，已跳过一个片段。"},
    {"Transcribing audio at %1", "正在转写 %1 处的音频"},
    {"[%1-%2] %3", "【%1-%2】%3"},
    {"[%1] %2", "【%1】%2"},
    {"[Video %1] %2", "【画面 %1】%2"},
    {"Audio segment recognized", "音频片段识别完成"},
    {"Audio segments and sampled video frames are sent to the configured service; "
     "usage may incur API charges. The API key is saved locally in plain text.",
     "音频片段和抽样视频画面会发送到所配置的服务，使用 API 可能产生费用。"
     "API 密钥会以明文形式保存在本机设置中。"},
    {"Analyzing the current video frame", "正在分析当前视频画面"},
    {"Video frame recognized", "视频画面识别完成"},
    {"Updating core content", "正在更新核心内容"},
    {"Core content updated", "核心内容已更新"},
    {"AI service returned HTTP %1", "AI 服务返回 HTTP %1"},
    {"Reveal Current File", "在文件夹中显示"},
    {"Copy to Clipboard", "复制到剪贴板"},
    {"File size: %1", "文件大小：%1"},
    {"Light Theme", "浅色主题"},
    {"Light theme", "浅色主题"},
    {"Dark theme", "深色主题"},
    {"Mini Player", "迷你播放器"},
    {"Mini player enabled", "已进入迷你播放器"},
    {"Mini player disabled", "已退出迷你播放器"},
    {"mini player", "迷你播放器"},
    {"Sleep Timer", "睡眠定时器"},
    {"%1 minutes", "%1 分钟"},
    {"15 minutes", "15 分钟"},
    {"30 minutes", "30 分钟"},
    {"45 minutes", "45 分钟"},
    {"60 minutes", "60 分钟"},
    {"90 minutes", "90 分钟"},
    {"Custom Timer...", "自定义时间..."},
    {"Cancel Sleep Timer", "取消睡眠定时器"},
    {"Minutes:", "分钟数："},
    {" min", " 分钟"},
    {"Sleep timer: %1", "睡眠定时：%1"},
    {"sleep in %1", "%1 后停止"},
    {"Sleep timer finished", "睡眠定时已到，播放已暂停"},
    {"Sleep timer cancelled", "睡眠定时已取消"},
    {"Stop after:", "停止时间："},
    {"Keyboard Shortcuts", "键盘快捷键"},
    {"About", "关于"},

    // Status bar and player states
    {"Ready", "就绪"},
    {"No media loaded", "未加载媒体"},
    {"Opening media...", "正在打开媒体..."},
    {"Opened %1 stream(s)", "已打开 %1 个流"},
    {"Decoding video...", "正在解码视频..."},
    {"Audio playback", "音频播放"},
    {"Stopped", "已停止"},
    {"Paused", "已暂停"},
    {"Playback finished", "播放结束"},
    {"Playback error", "播放错误"},
    {"Open media to begin", "打开媒体开始播放"},
    {"loop on", "循环开"},
    {"loop off", "循环关"},
    {"shuffle on", "随机开"},
    {"shuffle off", "随机关"},
    {"topmost", "置顶"},
    {"normal window", "普通窗口"},
    {"chapter %1", "章节 %1"},
    {"video", "视频"},
    {"audio", "音频"},
    {"End of playlist.", "已到播放列表末尾。"},
    {"Start of playlist.", "已到播放列表开头。"},
    {"The current media has no video stream.", "当前媒体没有视频流。"},
    {"Pause playback before stepping frames.", "请先暂停再逐帧前进。"},
    {"Stepping one video frame", "已前进一帧"},
    {"Added %1 item(s) to the playlist.", "已向播放列表添加 %1 项。"},
    {"The playlist is empty.", "播放列表为空。"},
    {"Playlist saved: %1", "播放列表已保存：%1"},
    {"There is no video frame to save.", "当前没有可保存的视频画面。"},
    {"Snapshot saved: %1", "截图已保存：%1"},
    {"No supported media was found.", "没有找到支持的媒体文件。"},
    {"No usable entries were found in %1", "%1 中没有可用条目。"},
    {"The playlist contains no usable entries.", "播放列表中没有可用条目。"},
    {"No media has been opened.", "尚未打开媒体。"},
    {"Source: %1", "来源：%1"},

    // Track menus
    {"Auto", "自动"},
    {"Automatic", "自动"},
    {"None", "无"},
    {"No audio tracks", "没有音轨"},
    {"No embedded subtitle tracks", "没有内嵌字幕轨"},
    {"Track #%1  %2", "轨道 #%1  %2"},
    {"Audio track changed", "已切换音轨"},
    {"Embedded subtitle track changed", "已切换内嵌字幕轨"},

    // Dialogs
    {"Open media files", "打开媒体文件"},
    {"Add media files", "添加媒体文件"},
    {"Open media folder", "打开媒体文件夹"},
    {"Open network stream", "打开网络流"},
    {"Network URL", "网络地址"},
    {"Enter a complete URL with a network scheme.", "请输入包含网络协议的完整 URL。"},
    {"Open playlist", "打开播放列表"},
    {"Load subtitle", "加载字幕"},
    {"Snapshot", "截图"},
    {"Cannot save the image to %1", "无法将图片保存到 %1"},
    {"Media files (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.m2ts *.m4v *.webm *.wmv "
     "*.mpg *.mpeg *.mp3 *.aac *.wav *.flac *.ogg *.opus *.m4a *.wma *.ape)",
     "媒体文件 (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.m2ts *.m4v *.webm *.wmv "
     "*.mpg *.mpeg *.mp3 *.aac *.wav *.flac *.ogg *.opus *.m4a *.wma *.ape)"},
    {"Video files (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.m2ts *.m4v *.webm *.wmv "
     "*.mpg *.mpeg)",
     "视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.m2ts *.m4v *.webm *.wmv "
     "*.mpg *.mpeg)"},
    {"Audio files (*.mp3 *.aac *.wav *.flac *.ogg *.opus *.m4a *.wma *.ape)",
     "音频文件 (*.mp3 *.aac *.wav *.flac *.ogg *.opus *.m4a *.wma *.ape)"},
    {"All files (*.*)", "所有文件 (*.*)"},
    {"M3U playlist (*.m3u);;All files (*.*)", "M3U 播放列表 (*.m3u);;所有文件 (*.*)"},
    {"Playlists (*.m3u *.m3u8 *.pls);;All files (*.*)",
     "播放列表 (*.m3u *.m3u8 *.pls);;所有文件 (*.*)"},
    {"Subtitles (*.srt *.ass *.ssa *.vtt);;All files (*.*)",
     "字幕文件 (*.srt *.ass *.ssa *.vtt);;所有文件 (*.*)"},

    // Player errors and media information
    {"Cannot allocate the FFmpeg format context.", "无法分配 FFmpeg 格式上下文。"},
    {"Cannot open media: %1", "无法打开媒体：%1"},
    {"Cannot read stream information: %1", "无法读取流信息：%1"},
    {"The media contains no playable audio or video stream.", "该媒体没有可播放的音视频流。"},
    {"Media read stopped: %1", "媒体读取已中断：%1"},
    {"No audio output device is available.", "没有可用的音频输出设备。"},
    {"The default audio device does not support 16-bit PCM output.",
     "默认音频设备不支持 16 位 PCM 输出。"},
    {"Cannot initialize the FFmpeg audio resampler.", "无法初始化 FFmpeg 音频重采样器。"},
    {"Seek failed: %1", "跳转失败：%1"},
    {"Cannot read subtitle file: %1", "无法读取字幕文件：%1"},
    {"Cannot decode subtitle text.", "无法解码字幕文本。"},
    {"No subtitle cues were found in the selected file.", "所选文件中没有字幕内容。"},
    {"Container: %1", "容器：%1"},
    {"unknown", "未知"},
    {"Video: %1  %2 x %3  %4 fps%5", "视频：%1  %2 x %3  %4 fps%5"},
    {"  rotated %1°", "  旋转 %1°"},
    {"Audio: %1  %2 Hz  %3 channel(s)", "音频：%1  %2 Hz  %3 声道"},
    {"Embedded subtitle: %1", "内嵌字幕：%1"},
    {"Duration: %1", "时长：%1"},
    {"Bitrate: %1 kb/s", "码率：%1 kb/s"},
    {"Streams: %1", "流数量：%1"},
    {"Chapters: %1", "章节数：%1"},
    {"Pixel format: %1", "像素格式：%1"},
    {"Video bitrate: %1 kb/s", "视频码率：%1 kb/s"},
    {"Audio bitrate: %1 kb/s", "音频码率：%1 kb/s"},
    {"Title: %1", "标题：%1"},
    {"Artist: %1", "艺术家：%1"},
    {"16:9", "16:9"},
    {"4:3", "4:3"},
    {" + ", " + "},
    {"%1 ch", "%1 声道"},
};

const QHash<QString, QString>& dictionary()
{
    static const QHash<QString, QString> table = [] {
        QHash<QString, QString> values;
        values.reserve(static_cast<int>(std::size(kEntries)));
        for (const Entry& entry : kEntries) {
            values.insert(QString::fromUtf8(entry.english), QString::fromUtf8(entry.chinese));
        }
        return values;
    }();
    return table;
}

class DictionaryTranslator final : public QTranslator
{
public:
    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation, int n) const override
    {
        Q_UNUSED(context)
        Q_UNUSED(disambiguation)
        Q_UNUSED(n)
        if (!sourceText) {
            return {};
        }
        // Unknown texts must return an empty string so QCoreApplication can
        // fall back to the Qt base translation installed next to this one.
        return dictionary().value(QString::fromUtf8(sourceText));
    }
};

UiLanguage g_current = UiLanguage::English;
DictionaryTranslator g_dictionaryTranslator;
QTranslator g_qtTranslator;
bool g_qtTranslatorLoaded = false;
bool g_qtTranslatorInstalled = false;
bool g_translatorsInstalled = false;

} // namespace

QString UiLanguageSupport::dictionaryText(const QString& english)
{
    if (g_current != UiLanguage::Chinese) {
        return english;
    }
    return dictionary().value(english, english);
}

UiLanguage UiLanguageSupport::fromCode(const QString& code)
{
    return code.compare(QStringLiteral("zh"), Qt::CaseInsensitive) == 0
        ? UiLanguage::Chinese
        : UiLanguage::English;
}

QString UiLanguageSupport::code(UiLanguage language)
{
    return language == UiLanguage::Chinese ? QStringLiteral("zh") : QStringLiteral("en");
}

UiLanguage UiLanguageSupport::systemDefault()
{
    return QLocale::system().language() == QLocale::Chinese
        ? UiLanguage::Chinese
        : UiLanguage::English;
}

void UiLanguageSupport::setCurrent(UiLanguage language)
{
    g_current = language;
    if (language == UiLanguage::English) {
        if (!g_translatorsInstalled) {
            return;
        }
        QCoreApplication::removeTranslator(&g_dictionaryTranslator);
        if (g_qtTranslatorInstalled) {
            QCoreApplication::removeTranslator(&g_qtTranslator);
            g_qtTranslatorInstalled = false;
        }
        g_translatorsInstalled = false;
        return;
    }

    if (g_translatorsInstalled) {
        return;
    }

    if (!g_qtTranslatorLoaded) {
        const QStringList candidates = {
            QCoreApplication::applicationDirPath()
                + QStringLiteral("/translations/qtbase_zh_CN.qm"),
            QLibraryInfo::path(QLibraryInfo::TranslationsPath)
                + QStringLiteral("/qtbase_zh_CN.qm")
        };
        for (const QString& candidate : candidates) {
            if (QFileInfo::exists(candidate) && g_qtTranslator.load(candidate)) {
                g_qtTranslatorLoaded = true;
                break;
            }
        }
    }
    // Installed before the dictionary translator: the dictionary is consulted
    // first and returns empty for Qt's own strings, which then fall through to
    // qtbase_zh_CN.qm for standard dialog buttons.
    if (g_qtTranslatorLoaded) {
        QCoreApplication::installTranslator(&g_qtTranslator);
        g_qtTranslatorInstalled = true;
    }
    QCoreApplication::installTranslator(&g_dictionaryTranslator);
    g_translatorsInstalled = true;
}

UiLanguage UiLanguageSupport::current()
{
    return g_current;
}

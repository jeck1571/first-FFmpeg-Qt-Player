#include "StreamingPolicy.h"

#include <QUrl>

#include <algorithm>

namespace {

bool levelReady(const StreamBufferSnapshot& level, qint64 durationMs, qint64 bytes)
{
    return !level.present || level.durationMs >= durationMs || level.bytes >= bytes;
}

} // namespace

bool StreamingPolicy::isNetworkTarget(const QString& target)
{
    const QUrl url = QUrl::fromUserInput(target);
    if (!url.isValid() || url.scheme().isEmpty()) {
        return false;
    }
    const QString scheme = url.scheme().toLower();
    return scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https")
        || scheme == QStringLiteral("rtsp")
        || scheme == QStringLiteral("rtmp")
        || scheme == QStringLiteral("ftp")
        || scheme == QStringLiteral("udp")
        || scheme == QStringLiteral("tcp");
}

bool StreamingPolicy::isHttpTarget(const QString& target)
{
    const QUrl url = QUrl::fromUserInput(target);
    const QString scheme = url.scheme().toLower();
    return scheme == QStringLiteral("http") || scheme == QStringLiteral("https");
}

void StreamingPolicy::configureOpenOptions(
    AVDictionary** options, const QString& target)
{
    if (!options || !isNetworkTarget(target)) {
        return;
    }

    const QByteArray timeout = QByteArray::number(
        static_cast<qint64>(readTimeoutMs) * 1000);
    av_dict_set(options, "rw_timeout", timeout.constData(), 0);
    av_dict_set(options, "timeout", timeout.constData(), 0);

    if (isHttpTarget(target)) {
        av_dict_set(options, "reconnect", "1", 0);
        av_dict_set(options, "reconnect_streamed", "1", 0);
        av_dict_set(options, "reconnect_at_eof", "0", 0);
        av_dict_set(options, "reconnect_delay_max", "4", 0);
        av_dict_set(options, "multiple_requests", "1", 0);
        av_dict_set(options, "user_agent", "FFmpegQtPlayer/2.0", 0);
    }
}

int StreamingPolicy::reconnectDelayMs(int retryIndex)
{
    const int exponent = std::clamp(retryIndex, 0, 5);
    return std::min(4000, 250 * (1 << exponent));
}

bool StreamingPolicy::buffersReady(
    bool initial,
    const StreamBufferSnapshot& audio,
    const StreamBufferSnapshot& video)
{
    const qint64 audioDuration = initial ? 600 : 1000;
    const qint64 audioBytes = initial ? 128 * 1024 : 256 * 1024;
    const qint64 videoDuration = initial ? 350 : 600;
    const qint64 videoBytes = initial ? 1024 * 1024 : 2 * 1024 * 1024;
    return levelReady(audio, audioDuration, audioBytes)
        && levelReady(video, videoDuration, videoBytes);
}

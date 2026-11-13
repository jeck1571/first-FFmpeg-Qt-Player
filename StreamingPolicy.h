#pragma once

#include <QString>
#include <QtGlobal>

extern "C" {
#include <libavformat/avformat.h>
}

struct StreamBufferSnapshot
{
    bool present = false;
    qint64 bytes = 0;
    qint64 durationMs = 0;
};

class StreamingPolicy final
{
public:
    static bool isNetworkTarget(const QString& target);
    static bool isHttpTarget(const QString& target);
    static void configureOpenOptions(AVDictionary** options, const QString& target);
    static int reconnectDelayMs(int retryIndex);
    static bool buffersReady(
        bool initial,
        const StreamBufferSnapshot& audio,
        const StreamBufferSnapshot& video);

    static constexpr int maxReconnectAttempts = 6;
    static constexpr int readTimeoutMs = 5000;
    static constexpr int initialBufferTimeoutMs = 8000;
};

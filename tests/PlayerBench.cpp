#include "../FFmpegPlayer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace {

#ifdef Q_OS_WIN
double processCpuSeconds()
{
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(
            GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0.0;
    }
    auto toTicks = [](const FILETIME& value) {
        ULARGE_INTEGER ticks{};
        ticks.LowPart = value.dwLowDateTime;
        ticks.HighPart = value.dwHighDateTime;
        return static_cast<double>(ticks.QuadPart);
    };
    return (toTicks(kernel) + toTicks(user)) / 10000000.0;
}

double processWorkingSetMb()
{
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0.0;
    }
    return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
}
#endif

double percentileMs(QVector<qint64> values, double percentile)
{
    if (values.isEmpty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const int index = std::clamp(
        static_cast<int>(std::ceil(percentile * values.size())) - 1,
        0,
        static_cast<int>(values.size()) - 1);
    return static_cast<double>(values.at(index));
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() < 2) {
        std::fprintf(stderr, "Usage: PlayerBench <media-file> [--seeks N]\n");
        return 64;
    }

    int seekCount = 8;
    const int seekArgument = application.arguments().indexOf(QStringLiteral("--seeks"));
    if (seekArgument >= 0 && seekArgument + 1 < application.arguments().size()) {
        seekCount = std::clamp(
            application.arguments().at(seekArgument + 1).toInt(),
            1,
            100);
    }

    FFmpegPlayer player;
    QElapsedTimer startupClock;
    QElapsedTimer seekClock;
    QElapsedTimer totalClock;
    QVector<qint64> seekLatencies;
    QVector<qint64> seekTargets;
    int seekIndex = 0;
    qint64 seekFrameBaseline = 0;
    double firstFrameMs = 0.0;
    qint64 durationMs = 0;
    bool finished = false;

    const double cpuStartSeconds =
#ifdef Q_OS_WIN
        processCpuSeconds();
#else
        0.0;
#endif

    auto finish = [&](int exitCode) {
        if (finished) {
            return;
        }
        finished = true;
        QObject::disconnect(&player, nullptr, &application, nullptr);
        const qint64 wallMs = qMax<qint64>(1, totalClock.elapsed());
        const double cpuSeconds =
#ifdef Q_OS_WIN
            qMax(0.0, processCpuSeconds() - cpuStartSeconds);
#else
            0.0;
#endif
        const double processorCount =
#ifdef Q_OS_WIN
            qMax(1u, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
#else
            1.0;
#endif
        const double averageCpuPercent =
            100.0 * cpuSeconds / (static_cast<double>(wallMs) / 1000.0)
            / processorCount;
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        player.stop();
        std::printf(
            "{\"firstFrameMs\":%.1f,\"seekP50Ms\":%.1f,\"seekP95Ms\":%.1f,"
            "\"seekSamples\":%lld,\"avgCpuPercent\":%.1f,\"peakWorkingSetMb\":%.1f,"
            "\"clock\":\"%s\",\"hardware\":\"%s\",\"audioFrames\":%lld,"
            "\"videoFrames\":%lld,\"droppedFrames\":%lld,"
            "\"packetsDropped\":%lld,\"avDriftMs\":%lld,"
            "\"cacheBytes\":%lld,\"cacheDurationMs\":%lld,\"rebuffers\":%lld,"
            "\"reconnects\":%lld,\"framePoolHits\":%lld,"
            "\"framePoolMisses\":%lld,\"mediaOpenSerial\":%lld}\n",
            firstFrameMs,
            percentileMs(seekLatencies, 0.50),
            percentileMs(seekLatencies, 0.95),
            static_cast<long long>(seekLatencies.size()),
            averageCpuPercent,
#ifdef Q_OS_WIN
            processWorkingSetMb(),
#else
            0.0,
#endif
            stats.clockSource.toUtf8().constData(),
            stats.hardwareDecode.toUtf8().constData(),
            static_cast<long long>(stats.audioFramesWritten),
            static_cast<long long>(stats.videoFramesPresented),
            static_cast<long long>(stats.videoFramesDropped),
            static_cast<long long>(stats.videoPacketsDropped),
            static_cast<long long>(stats.lastAvDriftMs),
            static_cast<long long>(stats.bufferedBytes),
            static_cast<long long>(stats.bufferedDurationMs),
            static_cast<long long>(stats.rebufferCount),
            static_cast<long long>(stats.reconnectCount),
            static_cast<long long>(stats.framePoolHits),
            static_cast<long long>(stats.framePoolMisses),
            static_cast<long long>(stats.mediaOpenSerial));
        application.exit(exitCode);
    };

    QTimer firstFramePoll;
    firstFramePoll.setInterval(5);
    QTimer seekPoll;
    seekPoll.setInterval(2);
    QObject::connect(&firstFramePoll, &QTimer::timeout, &application, [&] {
        if (player.playbackStats().videoFramesPresented <= 0) {
            return;
        }
        firstFramePoll.stop();
        firstFrameMs = static_cast<double>(startupClock.elapsed());
        seekTargets.reserve(seekCount);
        for (int index = 0; index < seekCount; ++index) {
            const qint64 ratioPercent = 5 + (index * 13) % 61;
            seekTargets.append(qBound<qint64>(
                0, durationMs * ratioPercent / 100, qMax<qint64>(0, durationMs - 100)));
        }
        seekIndex = 0;
        seekClock.start();
        seekFrameBaseline = player.playbackStats().videoFramesPresented;
        player.setPosition(seekTargets.at(0));
        seekPoll.start();
    });

    QObject::connect(&seekPoll, &QTimer::timeout, &application, [&] {
        if (player.playbackStats().videoFramesPresented <= seekFrameBaseline) {
            return;
        }
        seekLatencies.append(seekClock.elapsed());
        ++seekIndex;
        if (seekIndex >= seekTargets.size()) {
            seekPoll.stop();
            // Sample A/V drift after the seek clock and output pipeline have
            // settled instead of reporting the transient first-frame value.
            QTimer::singleShot(250, &application, [&] {
                finish(0);
            });
            return;
        }
        seekFrameBaseline = player.playbackStats().videoFramesPresented;
        seekClock.restart();
        player.setPosition(seekTargets.at(seekIndex));
    });

    QObject::connect(
        &player,
        &FFmpegPlayer::mediaOpened,
        &application,
        [&](qint64 mediaDuration, bool hasAudio, bool hasVideo) {
            if (!hasAudio || !hasVideo || mediaDuration < 1000) {
                std::fprintf(stderr, "Benchmark media must contain A/V and be at least 1 second.\n");
                finish(1);
                return;
            }
            durationMs = mediaDuration;
            startupClock.start();
            firstFramePoll.start();
        });
    QObject::connect(
        &player,
        &FFmpegPlayer::errorOccurred,
        &application,
        [&](const QString& message) {
            std::fprintf(stderr, "Player error: %s\n", message.toUtf8().constData());
            finish(1);
        });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application, [&] {
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        const qint64 target = seekIndex >= 0 && seekIndex < seekTargets.size()
            ? seekTargets.at(seekIndex)
            : -1;
        std::fprintf(
            stderr,
            "Benchmark timed out: seek=%d target=%lld state=%d "
            "presented=%lld baseline=%lld position=%lld\n",
            seekIndex,
            static_cast<long long>(target),
            static_cast<int>(player.state()),
            static_cast<long long>(stats.videoFramesPresented),
            static_cast<long long>(seekFrameBaseline),
            static_cast<long long>(player.position()));
        finish(2);
    });
    timeout.start(60000);
    totalClock.start();
    player.open(application.arguments().at(1));
    return application.exec();
}

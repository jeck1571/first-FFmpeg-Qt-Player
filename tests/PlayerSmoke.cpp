#include "../FFmpegPlayer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <cstdio>

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() < 2) {
        std::fprintf(stderr, "Usage: PlayerSmoke <media-file>\n");
        return 64;
    }

    FFmpegPlayer player;
    player.setHardwareDecodeEnabled(false);
    bool mediaOpened = false;
    qint64 stressAudioBaseline = 0;
    qint64 stressFrameBaseline = 0;
    int rapidSeekIndex = 0;
    QElapsedTimer rapidSettleClock;
    const QVector<qint64> rapidSeekTargets = {
        600, 1200, 2200, 800, 3000, 1600, 3400, 2000
    };
    QTimer rapidSeekTimer;
    rapidSeekTimer.setInterval(20);
    QTimer healthPoll;
    healthPoll.setInterval(50);
    bool finished = false;

    auto finish = [&](int exitCode) {
        if (finished) {
            return;
        }
        finished = true;
        rapidSeekTimer.stop();
        healthPoll.stop();
        QObject::disconnect(&player, nullptr, &application, nullptr);
        if (exitCode != 0) {
            std::fprintf(
                stderr,
                "%s\n",
                player.diagnosticsReport().toUtf8().constData());
        }
        player.stop();
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        std::printf(
            "clock=%s hardware=%s audioFrames=%lld videoFrames=%lld "
            "dropped=%lld packetsDropped=%lld driftMs=%lld\n",
            stats.clockSource.toUtf8().constData(),
            stats.hardwareDecode.toUtf8().constData(),
            static_cast<long long>(stats.audioFramesWritten),
            static_cast<long long>(stats.videoFramesPresented),
            static_cast<long long>(stats.videoFramesDropped),
            static_cast<long long>(stats.videoPacketsDropped),
            static_cast<long long>(stats.lastAvDriftMs));
        application.exit(exitCode);
    };

    QObject::connect(
        &player,
        &FFmpegPlayer::errorOccurred,
        &application,
        [&](const QString& message) {
            std::fprintf(stderr, "Player error: %s\n", message.toUtf8().constData());
            finish(1);
        });
    QObject::connect(
        &player,
        &FFmpegPlayer::mediaOpened,
        &application,
        [&](qint64, bool hasAudio, bool hasVideo) {
            mediaOpened = true;
            if (!hasAudio || !hasVideo) {
                std::fprintf(stderr, "Smoke media must contain audio and video.\n");
                finish(1);
                return;
            }

            QTimer::singleShot(250, &player, [&] {
                player.setPlaybackRate(2.0);
            });
            QTimer::singleShot(700, &player, [&] {
                player.setPosition(2000);
            });
            QTimer::singleShot(1000, &player, [&] {
                stressFrameBaseline = player.playbackStats().videoFramesPresented;
                stressAudioBaseline = player.playbackStats().audioFramesWritten;
                rapidSeekIndex = 0;
                rapidSeekTimer.start();
            });
        });

    QObject::connect(&rapidSeekTimer, &QTimer::timeout, &application, [&] {
        if (rapidSeekIndex >= rapidSeekTargets.size()) {
            rapidSeekTimer.stop();
            rapidSettleClock.start();
            healthPoll.start();
            return;
        }
        player.setPosition(rapidSeekTargets.at(rapidSeekIndex++));
    });
    QObject::connect(&healthPoll, &QTimer::timeout, &application, [&] {
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        const bool healthy = stats.clockSource == QStringLiteral("audio")
            && stats.audioFramesWritten > stressAudioBaseline
            && stats.videoFramesPresented > stressFrameBaseline
            && player.position() > rapidSeekTargets.last()
            && stats.videoPacketsDropped == 0
            && !stats.hardwareDecode.isEmpty()
            && player.state() == FFmpegPlayer::State::Playing;
        if (healthy) {
            QTemporaryDir diagnosticsDir;
            const QString diagnosticsPath = diagnosticsDir.filePath(
                QStringLiteral("diagnostics.txt"));
            QString diagnosticsError;
            if (!diagnosticsDir.isValid()
                || !player.exportDiagnostics(diagnosticsPath, &diagnosticsError)) {
                std::fprintf(
                    stderr,
                    "Diagnostics export failed: %s\n",
                    diagnosticsError.toUtf8().constData());
                finish(1);
                return;
            }
            QFile diagnosticsFile(diagnosticsPath);
            if (!diagnosticsFile.open(QIODevice::ReadOnly)) {
                std::fprintf(stderr, "Cannot read exported diagnostics.\n");
                finish(1);
                return;
            }
            const QByteArray diagnostics = diagnosticsFile.readAll();
            if (!diagnostics.contains("FFmpegQtPlayer diagnostics")
                || !diagnostics.contains("\"framePoolHits\"")
                || !diagnostics.contains("\"reconnects\"")) {
                std::fprintf(stderr, "Exported diagnostics is incomplete.\n");
                finish(1);
                return;
            }
            std::printf(
                "rapidSettleMs=%lld\n",
                static_cast<long long>(rapidSettleClock.elapsed()));
            finish(0);
            return;
        }
        if (rapidSettleClock.elapsed() >= 10000
            || player.state() == FFmpegPlayer::State::Ended
            || player.state() == FFmpegPlayer::State::Error) {
            std::fprintf(
                stderr,
                "Smoke check failed: state=%d position=%lld duration=%lld "
                "audioBaseline=%lld audioFrames=%lld baseline=%lld presented=%lld "
                "rapidSeeks=%d/%d packetDrops=%lld settleMs=%lld "
                "seeksHandled=%lld pending=%d phase=%d queues=%d/%d "
                "decodedPackets=%lld/%lld decodedFrames=%lld/%lld\n",
                static_cast<int>(player.state()),
                static_cast<long long>(player.position()),
                static_cast<long long>(player.duration()),
                static_cast<long long>(stressAudioBaseline),
                static_cast<long long>(stats.audioFramesWritten),
                static_cast<long long>(stressFrameBaseline),
                static_cast<long long>(stats.videoFramesPresented),
                rapidSeekIndex,
                static_cast<int>(rapidSeekTargets.size()),
                static_cast<long long>(stats.videoPacketsDropped),
                static_cast<long long>(rapidSettleClock.elapsed()),
                static_cast<long long>(stats.seekCommandsHandled),
                stats.seekPending ? 1 : 0,
                stats.controlPhase,
                stats.audioQueueDepth,
                stats.videoQueueDepth,
                static_cast<long long>(stats.audioPacketsDecoded),
                static_cast<long long>(stats.videoPacketsDecoded),
                static_cast<long long>(stats.audioFramesDecoded),
                static_cast<long long>(stats.videoFramesDecoded));
            std::fprintf(
                stderr,
                "%s\n",
                player.diagnosticsReport().toUtf8().constData());
            if (qEnvironmentVariableIsSet("FFMPEG_PLAYER_HOLD_ON_FAILURE")) {
                std::fprintf(stderr, "Holding failed smoke process for inspection.\n");
                QThread::msleep(30000);
            }
            finish(1);
        }
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application, [&] {
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        std::fprintf(
            stderr,
            "Timed out waiting for playback: state=%d position=%lld "
            "audioFrames=%lld videoFrames=%lld seeksHandled=%lld pending=%d "
            "phase=%d queues=%d/%d decodedPackets=%lld/%lld "
            "decodedFrames=%lld/%lld\n",
            static_cast<int>(player.state()),
            static_cast<long long>(player.position()),
            static_cast<long long>(stats.audioFramesWritten),
            static_cast<long long>(stats.videoFramesPresented),
            static_cast<long long>(stats.seekCommandsHandled),
            stats.seekPending ? 1 : 0,
            stats.controlPhase,
            stats.audioQueueDepth,
            stats.videoQueueDepth,
            static_cast<long long>(stats.audioPacketsDecoded),
            static_cast<long long>(stats.videoPacketsDecoded),
            static_cast<long long>(stats.audioFramesDecoded),
            static_cast<long long>(stats.videoFramesDecoded));
        std::fprintf(
            stderr,
            "%s\n",
            player.diagnosticsReport().toUtf8().constData());
        finish(2);
    });
    timeout.start(15000);

    player.open(application.arguments().at(1));
    const int result = application.exec();
    if (!mediaOpened) {
        std::fprintf(stderr, "Media never opened.\n");
        return 3;
    }
    return result;
}

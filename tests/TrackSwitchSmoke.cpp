#include "../FFmpegPlayer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <cstdio>

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() < 2) {
        std::fprintf(stderr, "Usage: TrackSwitchSmoke <multi-track-media>\n");
        return 64;
    }

    FFmpegPlayer player;
    player.setHardwareDecodeEnabled(false);
    bool finished = false;
    bool opened = false;
    qint64 openSerial = 0;
    int targetAudioStream = -1;
    int targetSubtitleStream = -1;
    qint64 audioFramesBeforeSwitch = 0;
    qint64 videoFramesBeforeSwitch = 0;
    QElapsedTimer settleClock;

    auto finish = [&](int exitCode) {
        if (finished) {
            return;
        }
        finished = true;
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        std::printf(
            "audio=%d subtitle=%d openSerial=%lld frames=%lld state=%d\n",
            player.activeAudioStream(),
            player.activeSubtitleStream(),
            static_cast<long long>(stats.mediaOpenSerial),
            static_cast<long long>(stats.videoFramesPresented),
            static_cast<int>(player.state()));
        if (exitCode != 0) {
            std::fprintf(stderr, "%s\n", player.diagnosticsReport().toUtf8().constData());
        }
        player.stop();
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
            opened = true;
            if (!hasAudio || !hasVideo) {
                std::fprintf(stderr, "Fixture needs audio and video streams.\n");
                finish(1);
                return;
            }

            const QVector<FFmpegPlayer::TrackInfo> audioTracks = player.audioTracks();
            const QVector<FFmpegPlayer::TrackInfo> subtitleTracks =
                player.subtitleTracks();
            if (audioTracks.size() < 2 || subtitleTracks.size() < 2) {
                std::fprintf(
                    stderr,
                    "Fixture needs two audio and two subtitle tracks: %lld/%lld\n",
                    static_cast<long long>(audioTracks.size()),
                    static_cast<long long>(subtitleTracks.size()));
                finish(1);
                return;
            }

            targetAudioStream = audioTracks.at(1).index;
            targetSubtitleStream = subtitleTracks.at(1).index;
            openSerial = player.playbackStats().mediaOpenSerial;
            QTimer::singleShot(500, &player, [&] {
                const FFmpegPlayer::PlaybackStats before = player.playbackStats();
                audioFramesBeforeSwitch = before.audioFramesWritten;
                videoFramesBeforeSwitch = before.videoFramesPresented;
                player.selectAudioStream(targetAudioStream);
                player.selectSubtitleStream(targetSubtitleStream);
                settleClock.start();
            });
        });

    QTimer poll;
    poll.setInterval(40);
    QObject::connect(&poll, &QTimer::timeout, &application, [&] {
        if (!opened || !settleClock.isValid()) {
            return;
        }
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        const bool switched = player.activeAudioStream() == targetAudioStream
            && player.activeSubtitleStream() == targetSubtitleStream;
        const bool continuing = stats.audioFramesWritten > audioFramesBeforeSwitch
            && stats.videoFramesPresented > videoFramesBeforeSwitch
            && stats.mediaOpenSerial == openSerial
            && player.position() > 700;
        if (switched && continuing
            && player.state() == FFmpegPlayer::State::Playing) {
            finish(0);
            return;
        }
        if (settleClock.elapsed() > 8000) {
            std::fprintf(
                stderr,
                "Track switch did not settle: active=%d/%d target=%d/%d "
                "position=%lld serial=%lld/%lld frames=%lld/%lld state=%d\n",
                player.activeAudioStream(),
                player.activeSubtitleStream(),
                targetAudioStream,
                targetSubtitleStream,
                static_cast<long long>(player.position()),
                static_cast<long long>(stats.mediaOpenSerial),
                static_cast<long long>(openSerial),
                static_cast<long long>(stats.audioFramesWritten),
                static_cast<long long>(stats.videoFramesPresented),
                static_cast<int>(player.state()));
            finish(2);
        }
    });
    poll.start();

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application, [&] {
        finish(3);
    });
    timeout.start(12000);

    player.open(application.arguments().at(1));
    return application.exec();
}

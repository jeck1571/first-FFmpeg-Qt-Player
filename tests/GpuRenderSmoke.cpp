#include "../FFmpegPlayer.h"
#include "../VideoCanvas.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>

#include <algorithm>
#include <cstdio>

namespace {

bool hasVisibleContent(const QImage& image)
{
    if (image.isNull() || image.width() < 2 || image.height() < 2) {
        return false;
    }

    int minimum = 255;
    int maximum = 0;
    const int stepX = std::max(1, image.width() / 64);
    const int stepY = std::max(1, image.height() / 36);
    for (int y = 0; y < image.height(); y += stepY) {
        for (int x = 0; x < image.width(); x += stepX) {
            const QRgb pixel = image.pixel(x, y);
            const int luma = (qRed(pixel) * 21 + qGreen(pixel) * 72
                              + qBlue(pixel) * 7) / 100;
            minimum = std::min(minimum, luma);
            maximum = std::max(maximum, luma);
        }
    }
    return maximum > 16 && maximum - minimum > 12;
}

} // namespace

int main(int argc, char* argv[])
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    QApplication application(argc, argv);
    if (application.arguments().size() < 2) {
        std::fprintf(stderr, "Usage: GpuRenderSmoke <media-file>\n");
        return 64;
    }

    VideoCanvas canvas;
    canvas.resize(960, 540);
    canvas.show();
    canvas.prepareGpuRenderer();

    FFmpegPlayer player;
    player.setVideoCanvas(&canvas);
    player.setHardwareDecodeEnabled(true);

    bool opened = false;
    bool finished = false;
    QElapsedTimer settleClock;

    auto finish = [&](int exitCode) {
        if (finished) {
            return;
        }
        finished = true;
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        std::printf(
            "hardware=%s render=%s submitted=%lld rendered=%lld readbacks=%lld "
            "state=%d\n",
            stats.hardwareDecode.toUtf8().constData(),
            stats.renderBackend.toUtf8().constData(),
            static_cast<long long>(stats.gpuFramesPresented),
            static_cast<long long>(stats.gpuFramesRendered),
            static_cast<long long>(stats.hardwareReadbacks),
            static_cast<int>(player.state()));
        std::fflush(stdout);
        if (exitCode != 0) {
            std::fprintf(stderr, "%s\n", player.diagnosticsReport().toUtf8().constData());
        }
        player.setVideoCanvas(nullptr);
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
        [&](qint64, bool, bool hasVideo) {
            opened = hasVideo;
            if (!opened) {
                std::fprintf(stderr, "GPU smoke media must contain video.\n");
                finish(2);
                return;
            }
            settleClock.start();
        });

    QTimer poll;
    poll.setInterval(40);
    QObject::connect(&poll, &QTimer::timeout, &application, [&] {
        if (!opened) {
            return;
        }
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        const bool healthy =
            stats.hardwareDecode == QStringLiteral("d3d11va-zero-copy")
            && stats.renderBackend == QStringLiteral("d3d11-rhi-zero-copy")
            && stats.gpuFramesPresented >= 5
            && stats.gpuFramesRendered >= 5
            && stats.hardwareReadbacks == 0
            && canvas.gpuRenderReady()
            && player.state() == FFmpegPlayer::State::Playing;
        if (healthy) {
            const QImage image = canvas.snapshot(false);
            const bool visible = hasVisibleContent(image);
            if (!visible) {
                std::fprintf(
                    stderr,
                    "GPU frame counters advanced but the rendered image is blank.\n");
                finish(5);
                return;
            }
            finish(0);
            return;
        }
        if (settleClock.elapsed() > 15000
            || player.state() == FFmpegPlayer::State::Error
            || player.state() == FFmpegPlayer::State::Ended) {
            std::fprintf(
                stderr,
                "GPU path did not settle: gpuReady=%d submitted=%lld rendered=%lld "
                "readbacks=%lld state=%d render=%s\n",
                canvas.gpuRenderReady() ? 1 : 0,
                static_cast<long long>(stats.gpuFramesPresented),
                static_cast<long long>(stats.gpuFramesRendered),
                static_cast<long long>(stats.hardwareReadbacks),
                static_cast<int>(player.state()),
                canvas.renderBackend().toUtf8().constData());
            finish(3);
        }
    });
    poll.start();

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application, [&] {
        finish(4);
    });
    timeout.start(20000);

    QTimer::singleShot(600, &application, [&] {
        player.open(application.arguments().at(1));
    });
    return application.exec();
}

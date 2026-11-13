#include "../AiRecognitionService.h"

#include <QCoreApplication>
#include <QImage>
#include <QTimer>

#include <cmath>
#include <cstdio>

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() < 2) {
        std::fprintf(stderr,
                     "Usage: AiRecognitionServiceSmoke <base-url> [--expect-auth-stop]\n");
        return 64;
    }
    const bool expectAuthenticationStop =
        application.arguments().contains(QStringLiteral("--expect-auth-stop"));

    AiRecognitionService service;
    AiRecognitionService::Config config;
    config.baseUrl = application.arguments().at(1);
    config.apiKey = QStringLiteral("test-key");
    config.audioChunkSeconds = 5;
    config.audioEnabled = true;
    config.visionEnabled = true;
    service.setConfig(config);

    bool transcriptReceived = false;
    bool visionReceived = false;
    bool summaryReceived = false;
    bool errorReceived = false;

    auto finishIfReady = [&] {
        if (transcriptReceived && visionReceived && summaryReceived) {
            std::printf("transcript=ok vision=ok summary=ok\n");
            QTimer::singleShot(50, &application, [&application] {
                application.exit(0);
            });
        }
    };

    QObject::connect(&service, &AiRecognitionService::transcriptSegmentReady, &application,
                     [&](const QString& text, qint64, qint64) {
        transcriptReceived = !text.isEmpty();
        finishIfReady();
    });
    QObject::connect(&service, &AiRecognitionService::visionNoteReady, &application,
                     [&](const QString& text, qint64) {
        visionReceived = !text.isEmpty();
        finishIfReady();
    });
    QObject::connect(&service, &AiRecognitionService::coreContentUpdated, &application,
                     [&](const QString& text) {
        summaryReceived = !text.isEmpty();
        finishIfReady();
    });
    QObject::connect(&service, &AiRecognitionService::errorOccurred, &application,
                     [&](const QString& message) {
        if (expectAuthenticationStop) {
            errorReceived = true;
            QTimer::singleShot(0, &application, [&] {
                if (!service.isRunning()) {
                    std::printf("auth-stop=ok\n");
                    application.exit(0);
                }
            });
            return;
        }
        std::fprintf(stderr, "AI service error: %s\n", message.toUtf8().constData());
        application.exit(1);
    });

    QObject::connect(&service, &AiRecognitionService::runningChanged, &application,
                     [&](bool running) {
        if (!running) {
            if (expectAuthenticationStop && errorReceived) {
                std::printf("auth-stop=ok\n");
                QTimer::singleShot(50, &application, [&application] {
                    application.exit(0);
                });
            }
            return;
        }
        constexpr int sampleRate = 16000;
        constexpr int channels = 2;
        QByteArray pcm(static_cast<qsizetype>(sampleRate) * channels * 2 * 5,
                       Qt::Uninitialized);
        auto* samples = reinterpret_cast<qint16*>(pcm.data());
        const qsizetype count = pcm.size() / static_cast<qsizetype>(sizeof(qint16));
        for (qsizetype index = 0; index < count; ++index) {
            samples[index] = static_cast<qint16>(
                std::lround(std::sin(index * 0.07) * 1200.0));
        }
        service.pushAudioSamples(pcm, sampleRate, channels, 0);

        QImage frame(320, 180, QImage::Format_RGB32);
        frame.fill(QColor(32, 52, 72));
        const bool frameQueued = service.pushVideoFrame(frame, 1000);
        if (!frameQueued) {
            std::fprintf(stderr, "Failed to queue the test video frame.\n");
            application.exit(1);
        }
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application, [&application] {
        std::fprintf(stderr, "Timed out waiting for AI service responses.\n");
        application.exit(2);
    });
    timeout.start(15000);
    service.start();
    return application.exec();
}

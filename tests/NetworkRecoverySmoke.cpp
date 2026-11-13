#include "../FFmpegPlayer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <atomic>
#include <cstdio>

namespace {

QByteArray httpResponse(
    const QByteArray& media,
    qint64 start,
    qint64 end,
    bool headOnly,
    qint64 sendLimit = -1)
{
    start = qBound<qint64>(0, start, media.size());
    end = qBound<qint64>(start, end, media.size() - 1);
    const qint64 length = end >= start ? end - start + 1 : 0;
    QByteArray header;
    header += "HTTP/1.1 206 Partial Content\r\n";
    header += "Content-Type: video/mp4\r\n";
    header += "Accept-Ranges: bytes\r\n";
    header += "Connection: close\r\n";
    header += "Content-Range: bytes "
        + QByteArray::number(start) + "-" + QByteArray::number(end)
        + "/" + QByteArray::number(media.size()) + "\r\n";
    header += "Content-Length: " + QByteArray::number(length) + "\r\n\r\n";
    if (headOnly || length <= 0) {
        return header;
    }
    const qint64 bodyLength = sendLimit < 0 ? length : qMin(length, sendLimit);
    return header + media.mid(static_cast<int>(start), static_cast<int>(bodyLength));
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() < 2) {
        std::fprintf(stderr, "Usage: NetworkRecoverySmoke <media-file>\n");
        return 64;
    }

    QFile mediaFile(application.arguments().at(1));
    if (!mediaFile.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "Cannot open test media.\n");
        return 65;
    }
    const QByteArray media = mediaFile.readAll();
    if (media.size() < 512 * 1024) {
        std::fprintf(stderr, "Test media is too small for an interrupted response.\n");
        return 65;
    }

    QTcpServer server;
    int requestCount = 0;
    bool playbackOpened = false;
    bool interruptedOnce = false;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "Cannot listen on localhost.\n");
        return 66;
    }

    QObject::connect(&server, &QTcpServer::newConnection, &application, [&] {
        while (QTcpSocket* socket = server.nextPendingConnection()) {
            socket->setParent(&server);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                QByteArray request = socket->property("request").toByteArray();
                request += socket->readAll();
                socket->setProperty("request", request);
                const int headerEnd = request.indexOf("\r\n\r\n");
                if (headerEnd < 0 || socket->property("responded").toBool()) {
                    return;
                }
                socket->setProperty("responded", true);
                const int requestIndex = ++requestCount;

                qint64 start = 0;
                const QRegularExpression rangeExpression(
                    QStringLiteral("Range:\\s*bytes=(\\d+)-(\\d*)"),
                    QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch match =
                    rangeExpression.match(QString::fromLatin1(request));
                if (match.hasMatch()) {
                    start = match.captured(1).toLongLong();
                }
                const bool headOnly = request.startsWith("HEAD ");
                const qint64 chunkEnd = qMin<qint64>(
                    media.size() - 1, start + 128 * 1024 - 1);
                const bool interrupted = playbackOpened
                    && !interruptedOnce
                    && chunkEnd + 1024 < media.size() - 1;
                if (interrupted) {
                    interruptedOnce = true;
                }
                std::printf(
                    "request=%d range=%lld head=%d interrupted=%d\n",
                    requestIndex,
                    static_cast<long long>(start),
                    headOnly ? 1 : 0,
                    interrupted ? 1 : 0);
                QByteArray response = httpResponse(
                    media,
                    start,
                    chunkEnd,
                    headOnly,
                    interrupted ? 24 * 1024 : -1);
                socket->write(response);
                if (interrupted && !headOnly) {
                    // Advertise the normal range length, then close before the
                    // advertised Content-Length has been delivered.
                    QTimer::singleShot(120, socket, [socket] {
                        socket->abort();
                    });
                } else {
                    socket->disconnectFromHost();
                }
            });
        }
    });

    FFmpegPlayer player;
    player.setHardwareDecodeEnabled(false);
    bool opened = false;
    bool finished = false;
    int recoveryStateCount = 0;
    QElapsedTimer settleClock;

    auto finish = [&](int exitCode) {
        if (finished) {
            return;
        }
        finished = true;
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        std::printf(
            "requests=%d reconnects=%lld frames=%lld state=%d\n",
            requestCount,
            static_cast<long long>(stats.reconnectCount),
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
        &FFmpegPlayer::stateChanged,
        &application,
        [&](FFmpegPlayer::State state) {
            if (state == FFmpegPlayer::State::Stalled
                || state == FFmpegPlayer::State::Reconnecting) {
                ++recoveryStateCount;
            }
        });
    QObject::connect(
        &player,
        &FFmpegPlayer::mediaOpened,
        &application,
        [&](qint64, bool hasAudio, bool hasVideo) {
            opened = true;
            playbackOpened = true;
            if (!hasVideo) {
                finish(1);
                return;
            }
            player.setPlaybackRate(2.0);
            settleClock.start();
        });

    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &application, [&] {
        const FFmpegPlayer::PlaybackStats stats = player.playbackStats();
        if (opened
            && requestCount >= 2
            && stats.reconnectCount >= 1
            && recoveryStateCount >= 1
            && stats.videoFramesPresented >= 6
            && player.state() == FFmpegPlayer::State::Playing) {
            finish(0);
            return;
        }
        if (settleClock.isValid() && settleClock.elapsed() > 18000) {
            std::fprintf(
                stderr,
                "Recovery did not settle: opened=%d requests=%d reconnects=%lld "
                "recoveryStates=%d frames=%lld state=%d\n",
                opened ? 1 : 0,
                requestCount,
                static_cast<long long>(stats.reconnectCount),
                recoveryStateCount,
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
    timeout.start(22000);

    const QString target = QStringLiteral("http://127.0.0.1:%1/sample.mp4")
                               .arg(server.serverPort());
    player.open(target);
    return application.exec();
}

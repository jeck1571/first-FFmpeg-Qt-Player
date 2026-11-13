#include "../PacketQueue.h"
#include "../PlaybackClock.h"
#include "../StreamingPolicy.h"
#include "../VideoFramePool.h"
#include "../HlsPlaylist.h"

#include <QFile>
#include <QTest>

#include <atomic>
#include <deque>
#include <thread>

class PlaybackCoreTests final : public QObject
{
    Q_OBJECT

private slots:
    void wallClockHonorsPauseAndRate();
    void audioClockUsesProcessedAudioTime();
    void videoDropThresholdAdaptsToFrameRate();
    void packetQueueRejectsWhenFull();
    void packetQueueWakesBlockedProducerOnAbort();
    void packetQueueTracksBytesAndDuration();
    void packetQueuePauseAndResume();
    void streamingPolicyReconnectAndBufferWatermarks();
    void hlsMasterPlaylistParsesVariants();
    void hlsAbrUsesBufferAwareSelection();
    void videoFramePoolKeepsLiveFramesStable();
    void videoFramePoolRebuildsForNewGeometry();
};

void PlaybackCoreTests::wallClockHonorsPauseAndRate()
{
    WallClock clock(1000);
    clock.setRate(2.0);
    QTest::qWait(30);
    QVERIFY(clock.mediaTimeMs() >= 1040);

    clock.setPaused(true);
    const qint64 pausedAt = clock.mediaTimeMs();
    QTest::qWait(25);
    QCOMPARE(clock.mediaTimeMs(), pausedAt);

    clock.setPaused(false);
    clock.setRate(0.5);
    QTest::qWait(30);
    QVERIFY(clock.mediaTimeMs() >= pausedAt + 5);
}

void PlaybackCoreTests::audioClockUsesProcessedAudioTime()
{
    AudioClock clock(2000);
    clock.setRate(1.5);
    std::atomic<qint64> processedUs{100000};
    clock.setProcessedUsProvider([&processedUs] {
        return processedUs.load();
    });
    clock.anchorAt(2000, processedUs.load());

    processedUs = 200000;
    QCOMPARE(clock.mediaTimeMs(), 2150);

    clock.setPaused(true);
    processedUs = 900000;
    QCOMPARE(clock.mediaTimeMs(), 2150);

    clock.setPaused(false);
    processedUs = 1000000;
    QCOMPARE(clock.mediaTimeMs(), 2300);
    clock.clearProcessedUsProvider();
    QCOMPARE(clock.mediaTimeMs(), 2300);
}

void PlaybackCoreTests::videoDropThresholdAdaptsToFrameRate()
{
    QCOMPARE(videoDropThresholdMs(30.0, 0.0), 50.0);
    QVERIFY(videoDropThresholdMs(24.0, 0.0) > videoDropThresholdMs(60.0, 0.0));
    QVERIFY(videoDropThresholdMs(30.0, 40.0) > videoDropThresholdMs(30.0, 0.0));
    QCOMPARE(videoDropThresholdMs(0.0, 1000.0), 180.0);
}

void PlaybackCoreTests::packetQueueRejectsWhenFull()
{
    PacketQueue queue(1);
    QVERIFY(queue.tryPush(av_packet_alloc()));
    QCOMPARE(queue.size(), std::size_t(1));
    QVERIFY(!queue.tryPush(av_packet_alloc()));

    AVPacket* packet = queue.pop(1);
    QVERIFY(packet);
    av_packet_free(&packet);
    QCOMPARE(queue.size(), std::size_t(0));
}

void PlaybackCoreTests::packetQueueWakesBlockedProducerOnAbort()
{
    PacketQueue queue(1);
    QVERIFY(queue.tryPush(av_packet_alloc()));

    std::atomic<bool> returned{false};
    std::atomic<bool> pushResult{true};
    std::thread producer([&] {
        AVPacket* packet = av_packet_alloc();
        pushResult = queue.push(packet);
        returned = true;
    });

    QTest::qWait(25);
    QVERIFY(!returned.load());
    queue.abort();
    producer.join();
    QVERIFY(returned.load());
    QVERIFY(!pushResult.load());
}

void PlaybackCoreTests::packetQueueTracksBytesAndDuration()
{
    PacketQueue queue(4, 64);
    AVPacket* first = av_packet_alloc();
    AVPacket* second = av_packet_alloc();
    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(av_new_packet(first, 48) >= 0);
    QVERIFY(av_new_packet(second, 32) >= 0);

    QVERIFY(queue.pushTimed(first, 120, 0));
    QCOMPARE(queue.bufferedBytes(), std::size_t(48));
    QCOMPARE(queue.bufferedDurationMs(), qint64(120));

    // A single packet larger than maxBytes is admitted only into an empty
    // queue. This keeps a large keyframe from deadlocking the producer.
    QVERIFY(!queue.pushTimed(second, 80, 0));
    av_packet_free(&second);

    AVPacket* popped = queue.pop(1);
    QVERIFY(popped);
    av_packet_free(&popped);
    QCOMPARE(queue.bufferedBytes(), std::size_t(0));
    QCOMPARE(queue.bufferedDurationMs(), qint64(0));
}

void PlaybackCoreTests::packetQueuePauseAndResume()
{
    PacketQueue queue(2);
    AVPacket* packet = av_packet_alloc();
    QVERIFY(packet);
    QVERIFY(queue.pushTimed(packet, 40, 0));

    queue.setPaused(true);
    QVERIFY(!queue.pop(25));

    queue.setPaused(false);
    AVPacket* resumed = queue.pop(100);
    QVERIFY(resumed);
    av_packet_free(&resumed);
}

void PlaybackCoreTests::streamingPolicyReconnectAndBufferWatermarks()
{
    QVERIFY(StreamingPolicy::isNetworkTarget(QStringLiteral("https://example.com/a.mp4")));
    QVERIFY(StreamingPolicy::isNetworkTarget(QStringLiteral("rtsp://example.com/live")));
    QVERIFY(!StreamingPolicy::isNetworkTarget(QStringLiteral("D:/video/sample.mp4")));

    QCOMPARE(StreamingPolicy::reconnectDelayMs(0), 250);
    QCOMPARE(StreamingPolicy::reconnectDelayMs(1), 500);
    QCOMPARE(StreamingPolicy::reconnectDelayMs(2), 1000);
    QCOMPARE(StreamingPolicy::reconnectDelayMs(4), 4000);
    QCOMPARE(StreamingPolicy::reconnectDelayMs(20), 4000);

    StreamBufferSnapshot audio;
    audio.present = true;
    audio.durationMs = 700;
    audio.bytes = 160 * 1024;
    StreamBufferSnapshot video;
    video.present = true;
    video.durationMs = 400;
    video.bytes = 1100 * 1024;
    QVERIFY(StreamingPolicy::buffersReady(true, audio, video));

    video.durationMs = 700;
    video.bytes = 700 * 1024;
    QVERIFY(!StreamingPolicy::buffersReady(false, audio, video));

    audio.durationMs = 1100;
    audio.bytes = 300 * 1024;
    video.durationMs = 700;
    video.bytes = 2100 * 1024;
    QVERIFY(StreamingPolicy::buffersReady(false, audio, video));
}

void PlaybackCoreTests::hlsMasterPlaylistParsesVariants()
{
    QFile fixture(QStringLiteral(FFMPEG_PLAYER_TEST_SOURCE_DIR
                                 "/tests/fixtures/hls-master.m3u8"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const QByteArray playlist = fixture.readAll();
    QVERIFY(HlsPlaylist::isMasterPlaylist(playlist));

    const QVector<HlsVariant> variants = HlsPlaylist::parseMaster(
        playlist, QUrl(QStringLiteral("https://media.example.com/master.m3u8")));
    QCOMPARE(variants.size(), 3);
    QCOMPARE(variants.at(0).width, 640);
    QCOMPARE(variants.at(0).height, 360);
    QCOMPARE(variants.at(0).selectionBandwidth(), qint64(700000));
    QCOMPARE(variants.at(0).url.toString(),
             QStringLiteral("https://media.example.com/360p/index.m3u8"));
    QCOMPARE(variants.at(1).codecs,
             QStringLiteral("avc1.4d401f,mp4a.40.2"));
    QCOMPARE(variants.at(2).url.toString(),
             QStringLiteral("https://cdn.example.com/hls/1080p/index.m3u8"));
}

void PlaybackCoreTests::hlsAbrUsesBufferAwareSelection()
{
    QVector<HlsVariant> variants;
    for (qint64 bandwidth : {800000, 2200000, 5000000}) {
        HlsVariant variant;
        variant.bandwidth = bandwidth;
        variant.url = QUrl(QStringLiteral("https://example.com/%1.m3u8").arg(bandwidth));
        variants.append(variant);
    }

    HlsAbrController abr;
    QCOMPARE(abr.chooseVariant(variants, 0, false), 0);

    abr.observeThroughput(1000 * 1000, 1000);
    QCOMPARE(abr.estimatedKbps(), qint64(8000));
    QCOMPARE(abr.chooseVariant(variants, 1000, false), 1);

    abr.reset();
    abr.observeThroughput(750 * 1000, 1000);
    QCOMPARE(abr.estimatedKbps(), qint64(6000));
    QCOMPARE(abr.chooseVariant(variants, 1000, false), 1);
    QCOMPARE(abr.chooseVariant(variants, 6000, false), 2);
    QCOMPARE(abr.chooseVariant(variants, 6000, true), 0);
}

void PlaybackCoreTests::videoFramePoolKeepsLiveFramesStable()
{
    VideoFramePool pool(2);
    QImage first = pool.acquire(32, 24, QImage::Format_ARGB32);
    QVERIFY(!first.isNull());
    QCOMPARE(first.width(), 32);
    QCOMPARE(first.height(), 24);
    QVERIFY(pool.misses() >= 1);

    first.fill(Qt::red);
    QImage second = pool.acquire(32, 24, QImage::Format_ARGB32);
    QVERIFY(!second.isNull());
    // The next request wraps onto a still-live slot, so it must allocate a
    // replacement rather than overwrite the frame held by the GUI.
    QImage third = pool.acquire(32, 24, QImage::Format_ARGB32);
    QVERIFY(!third.isNull());
    third.fill(Qt::blue);
    QCOMPARE(first.pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(third.pixelColor(0, 0), QColor(Qt::blue));

    first = QImage();
    second = QImage();
    third = QImage();
    QImage reused = pool.acquire(32, 24, QImage::Format_ARGB32);
    QVERIFY(!reused.isNull());
    QVERIFY(pool.hits() >= 1);
}

void PlaybackCoreTests::videoFramePoolRebuildsForNewGeometry()
{
    VideoFramePool pool(3);
    std::deque<QImage> live;
    for (int i = 0; i < 12; ++i) {
        QImage frame = pool.acquire(
            16 + i, 12 + i, QImage::Format_ARGB32);
        QVERIFY(!frame.isNull());
        frame.fill(QColor::fromRgb(i * 10, 20, 30));
        live.push_back(frame);
    }
    QCOMPARE(live.back().width(), 27);
    QCOMPARE(live.back().height(), 23);
    QCOMPARE(pool.misses(), qint64(12));
}

QTEST_MAIN(PlaybackCoreTests)
#include "PlaybackCoreTests.moc"

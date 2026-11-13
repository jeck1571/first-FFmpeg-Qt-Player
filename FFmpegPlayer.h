#pragma once

#include "HlsPlaylist.h"
#include "PacketQueue.h"
#include "StreamingPolicy.h"
#include "VideoFramePool.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class VideoCanvas;
class PlaybackClock;
class AudioClock;
class QNetworkAccessManager;
class QNetworkReply;

class FFmpegPlayer final : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Opening,
        Buffering,
        Playing,
        Paused,
        Stalled,
        Reconnecting,
        Stopped,
        Ended,
        Error
    };
    Q_ENUM(State)

    struct TrackInfo {
        int index = -1;
        QString label;
    };

    struct ChapterInfo {
        qint64 startMs = 0;
        qint64 endMs = 0;
        QString title;
    };

    struct PlaybackStats {
        qint64 audioFramesWritten = 0;
        qint64 videoFramesPresented = 0;
        qint64 videoFramesDropped = 0;
        qint64 videoPacketsDropped = 0;
        qint64 lastAvDriftMs = 0;
        qint64 audioPacketsDecoded = 0;
        qint64 videoPacketsDecoded = 0;
        qint64 audioFramesDecoded = 0;
        qint64 videoFramesDecoded = 0;
        qint64 seekCommandsHandled = 0;
        qint64 bufferedBytes = 0;
        qint64 bufferedDurationMs = 0;
        qint64 rebufferCount = 0;
        qint64 rebufferTotalMs = 0;
        qint64 lastRebufferMs = 0;
        qint64 reconnectCount = 0;
        qint64 framePoolHits = 0;
        qint64 framePoolMisses = 0;
        qint64 mediaOpenSerial = 0;
        qint64 hardwareReadbacks = 0;
        qint64 gpuFramesPresented = 0;
        qint64 gpuFramesRendered = 0;
        qint64 estimatedBandwidthKbps = 0;
        int audioQueueDepth = 0;
        int videoQueueDepth = 0;
        int controlPhase = 0;
        int hlsVariantIndex = -1;
        int hlsVariantCount = 0;
        bool seekPending = false;
        bool hlsAutoMode = true;
        QString clockSource;
        QString hardwareDecode;
        QString renderBackend;
    };

    explicit FFmpegPlayer(QObject* parent = nullptr);
    ~FFmpegPlayer() override;

    void setVideoCanvas(VideoCanvas* canvas);

    void open(const QString& target, qint64 startPositionMs = 0, bool startPaused = false);
    void play();
    void pause();
    void togglePause();
    void stop();

    void setPosition(qint64 milliseconds);
    void seekRelative(qint64 deltaMilliseconds);
    void setPlaybackRate(double rate);
    void stepFrame();
    void setVolume(int volume);
    void setMuted(bool muted);
    void setRecognitionAudioEnabled(bool enabled);
    void setHardwareDecodeEnabled(bool enabled);
    void setD3D11Device(void* device, void* context);

    void selectAudioStream(int streamIndex);
    void selectSubtitleStream(int streamIndex);
    QVector<HlsVariant> hlsVariants() const;
    int activeHlsVariant() const;
    bool hlsAutoMode() const;
    void selectHlsVariant(int index);
    void setHlsAutoMode(bool enabled);

    State state() const;
    qint64 position() const;
    qint64 duration() const;
    double playbackRate() const;
    int volume() const;
    bool isMuted() const;
    bool hasAudio() const;
    bool hasVideo() const;
    int videoRotation() const;
    double videoFrameRate() const;
    int requestedAudioStream() const;
    int requestedSubtitleStream() const;
    int activeAudioStream() const;
    int activeSubtitleStream() const;
    QVector<TrackInfo> audioTracks() const;
    QVector<TrackInfo> subtitleTracks() const;
    QVector<ChapterInfo> chapters() const;
    QString embeddedSubtitleTextAt(qint64 positionMs) const;
    QString currentTarget() const;
    QString mediaInfo() const;
    QString lastError() const;
    QString diagnosticsReport() const;
    bool exportDiagnostics(const QString& path, QString* error = nullptr) const;
    PlaybackStats playbackStats() const;
    QString performanceSummary() const;

signals:
    void stateChanged(FFmpegPlayer::State state);
    void mediaOpened(qint64 durationMs, bool hasAudio, bool hasVideo);
    void mediaInfoChanged(const QString& info);
    void playbackFinished();
    void errorOccurred(const QString& message);
    void seekRequested(qint64 positionMs);
    void audioSamplesReady(const QByteArray& pcm, int sampleRate, int channels,
                           qint64 positionMs);
    void hlsVariantsChanged();
    void hlsVariantChanged(int index);

private:
    struct StreamInfo {
        int index = -1;
        qint64 startTimestamp = AV_NOPTS_VALUE;
        AVRational timeBase{1, 1000};
    };

    struct EmbeddedCue {
        qint64 startMs = 0;
        qint64 endMs = 0;
        QString text;
    };

    void setState(State state);
    void openInternal(const QString& target, qint64 startPositionMs,
                      bool startPaused, bool preserveHlsSession);
    void probeHlsTarget(const QString& target, qint64 startPositionMs, bool startPaused);
    void activateHlsVariant(int index, qint64 startPositionMs, bool startPaused,
                            bool automatic);
    void requestAutomaticHlsSwitch(int index);
    bool requestHlsFallback(const QString& reason);
    void schedulePendingHlsFallback();
    void stopThreads();
    void abortPlayback();
    void run(const QString& target);
    void readPackets();
    bool primeQueues();
    int readFrameWithRecovery(AVPacket* packet);
    void enterBuffering(const QString& reason);
    void leaveBuffering();
    void setQueuePaused(bool paused);
    bool buffersReady(bool initial) const;
    StreamBufferSnapshot audioBufferSnapshot() const;
    StreamBufferSnapshot videoBufferSnapshot() const;
    void processTrackSwitchRequests();
    bool switchAudioStreamNow();
    bool switchSubtitleStreamNow();
    int resolveAudioStreamIndex(int requestedIndex) const;
    int resolveSubtitleStreamIndex(int requestedIndex) const;
    bool openAudioDecoder(int streamIndex);
    bool openSubtitleDecoder(int streamIndex);
    void closeAudioDecoder();
    void closeSubtitleDecoder();
    void audioLoop();
    void videoLoop();

    void updatePosition(qint64 milliseconds);
    bool waitUntil(qint64 mediaMilliseconds, bool allowFrameStep,
                   quint64 requestSerial, quint64 appliedSerial);
    qint64 clockMediaTimeMs() const;
    void setClockPaused(bool paused);
    void setClockRate(double rate);
    void resetClock(qint64 mediaMs, bool paused);
    qint64 packetTimeMs(const AVPacket* packet, int streamIndex) const;
    qint64 packetDurationMs(const AVPacket* packet, int streamIndex) const;
    qint64 frameTimeMs(const AVFrame* frame, int streamIndex) const;

    void setLastError(const QString& message);
    bool handleSeekLocked(qint64 targetMs);
    void closeQueues();

    void decodeSubtitlePacket(AVPacket* packet);
    void clearEmbeddedCues();
    void reloadAtCurrentPosition();
    bool openVideoDecoder(AVFormatContext* format, int streamIndex);
    bool switchVideoDecoderToSoftware();

    static int interruptCallback(void* opaque);
    static AVPixelFormat getHardwareFormat(
        AVCodecContext* codecContext, const AVPixelFormat* pixelFormats);
    static void ffmpegLogCallback(
        void* context, int level, const char* format, va_list arguments);
    void appendDiagnostic(const QString& category, const QString& message);
    static QString stateName(State state);

    mutable std::mutex errorMutex_;
    QString lastError_;

    mutable std::mutex statsMutex_;
    QString hardwareDecodeBackend_ = QStringLiteral("software");
    QString renderBackend_ = QStringLiteral("software-cpu");

    mutable std::mutex targetMutex_;
    QString target_;

    mutable std::mutex infoMutex_;
    QString info_;

    std::atomic<State> state_{State::Idle};
    std::atomic<bool> abort_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> networkTarget_{false};
    std::atomic<bool> hasAudio_{false};
    std::atomic<bool> hasVideo_{false};
    std::atomic<qint64> positionMs_{0};
    std::atomic<qint64> durationMs_{0};
    std::atomic<qint64> seekRequestMs_{-1};
    std::atomic<bool> seekPending_{false};
    std::atomic<quint64> seekRequestSerial_{0};
    std::atomic<quint64> seekAppliedSerial_{0};
    std::atomic<bool> previewPending_{false};
    std::atomic<qint64> dropUntilMs_{0};
    std::atomic<unsigned> audioRebaseSerial_{0};
    std::atomic<qint64> startPositionMs_{0};
    std::atomic<bool> startPaused_{false};
    std::atomic<bool> opening_{false};
    std::atomic<qint64> openDeadlineMs_{0};
    std::atomic<int> videoStepCount_{0};
    std::atomic<double> requestedRate_{1.0};
    std::atomic<int> requestedVolume_{100};
    std::atomic<bool> requestedMuted_{false};
    std::atomic<bool> recognitionAudioEnabled_{false};
    std::atomic<bool> hardwareDecodeEnabled_{true};
    std::atomic<int> videoRotation_{0};
    std::atomic<int> requestedAudioStream_{-1};
    std::atomic<int> requestedSubtitleStream_{-1};
    std::atomic<int> activeAudioStream_{-1};
    std::atomic<int> activeSubtitleStream_{-1};
    std::atomic<bool> bufferingRequested_{false};
    std::atomic<bool> audioSwitchRequested_{false};
    std::atomic<bool> subtitleSwitchRequested_{false};
    std::atomic<unsigned> audioGeneration_{0};
    std::atomic<qint64> audioFramesWritten_{0};
    std::atomic<qint64> videoFramesPresented_{0};
    std::atomic<qint64> videoFramesDropped_{0};
    std::atomic<qint64> videoPacketsDropped_{0};
    std::atomic<qint64> lastAvDriftMs_{0};
    std::atomic<qint64> audioPacketsDecoded_{0};
    std::atomic<qint64> videoPacketsDecoded_{0};
    std::atomic<qint64> audioFramesDecoded_{0};
    std::atomic<qint64> videoFramesDecoded_{0};
    std::atomic<qint64> seekCommandsHandled_{0};
    std::atomic<qint64> bufferedBytes_{0};
    std::atomic<qint64> bufferedDurationMs_{0};
    std::atomic<qint64> rebufferCount_{0};
    std::atomic<qint64> rebufferTotalMs_{0};
    std::atomic<qint64> lastRebufferMs_{0};
    std::atomic<qint64> reconnectCount_{0};
    std::atomic<qint64> mediaOpenSerial_{0};
    std::atomic<int> controlPhase_{0};
    std::atomic<bool> hardwareZeroCopyEnabled_{false};
    std::atomic<qint64> hardwareReadbacks_{0};
    std::atomic<qint64> gpuFramesPresented_{0};
    std::atomic<qint64> estimatedHlsKbps_{0};
    std::atomic<int> activeHlsVariant_{-1};
    std::atomic<bool> hlsAutoMode_{true};

    std::atomic<VideoCanvas*> canvas_{nullptr};

    std::thread controlThread_;
    std::thread audioThread_;
    std::thread videoThread_;
    mutable std::mutex clockMutex_;
    std::unique_ptr<PlaybackClock> clock_;
    AudioClock* audioClock_ = nullptr;
    VideoFramePool framePool_{3};

    mutable std::mutex queueMutex_;
    std::unique_ptr<PacketQueue> audioQueue_;
    std::unique_ptr<PacketQueue> videoQueue_;

    std::mutex codecMutex_;
    AVFormatContext* formatContext_ = nullptr;
    AVCodecContext* audioCodecContext_ = nullptr;
    AVCodecContext* videoCodecContext_ = nullptr;
    AVCodecContext* subtitleCodecContext_ = nullptr;
    AVBufferRef* hwDeviceContext_ = nullptr;
    std::atomic<int> hardwarePixelFormat_{-1};
    StreamInfo audioStream_;
    StreamInfo videoStream_;
    StreamInfo subtitleStream_;

    mutable std::mutex tracksMutex_;
    QVector<TrackInfo> audioTracks_;
    QVector<TrackInfo> subtitleTracks_;

    mutable std::mutex chaptersMutex_;
    QVector<ChapterInfo> chapters_;

    mutable std::mutex subtitleMutex_;
    std::vector<EmbeddedCue> embeddedCues_;

    std::atomic<int> videoWidth_{0};
    std::atomic<int> videoHeight_{0};
    std::atomic<double> videoFrameRate_{0.0};
    std::atomic<int> audioSampleRate_{0};
    std::atomic<int> audioChannels_{0};

    mutable std::mutex d3dDeviceMutex_;
    void* d3d11Device_ = nullptr;
    void* d3d11DeviceContext_ = nullptr;

    std::unique_ptr<QNetworkAccessManager> networkManager_;
    QPointer<QNetworkReply> hlsReply_;
    quint64 hlsProbeSerial_ = 0;
    mutable std::mutex hlsMutex_;
    QVector<HlsVariant> hlsVariants_;
    QUrl hlsMasterUrl_;
    HlsAbrController hlsAbrController_;
    qint64 hlsSampleStartedMs_ = 0;
    qint64 hlsSampleBytes_ = 0;
    qint64 lastHlsAutoSwitchMs_ = 0;
    std::atomic<bool> hlsFallbackAttempted_{false};
    std::atomic<bool> hlsFallbackPending_{false};
    std::atomic<int> hlsFallbackTargetIndex_{-1};
    std::atomic<qint64> hlsFallbackResumeMs_{0};
    std::atomic<bool> hlsFallbackPaused_{false};
    std::atomic<bool> hlsFallbackAutomatic_{false};
    bool hlsAutomaticSwitch_ = false;

    mutable std::mutex diagnosticsMutex_;
    std::deque<QString> diagnostics_;
    qint64 bufferingStartedMs_ = 0;

    static std::atomic<FFmpegPlayer*> logSink_;
};

#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QQueue>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class AiRecognitionService final : public QObject
{
    Q_OBJECT

public:
    struct Config
    {
        QString baseUrl = QStringLiteral("https://api.openai.com/v1");
        QString apiKey;
        QString transcriptionModel = QStringLiteral("whisper-1");
        QString textModel = QStringLiteral("gpt-4o-mini");
        QString visionModel = QStringLiteral("gpt-4o-mini");
        QString transcriptionLanguage = QStringLiteral("auto");
        QString summaryLanguage = QStringLiteral("zh");
        bool audioEnabled = true;
        bool visionEnabled = true;
        int audioChunkSeconds = 12;
        int visionIntervalSeconds = 20;
    };

    explicit AiRecognitionService(QObject* parent = nullptr);
    ~AiRecognitionService() override;

    void setConfig(const Config& config);
    Config config() const;

    bool isRunning() const;
    void start();
    void stop();
    void resetSession();

    void pushAudioSamples(const QByteArray& pcm, int sampleRate, int channels,
                          qint64 positionMs);
    void flushAudio();
    bool pushVideoFrame(const QImage& frame, qint64 positionMs);

signals:
    void runningChanged(bool running);
    void transcriptSegmentReady(const QString& text, qint64 startMs, qint64 endMs);
    void visionNoteReady(const QString& text, qint64 positionMs);
    void coreContentUpdated(const QString& summary);
    void statusChanged(const QString& status);
    void errorOccurred(const QString& message);

private:
    struct AudioChunk
    {
        QByteArray pcm;
        int sampleRate = 0;
        int channels = 0;
        qint64 startMs = 0;
        qint64 endMs = 0;
    };

    void enqueueAudioChunk(AudioChunk chunk);
    void processNextAudioChunk();
    void finishTranscription(QNetworkReply* reply);
    void requestSummary();
    void finishSummary(QNetworkReply* reply);
    void requestVision(const QImage& frame, qint64 positionMs);
    void finishVision(QNetworkReply* reply);
    void scheduleSummary();
    void stopWithError(const QString& message);
    QNetworkReply* postJson(const QString& path, const QByteArray& payload);
    QByteArray wavForChunk(const AudioChunk& chunk) const;
    QString responseError(QNetworkReply* reply, const QByteArray& payload) const;
    QString summarySystemPrompt() const;
    void trimContext(QString* text, int maximumCharacters) const;

    Config config_;
    bool running_ = false;
    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* transcriptionReply_ = nullptr;
    QNetworkReply* summaryReply_ = nullptr;
    QNetworkReply* visionReply_ = nullptr;
    QTimer* summaryDelayTimer_ = nullptr;
    QQueue<AudioChunk> audioQueue_;

    QByteArray bufferedPcm_;
    AudioChunk activeAudioChunk_;
    qint64 activeVisionPositionMs_ = 0;
    int bufferedSampleRate_ = 0;
    int bufferedChannels_ = 0;
    qint64 bufferedStartMs_ = 0;
    qint64 bufferedEndMs_ = 0;

    QString transcriptContext_;
    QString visionContext_;
    QString coreContent_;
    QString previousTranscriptPrompt_;
    bool summaryDirty_ = false;
    qint64 lastSummaryAtMs_ = 0;
};

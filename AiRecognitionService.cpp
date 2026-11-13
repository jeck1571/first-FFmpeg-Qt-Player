#include "AiRecognitionService.h"

#include <QBuffer>
#include <QDateTime>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kWavSampleRate = 16000;
constexpr int kMinimumChunkMs = 450;
constexpr int kMaximumQueuedChunks = 8;
constexpr int kSummaryDebounceMs = 1600;
constexpr int kMinimumSummaryGapMs = 7000;

void appendLe16(QByteArray* output, quint16 value)
{
    output->append(static_cast<char>(value & 0xff));
    output->append(static_cast<char>((value >> 8) & 0xff));
}

void appendLe32(QByteArray* output, quint32 value)
{
    output->append(static_cast<char>(value & 0xff));
    output->append(static_cast<char>((value >> 8) & 0xff));
    output->append(static_cast<char>((value >> 16) & 0xff));
    output->append(static_cast<char>((value >> 24) & 0xff));
}

QString timestampText(qint64 milliseconds)
{
    const qint64 totalSeconds = qMax<qint64>(0, milliseconds) / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

QUrl endpointUrl(const QString& base, const QString& relativePath)
{
    QUrl url(base.trimmed());
    QString path = url.path();
    while (path.endsWith(QChar('/'))) {
        path.chop(1);
    }
    if (path.isEmpty()) {
        path = QStringLiteral("/v1");
    }
    url.setPath(path + relativePath);
    return url;
}

bool hasAudibleSignal(const QByteArray& pcm)
{
    if (pcm.size() < static_cast<qsizetype>(sizeof(qint16))) {
        return false;
    }
    const auto* samples = reinterpret_cast<const qint16*>(pcm.constData());
    const qsizetype count = pcm.size() / static_cast<qsizetype>(sizeof(qint16));
    for (qsizetype index = 0; index < count; ++index) {
        if (std::abs(static_cast<int>(samples[index])) >= 180) {
            return true;
        }
    }
    return false;
}

bool isAuthenticationFailure(QNetworkReply* reply)
{
    if (!reply) {
        return false;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return status == 401 || status == 403;
}

QString chatMessageText(const QJsonObject& root)
{
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return {};
    }
    const QJsonObject message = choices.first().toObject()
                                    .value(QStringLiteral("message")).toObject();
    const QJsonValue content = message.value(QStringLiteral("content"));
    if (content.isString()) {
        return content.toString().trimmed();
    }
    if (content.isArray()) {
        QStringList pieces;
        for (const QJsonValue& value : content.toArray()) {
            const QJsonObject part = value.toObject();
            if (part.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                pieces.append(part.value(QStringLiteral("text")).toString());
            }
        }
        return pieces.join(QChar('\n')).trimmed();
    }
    return {};
}

} // namespace

AiRecognitionService::AiRecognitionService(QObject* parent)
    : QObject(parent)
    , network_(new QNetworkAccessManager(this))
    , summaryDelayTimer_(new QTimer(this))
{
    summaryDelayTimer_->setSingleShot(true);
    connect(summaryDelayTimer_, &QTimer::timeout, this, &AiRecognitionService::requestSummary);
}

AiRecognitionService::~AiRecognitionService()
{
    stop();
}

void AiRecognitionService::setConfig(const Config& config)
{
    config_ = config;
    config_.baseUrl = config_.baseUrl.trimmed();
    while (config_.baseUrl.endsWith(QChar('/'))) {
        config_.baseUrl.chop(1);
    }
    config_.audioChunkSeconds = std::clamp(config_.audioChunkSeconds, 5, 30);
    config_.visionIntervalSeconds = std::clamp(config_.visionIntervalSeconds, 5, 120);
    if (config_.transcriptionModel.trimmed().isEmpty()) {
        config_.transcriptionModel = QStringLiteral("whisper-1");
    }
    if (config_.textModel.trimmed().isEmpty()) {
        config_.textModel = QStringLiteral("gpt-4o-mini");
    }
    if (config_.visionModel.trimmed().isEmpty()) {
        config_.visionModel = config_.textModel;
    }
    if (config_.summaryLanguage != QStringLiteral("en")) {
        config_.summaryLanguage = QStringLiteral("zh");
    }
}

AiRecognitionService::Config AiRecognitionService::config() const
{
    return config_;
}

bool AiRecognitionService::isRunning() const
{
    return running_;
}

void AiRecognitionService::start()
{
    if (running_) {
        return;
    }
    if (!config_.audioEnabled && !config_.visionEnabled) {
        emit errorOccurred(tr("Enable audio recognition or video recognition first."));
        return;
    }
    const QUrl endpoint(config_.baseUrl);
    if (!endpoint.isValid() || endpoint.scheme().isEmpty() || endpoint.host().isEmpty()) {
        emit errorOccurred(tr("Enter a valid OpenAI-compatible API base URL."));
        return;
    }

    resetSession();
    running_ = true;
    emit runningChanged(true);
    emit statusChanged(tr("AI recognition is listening"));
}

void AiRecognitionService::stop()
{
    const bool wasRunning = running_;
    running_ = false;
    summaryDelayTimer_->stop();

    const auto disposeReply = [](QNetworkReply*& reply) {
        if (!reply) {
            return;
        }
        QObject::disconnect(reply, nullptr, nullptr, nullptr);
        reply->abort();
        reply->deleteLater();
        reply = nullptr;
    };
    disposeReply(transcriptionReply_);
    disposeReply(summaryReply_);
    disposeReply(visionReply_);

    audioQueue_.clear();
    bufferedPcm_.clear();
    bufferedSampleRate_ = 0;
    bufferedChannels_ = 0;
    bufferedStartMs_ = 0;
    bufferedEndMs_ = 0;
    activeAudioChunk_ = {};
    summaryDirty_ = false;

    if (wasRunning) {
        emit statusChanged(tr("AI recognition stopped"));
        emit runningChanged(false);
    }
}

void AiRecognitionService::stopWithError(const QString& message)
{
    stop();
    if (!message.isEmpty()) {
        emit errorOccurred(message);
    }
}

void AiRecognitionService::resetSession()
{
    summaryDelayTimer_->stop();
    audioQueue_.clear();
    bufferedPcm_.clear();
    bufferedSampleRate_ = 0;
    bufferedChannels_ = 0;
    bufferedStartMs_ = 0;
    bufferedEndMs_ = 0;
    activeAudioChunk_ = {};
    transcriptContext_.clear();
    visionContext_.clear();
    coreContent_.clear();
    previousTranscriptPrompt_.clear();
    summaryDirty_ = false;
    lastSummaryAtMs_ = 0;

    const auto disposeReply = [](QNetworkReply*& reply) {
        if (!reply) {
            return;
        }
        QObject::disconnect(reply, nullptr, nullptr, nullptr);
        reply->abort();
        reply->deleteLater();
        reply = nullptr;
    };
    disposeReply(transcriptionReply_);
    disposeReply(summaryReply_);
    disposeReply(visionReply_);
    emit coreContentUpdated({});
}

void AiRecognitionService::pushAudioSamples(const QByteArray& pcm, int sampleRate,
                                            int channels, qint64 positionMs)
{
    if (!running_ || !config_.audioEnabled || pcm.isEmpty()
        || sampleRate <= 0 || channels <= 0) {
        return;
    }
    const int frameBytes = channels * static_cast<int>(sizeof(qint16));
    if (frameBytes <= 0) {
        return;
    }
    const qint64 incomingPositionMs = qMax<qint64>(0, positionMs);

    if (!bufferedPcm_.isEmpty()
        && (bufferedSampleRate_ != sampleRate || bufferedChannels_ != channels)) {
        flushAudio();
    } else if (!bufferedPcm_.isEmpty()
               && (incomingPositionMs > bufferedEndMs_ + 1500
                   || incomingPositionMs + 1500 < bufferedStartMs_)) {
        // A seek or stream restart can leave decoded frames from the old
        // position in flight. Close the old segment before accepting the new
        // timeline so transcript timestamps never overlap.
        flushAudio();
    }
    if (bufferedPcm_.isEmpty()) {
        bufferedSampleRate_ = sampleRate;
        bufferedChannels_ = channels;
        bufferedStartMs_ = incomingPositionMs;
        bufferedEndMs_ = bufferedStartMs_;
    }

    bufferedPcm_.append(pcm);
    const qint64 durationMs = static_cast<qint64>(pcm.size()) * 1000
        / (static_cast<qint64>(sampleRate) * frameBytes);
    bufferedEndMs_ = qMax(bufferedEndMs_, incomingPositionMs + durationMs);

    qint64 chunkBytes = static_cast<qint64>(sampleRate) * frameBytes
        * config_.audioChunkSeconds;
    chunkBytes -= chunkBytes % frameBytes;
    while (chunkBytes > 0 && bufferedPcm_.size() >= chunkBytes) {
        AudioChunk chunk;
        chunk.pcm = bufferedPcm_.left(static_cast<qsizetype>(chunkBytes));
        chunk.sampleRate = bufferedSampleRate_;
        chunk.channels = bufferedChannels_;
        chunk.startMs = bufferedStartMs_;
        chunk.endMs = chunk.startMs
            + static_cast<qint64>(chunk.pcm.size()) * 1000
                / (static_cast<qint64>(sampleRate) * frameBytes);
        bufferedPcm_.remove(0, static_cast<qsizetype>(chunkBytes));
        bufferedStartMs_ = chunk.endMs;
        enqueueAudioChunk(std::move(chunk));
    }
}

void AiRecognitionService::flushAudio()
{
    if (bufferedPcm_.isEmpty() || bufferedSampleRate_ <= 0 || bufferedChannels_ <= 0) {
        bufferedPcm_.clear();
        return;
    }
    const int frameBytes = bufferedChannels_ * static_cast<int>(sizeof(qint16));
    const qint64 durationMs = static_cast<qint64>(bufferedPcm_.size()) * 1000
        / (static_cast<qint64>(bufferedSampleRate_) * frameBytes);
    if (durationMs >= kMinimumChunkMs) {
        AudioChunk chunk;
        chunk.pcm = bufferedPcm_;
        chunk.sampleRate = bufferedSampleRate_;
        chunk.channels = bufferedChannels_;
        chunk.startMs = bufferedStartMs_;
        chunk.endMs = qMax(bufferedEndMs_, bufferedStartMs_ + durationMs);
        enqueueAudioChunk(std::move(chunk));
    }
    bufferedPcm_.clear();
    bufferedSampleRate_ = 0;
    bufferedChannels_ = 0;
    bufferedStartMs_ = 0;
    bufferedEndMs_ = 0;
}

void AiRecognitionService::enqueueAudioChunk(AudioChunk chunk)
{
    if (!hasAudibleSignal(chunk.pcm)) {
        return;
    }
    if (audioQueue_.size() >= kMaximumQueuedChunks) {
        emit errorOccurred(tr("The AI audio queue is full; one segment was skipped."));
        return;
    }
    audioQueue_.enqueue(std::move(chunk));
    processNextAudioChunk();
}

void AiRecognitionService::processNextAudioChunk()
{
    if (!running_ || !config_.audioEnabled || transcriptionReply_ || audioQueue_.isEmpty()) {
        return;
    }
    activeAudioChunk_ = audioQueue_.dequeue();

    auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"file\"; filename=\"segment.wav\""));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("audio/wav"));
    filePart.setBody(wavForChunk(activeAudioChunk_));
    multipart->append(filePart);

    QHttpPart modelPart;
    modelPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QStringLiteral("form-data; name=\"model\""));
    modelPart.setBody(config_.transcriptionModel.toUtf8());
    multipart->append(modelPart);

    QHttpPart formatPart;
    formatPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                         QStringLiteral("form-data; name=\"response_format\""));
    formatPart.setBody("json");
    multipart->append(formatPart);

    if (!config_.transcriptionLanguage.isEmpty()
        && config_.transcriptionLanguage.compare(QStringLiteral("auto"),
                                                  Qt::CaseInsensitive) != 0) {
        QHttpPart languagePart;
        languagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                               QStringLiteral("form-data; name=\"language\""));
        languagePart.setBody(config_.transcriptionLanguage.toUtf8());
        multipart->append(languagePart);
    }
    if (!previousTranscriptPrompt_.isEmpty()) {
        QHttpPart promptPart;
        promptPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                             QStringLiteral("form-data; name=\"prompt\""));
        promptPart.setBody(previousTranscriptPrompt_.toUtf8());
        multipart->append(promptPart);
    }

    QNetworkRequest request(
        endpointUrl(config_.baseUrl, QStringLiteral("/audio/transcriptions")));
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("FFmpegQtPlayer-AI/1.0"));
    request.setTransferTimeout(90000);
    if (!config_.apiKey.trimmed().isEmpty()) {
        request.setRawHeader("Authorization",
                             QByteArray("Bearer ") + config_.apiKey.trimmed().toUtf8());
    }

    transcriptionReply_ = network_->post(request, multipart);
    multipart->setParent(transcriptionReply_);
    QNetworkReply* reply = transcriptionReply_;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (transcriptionReply_ != reply) {
            return;
        }
        finishTranscription(reply);
    });
    emit statusChanged(tr("Transcribing audio at %1")
                           .arg(timestampText(activeAudioChunk_.startMs)));
}

void AiRecognitionService::finishTranscription(QNetworkReply* reply)
{
    const QByteArray payload = reply->readAll();
    const QString error = responseError(reply, payload);
    const bool authenticationFailure = isAuthenticationFailure(reply);

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    const QJsonObject root = document.isObject() ? document.object() : QJsonObject();
    QString text = root.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty() && parseError.error == QJsonParseError::NoError
        && !document.isObject()) {
        text = QString::fromUtf8(payload).trimmed();
    }

    transcriptionReply_->deleteLater();
    transcriptionReply_ = nullptr;

    if (!error.isEmpty() || text.isEmpty()) {
        if (!error.isEmpty()) {
            if (authenticationFailure) {
                stopWithError(error);
            } else {
                emit errorOccurred(error);
            }
        }
        if (authenticationFailure) {
            return;
        }
        processNextAudioChunk();
        return;
    }

    const QString line = tr("[%1-%2] %3")
                             .arg(timestampText(activeAudioChunk_.startMs),
                                  timestampText(activeAudioChunk_.endMs),
                                  text);
    if (!transcriptContext_.isEmpty()) {
        transcriptContext_.append(QChar('\n'));
    }
    transcriptContext_.append(line);
    trimContext(&transcriptContext_, 16000);
    previousTranscriptPrompt_ = text.right(800);
    emit transcriptSegmentReady(text, activeAudioChunk_.startMs, activeAudioChunk_.endMs);
    emit statusChanged(tr("Audio segment recognized"));
    summaryDirty_ = true;
    scheduleSummary();
    processNextAudioChunk();
}

bool AiRecognitionService::pushVideoFrame(const QImage& frame, qint64 positionMs)
{
    if (!running_ || !config_.visionEnabled || frame.isNull() || visionReply_) {
        return false;
    }
    requestVision(frame, qMax<qint64>(0, positionMs));
    return true;
}

void AiRecognitionService::requestVision(const QImage& frame, qint64 positionMs)
{
    QImage scaled = frame;
    if (scaled.width() > 960 || scaled.height() > 540) {
        scaled = scaled.scaled(960, 540, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!scaled.save(&buffer, "JPG", 76) || jpeg.isEmpty()) {
        return;
    }

    const bool chinese = config_.summaryLanguage == QStringLiteral("zh");
    const QString prompt = chinese
        ? QStringLiteral("请用简体中文简要描述这一视频画面中的人物、物体、动作、场景和屏幕文字。"
                         "只保留与理解当前内容有关的信息。")
        : QStringLiteral("Briefly describe the people, objects, actions, scene, and "
                         "on-screen text in this video frame. Keep only information "
                         "relevant to understanding the current content.");

    QJsonObject imageUrl;
    imageUrl.insert(QStringLiteral("url"),
                    QStringLiteral("data:image/jpeg;base64,")
                        + QString::fromLatin1(jpeg.toBase64()));
    QJsonObject imagePart;
    imagePart.insert(QStringLiteral("type"), QStringLiteral("image_url"));
    imagePart.insert(QStringLiteral("image_url"), imageUrl);
    QJsonObject textPart;
    textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
    textPart.insert(QStringLiteral("text"), prompt);
    QJsonArray content{textPart, imagePart};
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), content);
    QJsonObject payload;
    payload.insert(QStringLiteral("model"), config_.visionModel);
    payload.insert(QStringLiteral("messages"), QJsonArray{message});
    payload.insert(QStringLiteral("temperature"), 0.2);
    payload.insert(QStringLiteral("max_tokens"), 220);

    activeVisionPositionMs_ = positionMs;
    visionReply_ = postJson(QStringLiteral("/chat/completions"),
                            QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QNetworkReply* reply = visionReply_;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (visionReply_ != reply) {
            return;
        }
        finishVision(reply);
    });
    emit statusChanged(tr("Analyzing the current video frame"));
}

void AiRecognitionService::finishVision(QNetworkReply* reply)
{
    const QByteArray payload = reply->readAll();
    const QString error = responseError(reply, payload);
    const bool authenticationFailure = isAuthenticationFailure(reply);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    const QString text = parseError.error == QJsonParseError::NoError
        ? chatMessageText(document.object())
        : QString();

    visionReply_->deleteLater();
    visionReply_ = nullptr;
    if (!error.isEmpty()) {
        if (authenticationFailure) {
            stopWithError(error);
        } else {
            emit errorOccurred(error);
        }
        return;
    }
    if (text.isEmpty()) {
        return;
    }

    const QString line = tr("[%1] %2")
                             .arg(timestampText(activeVisionPositionMs_), text);
    if (!visionContext_.isEmpty()) {
        visionContext_.append(QChar('\n'));
    }
    visionContext_.append(line);
    trimContext(&visionContext_, 9000);
    emit visionNoteReady(text, activeVisionPositionMs_);
    emit statusChanged(tr("Video frame recognized"));
    summaryDirty_ = true;
    scheduleSummary();
}

void AiRecognitionService::scheduleSummary()
{
    if (!running_ || (transcriptContext_.isEmpty() && visionContext_.isEmpty())) {
        return;
    }
    summaryDirty_ = true;
    if (summaryDelayTimer_->isActive() || summaryReply_) {
        return;
    }
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - lastSummaryAtMs_;
    const int delay = lastSummaryAtMs_ == 0
        ? kSummaryDebounceMs
        : static_cast<int>(qMax<qint64>(kSummaryDebounceMs,
                                        kMinimumSummaryGapMs - elapsed));
    summaryDelayTimer_->start(delay);
}

void AiRecognitionService::requestSummary()
{
    if (!running_ || !summaryDirty_
        || (transcriptContext_.isEmpty() && visionContext_.isEmpty())) {
        return;
    }
    if (summaryReply_) {
        summaryDelayTimer_->start(kSummaryDebounceMs);
        return;
    }

    summaryDirty_ = false;
    lastSummaryAtMs_ = QDateTime::currentMSecsSinceEpoch();
    const bool chinese = config_.summaryLanguage == QStringLiteral("zh");
    QString userText = chinese ? QStringLiteral("语音转写：\n")
                               : QStringLiteral("Audio transcript:\n");
    userText += transcriptContext_.isEmpty()
        ? (chinese ? QStringLiteral("（暂无）") : QStringLiteral("(none)"))
        : transcriptContext_;
    userText += chinese ? QStringLiteral("\n\n画面记录：\n")
                        : QStringLiteral("\n\nVideo observations:\n");
    userText += visionContext_.isEmpty()
        ? (chinese ? QStringLiteral("（暂无）") : QStringLiteral("(none)"))
        : visionContext_;
    if (!coreContent_.isEmpty()) {
        userText += chinese ? QStringLiteral("\n\n上一版核心内容：\n")
                            : QStringLiteral("\n\nPrevious core summary:\n");
        userText += coreContent_;
    }

    QJsonObject systemMessage;
    systemMessage.insert(QStringLiteral("role"), QStringLiteral("system"));
    systemMessage.insert(QStringLiteral("content"), summarySystemPrompt());
    QJsonObject userMessage;
    userMessage.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMessage.insert(QStringLiteral("content"), userText);
    QJsonObject payload;
    payload.insert(QStringLiteral("model"), config_.textModel);
    payload.insert(QStringLiteral("messages"), QJsonArray{systemMessage, userMessage});
    payload.insert(QStringLiteral("temperature"), 0.2);
    payload.insert(QStringLiteral("max_tokens"), 700);

    summaryReply_ = postJson(QStringLiteral("/chat/completions"),
                             QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QNetworkReply* reply = summaryReply_;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (summaryReply_ != reply) {
            return;
        }
        finishSummary(reply);
    });
    emit statusChanged(tr("Updating core content"));
}

void AiRecognitionService::finishSummary(QNetworkReply* reply)
{
    const QByteArray payload = reply->readAll();
    const QString error = responseError(reply, payload);
    const bool authenticationFailure = isAuthenticationFailure(reply);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    const QString summary = parseError.error == QJsonParseError::NoError
        ? chatMessageText(document.object())
        : QString();

    summaryReply_->deleteLater();
    summaryReply_ = nullptr;
    if (!error.isEmpty()) {
        if (authenticationFailure) {
            stopWithError(error);
            return;
        }
        emit errorOccurred(error);
    } else if (!summary.isEmpty()) {
        coreContent_ = summary;
        emit coreContentUpdated(coreContent_);
        emit statusChanged(tr("Core content updated"));
    }
    if (summaryDirty_) {
        scheduleSummary();
    }
}

QNetworkReply* AiRecognitionService::postJson(const QString& path, const QByteArray& payload)
{
    QNetworkRequest request(endpointUrl(config_.baseUrl, path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("FFmpegQtPlayer-AI/1.0"));
    request.setTransferTimeout(90000);
    if (!config_.apiKey.trimmed().isEmpty()) {
        request.setRawHeader("Authorization",
                             QByteArray("Bearer ") + config_.apiKey.trimmed().toUtf8());
    }
    return network_->post(request, payload);
}

QByteArray AiRecognitionService::wavForChunk(const AudioChunk& chunk) const
{
    if (chunk.sampleRate <= 0 || chunk.channels <= 0 || chunk.pcm.isEmpty()) {
        return {};
    }
    const int channelCount = chunk.channels;
    const qsizetype sourceFrames = chunk.pcm.size()
        / (channelCount * static_cast<qsizetype>(sizeof(qint16)));
    const qsizetype targetFrames = qMax<qsizetype>(
        1, sourceFrames * kWavSampleRate / chunk.sampleRate);
    const auto* source = reinterpret_cast<const qint16*>(chunk.pcm.constData());

    QByteArray mono(static_cast<qsizetype>(targetFrames) * static_cast<qsizetype>(sizeof(qint16)),
                    Qt::Uninitialized);
    auto* destination = reinterpret_cast<qint16*>(mono.data());
    for (qsizetype index = 0; index < targetFrames; ++index) {
        const double sourcePosition = static_cast<double>(index) * chunk.sampleRate
            / kWavSampleRate;
        const qsizetype first = qMin(sourceFrames - 1,
                                     static_cast<qsizetype>(std::floor(sourcePosition)));
        const qsizetype second = qMin(sourceFrames - 1, first + 1);
        const double fraction = sourcePosition - std::floor(sourcePosition);
        double left = 0.0;
        double right = 0.0;
        for (int channel = 0; channel < channelCount; ++channel) {
            left += source[first * channelCount + channel];
            right += source[second * channelCount + channel];
        }
        left /= channelCount;
        right /= channelCount;
        const double sample = left + (right - left) * fraction;
        destination[index] = static_cast<qint16>(
            std::clamp(std::lround(sample), -32768L, 32767L));
    }

    QByteArray wav;
    wav.reserve(44 + mono.size());
    wav.append("RIFF", 4);
    appendLe32(&wav, static_cast<quint32>(36 + mono.size()));
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    appendLe32(&wav, 16);
    appendLe16(&wav, 1);
    appendLe16(&wav, 1);
    appendLe32(&wav, kWavSampleRate);
    appendLe32(&wav, kWavSampleRate * 2);
    appendLe16(&wav, 2);
    appendLe16(&wav, 16);
    wav.append("data", 4);
    appendLe32(&wav, static_cast<quint32>(mono.size()));
    wav.append(mono);
    return wav;
}

QString AiRecognitionService::responseError(QNetworkReply* reply,
                                            const QByteArray& payload) const
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error == QJsonParseError::NoError && document.isObject()) {
        const QJsonObject error = document.object()
                                      .value(QStringLiteral("error")).toObject();
        const QString message = error.value(QStringLiteral("message")).toString().trimmed();
        if (!message.isEmpty()) {
            return message;
        }
    }
    if (reply->error() != QNetworkReply::NoError) {
        return reply->errorString();
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {
        return tr("AI service returned HTTP %1").arg(status);
    }
    return {};
}

QString AiRecognitionService::summarySystemPrompt() const
{
    if (config_.summaryLanguage == QStringLiteral("en")) {
        return QStringLiteral("You are a real-time video content analyst. Use the audio "
                              "transcript and video observations to maintain the current "
                              "core content. Write 3-6 concise bullets in English. Focus "
                              "on the topic, people, key events, important facts, and "
                              "conclusions. Do not repeat every sentence and do not "
                              "invent details that are not supported by the provided "
                              "material.");
    }
    return QStringLiteral("你是实时视频内容分析助手。根据语音转写和画面记录，持续维护当前视频的核心内容。"
                          "请用简体中文输出 3-6 条简短要点，聚焦主题、人物、关键事件、重要事实和结论。"
                          "不要逐句复述转写，也不要编造材料中没有的信息。");
}

void AiRecognitionService::trimContext(QString* text, int maximumCharacters) const
{
    if (!text || text->size() <= maximumCharacters) {
        return;
    }
    *text = text->right(maximumCharacters);
    const int newline = text->indexOf(QChar('\n'));
    if (newline >= 0 && newline < 120) {
        text->remove(0, newline + 1);
    }
}

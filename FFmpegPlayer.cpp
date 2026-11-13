#include "FFmpegPlayer.h"

#include "D3d11ContextSync.h"
#include "PlaybackClock.h"
#include "VideoCanvas.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QMediaDevices>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <QThread>
#include <QUrl>

#include <d3d11.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace {

using SteadyClock = std::chrono::steady_clock;

QString ffmpegError(int errorCode)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

qint64 monotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               SteadyClock::now().time_since_epoch())
        .count();
}

qint64 rationalToMilliseconds(qint64 timestamp, AVRational timeBase)
{
    return av_rescale_q(timestamp, timeBase, AVRational{1, 1000});
}

int channelCount(const AVFrame* frame, const AVCodecContext* codec)
{
    if (frame && frame->channels > 0) {
        return frame->channels;
    }
    return codec && codec->channels > 0 ? codec->channels : 2;
}

qint64 channelLayout(const AVFrame* frame, const AVCodecContext* codec, int channels)
{
    if (frame && frame->channel_layout != 0) {
        return static_cast<qint64>(frame->channel_layout);
    }
    if (codec && codec->channel_layout != 0) {
        return static_cast<qint64>(codec->channel_layout);
    }
    return av_get_default_channel_layout(channels);
}

void applyVolume(QByteArray* pcm, float gain)
{
    if (!pcm || pcm->isEmpty() || qFuzzyCompare(gain, 1.0f)) {
        return;
    }

    auto* samples = reinterpret_cast<qint16*>(pcm->data());
    const qsizetype count = pcm->size() / static_cast<qsizetype>(sizeof(qint16));
    for (qsizetype index = 0; index < count; ++index) {
        int value = static_cast<int>(std::lround(samples[index] * gain));
        samples[index] = static_cast<qint16>(std::clamp(value, -32768, 32767));
    }
}

bool isTextSubtitleCodec(AVCodecID codecId)
{
    switch (codecId) {
    case AV_CODEC_ID_SUBRIP:
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_SSA:
    case AV_CODEC_ID_MOV_TEXT:
    case AV_CODEC_ID_WEBVTT:
    case AV_CODEC_ID_TEXT:
    case AV_CODEC_ID_MICRODVD:
    case AV_CODEC_ID_SUBVIEWER:
    case AV_CODEC_ID_SUBVIEWER1:
    case AV_CODEC_ID_SAMI:
    case AV_CODEC_ID_REALTEXT:
    case AV_CODEC_ID_EIA_608:
    case AV_CODEC_ID_STL:
    case AV_CODEC_ID_HDMV_TEXT_SUBTITLE:
        return true;
    default:
        return false;
    }
}

QString cleanSubtitleMarkup(QString text)
{
    text.replace(QRegularExpression(QStringLiteral("<[^>]+>")), QString());
    text.replace(QRegularExpression(QStringLiteral("\\{[^}]*\\}")), QString());
    text.replace(QStringLiteral("\\N"), QStringLiteral("\n"));
    text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    text.replace(QStringLiteral("\\h"), QStringLiteral(" "));
    text.remove(QChar('\r'));
    return text.trimmed();
}

QString assDialogueText(const QString& payload)
{
    // ASS rect payloads come either as the raw dialogue fields
    // (ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text)
    // or as a full "Dialogue: Layer,Start,End,..." line produced by
    // decoders that convert other formats (MOV text) to ASS.
    QString body = payload;
    int fieldCount = 8;
    const int colon = body.indexOf(QChar(':'));
    if (colon >= 0
        && body.left(colon).trimmed().compare(QStringLiteral("Dialogue"), Qt::CaseInsensitive) == 0) {
        body = body.mid(colon + 1);
        fieldCount = 9;
    }
    int commas = 0;
    for (int index = 0; index < body.size(); ++index) {
        if (body.at(index) == QChar(',') && ++commas == fieldCount) {
            return body.mid(index + 1);
        }
    }
    return body;
}

QString streamDescription(const AVFormatContext* format, int index)
{
    const AVStream* stream = format->streams[index];
    const AVCodecContext* codec = stream->codec;
    QStringList parts;
    const AVDictionaryEntry* title = av_dict_get(stream->metadata, "title", nullptr, 0);
    const AVDictionaryEntry* language = av_dict_get(stream->metadata, "language", nullptr, 0);
    if (title && *title->value) {
        parts.append(QString::fromUtf8(title->value));
    }
    if (language && *language->value) {
        parts.append(QString::fromUtf8(language->value));
    }
    parts.append(QString::fromUtf8(avcodec_get_name(codec->codec_id)));
    if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (codec->sample_rate > 0) {
            parts.append(QStringLiteral("%1 Hz").arg(codec->sample_rate));
        }
        if (codec->channels > 0) {
            parts.append(QObject::tr("%1 ch").arg(codec->channels));
        }
    }
    return parts.join(QStringLiteral(" · "));
}

int displayRotation(const AVStream* stream)
{
    if (!stream) {
        return 0;
    }
    for (int index = 0; index < stream->nb_side_data; ++index) {
        const AVPacketSideData& sideData = stream->side_data[index];
        if (sideData.type != AV_PKT_DATA_DISPLAYMATRIX || !sideData.data) {
            continue;
        }
        const double theta = av_display_rotation_get(
            reinterpret_cast<const int32_t*>(sideData.data));
        if (!std::isfinite(theta)) {
            continue;
        }
        int degrees = static_cast<int>(std::lround(theta)) % 360;
        if (degrees < 0) {
            degrees += 360;
        }
        return degrees;
    }
    return 0;
}

} // namespace

std::atomic<FFmpegPlayer*> FFmpegPlayer::logSink_{nullptr};

FFmpegPlayer::FFmpegPlayer(QObject* parent)
    : QObject(parent)
    , networkManager_(std::make_unique<QNetworkAccessManager>())
{
    av_register_all();
    avfilter_register_all();
    avformat_network_init();
    logSink_.store(this, std::memory_order_release);
    av_log_set_callback(&FFmpegPlayer::ffmpegLogCallback);
    appendDiagnostic(QStringLiteral("player"), QStringLiteral("Player created"));
}

FFmpegPlayer::~FFmpegPlayer()
{
    stopThreads();
    if (hlsReply_) {
        hlsReply_->abort();
        hlsReply_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(d3dDeviceMutex_);
        if (d3d11DeviceContext_) {
            static_cast<ID3D11DeviceContext*>(d3d11DeviceContext_)->Release();
            d3d11DeviceContext_ = nullptr;
        }
        if (d3d11Device_) {
            static_cast<ID3D11Device*>(d3d11Device_)->Release();
            d3d11Device_ = nullptr;
        }
    }
    FFmpegPlayer* expected = this;
    if (logSink_.compare_exchange_strong(expected, nullptr)) {
        av_log_set_callback(av_log_default_callback);
    }
    avformat_network_deinit();
}

void FFmpegPlayer::setVideoCanvas(VideoCanvas* canvas)
{
    canvas_.store(canvas, std::memory_order_release);
}

void FFmpegPlayer::open(const QString& target, qint64 startPositionMs, bool startPaused)
{
    if (target.trimmed().isEmpty()) {
        return;
    }
    ++hlsProbeSerial_;
    if (hlsReply_) {
        hlsReply_->abort();
        hlsReply_->deleteLater();
        hlsReply_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(hlsMutex_);
        hlsVariants_.clear();
        hlsMasterUrl_ = {};
    }
    activeHlsVariant_ = -1;
    hlsAutoMode_ = true;
    estimatedHlsKbps_ = 0;
    hlsAbrController_.reset();
    hlsFallbackAttempted_ = false;
    hlsFallbackPending_ = false;
    hlsFallbackTargetIndex_ = -1;

    if (HlsPlaylist::isHlsTarget(target)) {
        probeHlsTarget(target, startPositionMs, startPaused);
    } else {
        openInternal(target, startPositionMs, startPaused, false);
    }
}

void FFmpegPlayer::probeHlsTarget(
    const QString& target, qint64 startPositionMs, bool startPaused)
{
    const QUrl sourceUrl = QUrl::fromUserInput(target);
    if (sourceUrl.isLocalFile()) {
        QFile file(sourceUrl.toLocalFile());
        if (!file.open(QIODevice::ReadOnly)) {
            openInternal(target, startPositionMs, startPaused, false);
            return;
        }
        const QByteArray playlist = file.readAll();
        const QVector<HlsVariant> variants = HlsPlaylist::parseMaster(
            playlist, sourceUrl);
        if (variants.isEmpty()) {
            openInternal(target, startPositionMs, startPaused, false);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(hlsMutex_);
            hlsVariants_ = variants;
            hlsMasterUrl_ = sourceUrl;
        }
        emit hlsVariantsChanged();
        activateHlsVariant(0, startPositionMs, startPaused, true);
        return;
    }

    const quint64 probeSerial = hlsProbeSerial_;
    setState(State::Opening);
    QNetworkRequest request(sourceUrl);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "FFmpegQtPlayer/2.0");
    QNetworkReply* reply = networkManager_->get(request);
    hlsReply_ = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, probeSerial, sourceUrl, startPositionMs, startPaused] {
        const QByteArray playlist = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const QString errorText = reply->errorString();
        reply->deleteLater();
        if (hlsReply_ == reply) {
            hlsReply_.clear();
        }
        if (probeSerial != hlsProbeSerial_ || abort_.load()) {
            return;
        }
        if (error != QNetworkReply::NoError) {
            appendDiagnostic(
                QStringLiteral("hls"),
                QStringLiteral("Master playlist probe failed: %1").arg(errorText));
            openInternal(sourceUrl.toString(), startPositionMs, startPaused, false);
            return;
        }

        const QVector<HlsVariant> variants = HlsPlaylist::parseMaster(
            playlist, reply->url().isEmpty() ? sourceUrl : reply->url());
        if (variants.isEmpty()) {
            openInternal(sourceUrl.toString(), startPositionMs, startPaused, false);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(hlsMutex_);
            hlsVariants_ = variants;
            hlsMasterUrl_ = reply->url().isEmpty() ? sourceUrl : reply->url();
        }
        appendDiagnostic(
            QStringLiteral("hls"),
            QStringLiteral("Master playlist loaded with %1 variants")
                .arg(variants.size()));
        emit hlsVariantsChanged();
        activateHlsVariant(0, startPositionMs, startPaused, true);
    });
}

void FFmpegPlayer::openInternal(
    const QString& target, qint64 startPositionMs, bool startPaused,
    bool preserveHlsSession)
{
    if (target.trimmed().isEmpty()) {
        return;
    }

    hlsFallbackPending_ = false;
    stopThreads();
    if (!preserveHlsSession) {
        std::lock_guard<std::mutex> lock(hlsMutex_);
        hlsVariants_.clear();
        hlsMasterUrl_ = {};
        activeHlsVariant_ = -1;
        hlsAutoMode_ = true;
        estimatedHlsKbps_ = 0;
        hlsAbrController_.reset();
    }
    abort_ = false;
    networkTarget_ = StreamingPolicy::isNetworkTarget(target);
    paused_ = startPaused;
    seekRequestMs_ = -1;
    seekPending_ = false;
    seekRequestSerial_ = 0;
    seekAppliedSerial_ = 0;
    previewPending_ = false;
    dropUntilMs_ = 0;
    videoStepCount_ = 0;
    startPositionMs_ = qMax<qint64>(0, startPositionMs);
    startPaused_ = startPaused;
    positionMs_ = startPositionMs_.load();
    durationMs_ = 0;
    hasAudio_ = false;
    hasVideo_ = false;
    videoRotation_ = 0;
    activeAudioStream_ = -1;
    activeSubtitleStream_ = -1;
    audioFramesWritten_ = 0;
    videoFramesPresented_ = 0;
    videoFramesDropped_ = 0;
    videoPacketsDropped_ = 0;
    lastAvDriftMs_ = 0;
    audioPacketsDecoded_ = 0;
    videoPacketsDecoded_ = 0;
    audioFramesDecoded_ = 0;
    videoFramesDecoded_ = 0;
    seekCommandsHandled_ = 0;
    bufferedBytes_ = 0;
    bufferedDurationMs_ = 0;
    rebufferCount_ = 0;
    rebufferTotalMs_ = 0;
    lastRebufferMs_ = 0;
    reconnectCount_ = 0;
    bufferingRequested_ = false;
    audioSwitchRequested_ = false;
    subtitleSwitchRequested_ = false;
    bufferingStartedMs_ = 0;
    hlsSampleStartedMs_ = monotonicMilliseconds();
    hlsSampleBytes_ = 0;
    mediaOpenSerial_.fetch_add(1, std::memory_order_acq_rel);
    controlPhase_ = 0;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        hardwareDecodeBackend_ = QStringLiteral("software");
    }
    opening_ = true;
    openDeadlineMs_ = monotonicMilliseconds() + 30000;
    {
        std::lock_guard<std::mutex> lock(targetMutex_);
        target_ = target;
    }
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        info_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(tracksMutex_);
        audioTracks_.clear();
        subtitleTracks_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(chaptersMutex_);
        chapters_.clear();
    }
    clearEmbeddedCues();
    framePool_.reset();
    setLastError({});
    setState(State::Opening);
    appendDiagnostic(
        QStringLiteral("player"),
        QStringLiteral("Opening target=%1 startMs=%2 network=%3")
            .arg(target)
            .arg(startPositionMs)
            .arg(networkTarget_.load() ? QStringLiteral("true") : QStringLiteral("false")));

    {
        std::lock_guard<std::mutex> lock(clockMutex_);
        audioClock_ = nullptr;
        clock_ = std::make_unique<WallClock>(startPositionMs_.load());
        clock_->setRate(requestedRate_.load());
        clock_->setPaused(startPaused);
    }
    controlThread_ = std::thread([this, target] { run(target); });
}

QVector<HlsVariant> FFmpegPlayer::hlsVariants() const
{
    std::lock_guard<std::mutex> lock(hlsMutex_);
    return hlsVariants_;
}

int FFmpegPlayer::activeHlsVariant() const
{
    return activeHlsVariant_.load(std::memory_order_acquire);
}

bool FFmpegPlayer::hlsAutoMode() const
{
    return hlsAutoMode_.load(std::memory_order_acquire);
}

void FFmpegPlayer::activateHlsVariant(
    int index, qint64 startPositionMs, bool startPaused, bool automatic)
{
    HlsVariant variant;
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(hlsMutex_);
        count = hlsVariants_.size();
        if (index >= 0 && index < count) {
            variant = hlsVariants_.at(index);
        }
    }
    if (index < 0 || index >= count || !variant.url.isValid()) {
        return;
    }

    activeHlsVariant_ = index;
    hlsAutomaticSwitch_ = automatic;
    appendDiagnostic(
        QStringLiteral("hls"),
        QStringLiteral("Selecting variant %1/%2 %3")
            .arg(index + 1)
            .arg(count)
            .arg(variant.label()));
    openInternal(
        variant.url.toString(),
        qMax<qint64>(0, startPositionMs),
        startPaused,
        true);
    emit hlsVariantChanged(index);
}

void FFmpegPlayer::selectHlsVariant(int index)
{
    if (index < 0) {
        setHlsAutoMode(true);
        return;
    }
    hlsAutoMode_ = false;
    hlsFallbackAttempted_ = false;
    hlsFallbackPending_ = false;
    hlsFallbackTargetIndex_ = -1;
    if (index == activeHlsVariant_.load(std::memory_order_acquire)) {
        emit hlsVariantChanged(index);
        return;
    }
    const bool paused = state_.load() == State::Paused || paused_.load();
    activateHlsVariant(index, positionMs_.load(), paused, false);
}

void FFmpegPlayer::setHlsAutoMode(bool enabled)
{
    hlsAutoMode_ = enabled;
    hlsFallbackAttempted_ = false;
    hlsFallbackPending_ = false;
    hlsFallbackTargetIndex_ = -1;
    if (!enabled) {
        return;
    }

    QVector<HlsVariant> variants = hlsVariants();
    if (variants.isEmpty()) {
        return;
    }
    const int chosen = hlsAbrController_.chooseVariant(
        variants, bufferedDurationMs_.load(), state_.load() == State::Buffering);
    if (chosen >= 0
        && chosen != activeHlsVariant_.load(std::memory_order_acquire)) {
        activateHlsVariant(chosen, positionMs_.load(), paused_.load(), true);
    } else {
        emit hlsVariantChanged(activeHlsVariant_.load(std::memory_order_acquire));
    }
}

void FFmpegPlayer::requestAutomaticHlsSwitch(int index)
{
    if (!hlsAutoMode_.load(std::memory_order_acquire)
        || index < 0
        || index == activeHlsVariant_.load(std::memory_order_acquire)
        || monotonicMilliseconds() - lastHlsAutoSwitchMs_ < 7000) {
        return;
    }
    lastHlsAutoSwitchMs_ = monotonicMilliseconds();
    const bool paused = state_.load() == State::Paused || paused_.load();
    QMetaObject::invokeMethod(
        this,
        [this, index, paused] {
            if (!abort_.load()) {
                activateHlsVariant(index, positionMs_.load(), paused, true);
            }
        },
        Qt::QueuedConnection);
}

bool FFmpegPlayer::requestHlsFallback(const QString& reason)
{
    if (hlsFallbackAttempted_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }

    int targetIndex = -1;
    int activeIndex = activeHlsVariant_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(hlsMutex_);
        if (hlsVariants_.size() < 2 || activeIndex <= 0) {
            return false;
        }
        targetIndex = activeIndex - 1;
    }
    if (targetIndex < 0) {
        return false;
    }

    const qint64 resumeMs = positionMs_.load();
    const bool paused = paused_.load();
    const bool automatic = hlsAutoMode_.load();
    hlsFallbackTargetIndex_ = targetIndex;
    hlsFallbackResumeMs_ = resumeMs;
    hlsFallbackPaused_ = paused;
    hlsFallbackAutomatic_ = automatic;
    hlsFallbackPending_ = true;
    appendDiagnostic(
        QStringLiteral("hls"),
        QStringLiteral("Falling back from variant %1 to %2 at %3ms: %4")
            .arg(activeIndex)
            .arg(targetIndex)
            .arg(resumeMs)
            .arg(reason));
    setState(State::Opening);
    return true;
}

void FFmpegPlayer::schedulePendingHlsFallback()
{
    if (!hlsFallbackPending_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const int targetIndex = hlsFallbackTargetIndex_.exchange(
        -1, std::memory_order_acq_rel);
    const qint64 resumeMs = hlsFallbackResumeMs_.load(std::memory_order_acquire);
    const bool paused = hlsFallbackPaused_.load(std::memory_order_acquire);
    const bool automatic = hlsFallbackAutomatic_.load(std::memory_order_acquire);
    if (targetIndex < 0) {
        return;
    }

    // The control thread calls this only after the failed session has closed
    // its decoders and queues. The GUI thread can then join that finished
    // thread and start the lower variant without touching a live session.
    QMetaObject::invokeMethod(
        this,
        [this, targetIndex, resumeMs, paused, automatic] {
            if (!abort_.load()) {
                activateHlsVariant(targetIndex, resumeMs, paused, automatic);
            }
        },
        Qt::QueuedConnection);
}

void FFmpegPlayer::play()
{
    const State current = state_.load();
    if (current == State::Buffering
        || current == State::Stalled
        || current == State::Reconnecting) {
        paused_ = false;
        setClockPaused(false);
        return;
    }
    if (current == State::Paused) {
        paused_ = false;
        setClockPaused(false);
        setState(State::Playing);
        return;
    }
    if (current == State::Stopped || current == State::Ended || current == State::Idle) {
        const QString currentTarget = this->currentTarget();
        if (!currentTarget.isEmpty()) {
            open(currentTarget);
        }
    }
}

void FFmpegPlayer::pause()
{
    if (state_.load() != State::Playing) {
        return;
    }
    paused_ = true;
    setClockPaused(true);
    setState(State::Paused);
}

void FFmpegPlayer::togglePause()
{
    if (state_.load() == State::Playing) {
        pause();
    } else if (state_.load() == State::Paused) {
        play();
    } else {
        play();
    }
}

void FFmpegPlayer::stop()
{
    stopThreads();
    setClockPaused(true);
    paused_ = true;
    positionMs_ = 0;
    setState(State::Stopped);
}

void FFmpegPlayer::setPosition(qint64 milliseconds)
{
    const State current = state_.load();
    if (current == State::Ended) {
        // Seeking after playback finished reopens the media at the requested
        // position in a paused state, so the slider stays usable.
        const QString endedTarget = currentTarget();
        if (!endedTarget.isEmpty()) {
            emit seekRequested(milliseconds);
            open(endedTarget, milliseconds, true);
        }
        return;
    }
    if (current == State::Idle || current == State::Stopped || current == State::Error) {
        return;
    }

    const qint64 duration = durationMs_.load();
    milliseconds = qBound<qint64>(0, milliseconds, duration > 0 ? duration : milliseconds);
    seekRequestSerial_.fetch_add(1, std::memory_order_acq_rel);
    seekRequestMs_ = milliseconds;
    seekPending_ = true;
    positionMs_ = milliseconds;
    emit seekRequested(milliseconds);

    std::lock_guard<std::mutex> lock(queueMutex_);
    if (audioQueue_) {
        audioQueue_->setPaused(true);
    }
    if (videoQueue_) {
        videoQueue_->setPaused(true);
    }
}

void FFmpegPlayer::seekRelative(qint64 deltaMilliseconds)
{
    setPosition(positionMs_.load() + deltaMilliseconds);
}

void FFmpegPlayer::setPlaybackRate(double rate)
{
    rate = std::clamp(rate, 0.25, 4.0);
    requestedRate_ = rate;
    setClockRate(rate);
    if (hasAudio_.load()) {
        audioRebaseSerial_.fetch_add(1, std::memory_order_acq_rel);
    }
}

void FFmpegPlayer::stepFrame()
{
    if (!paused_.load() || !hasVideo_.load() || videoStepCount_.load() >= 8) {
        return;
    }
    videoStepCount_.fetch_add(1);
}

void FFmpegPlayer::setVolume(int volume)
{
    requestedVolume_ = std::clamp(volume, 0, 125);
}

void FFmpegPlayer::setMuted(bool muted)
{
    requestedMuted_ = muted;
}

void FFmpegPlayer::setRecognitionAudioEnabled(bool enabled)
{
    recognitionAudioEnabled_ = enabled;
}

void FFmpegPlayer::setHardwareDecodeEnabled(bool enabled)
{
    hardwareDecodeEnabled_ = enabled;
}

void FFmpegPlayer::setD3D11Device(void* device, void* context)
{
    std::lock_guard<std::mutex> lock(d3dDeviceMutex_);
    if (d3d11Device_ == device && d3d11DeviceContext_ == context) {
        return;
    }
    if (d3d11DeviceContext_) {
        static_cast<ID3D11DeviceContext*>(d3d11DeviceContext_)->Release();
    }
    if (d3d11Device_) {
        static_cast<ID3D11Device*>(d3d11Device_)->Release();
    }
    d3d11Device_ = device;
    d3d11DeviceContext_ = context;
    if (d3d11Device_) {
        static_cast<ID3D11Device*>(d3d11Device_)->AddRef();
    }
    if (d3d11DeviceContext_) {
        static_cast<ID3D11DeviceContext*>(d3d11DeviceContext_)->AddRef();
    }
}

void FFmpegPlayer::selectAudioStream(int streamIndex)
{
    if (requestedAudioStream_.exchange(streamIndex) == streamIndex) {
        return;
    }
    audioSwitchRequested_ = true;
    appendDiagnostic(
        QStringLiteral("track"),
        QStringLiteral("Requested audio stream %1").arg(streamIndex));
}

void FFmpegPlayer::selectSubtitleStream(int streamIndex)
{
    if (requestedSubtitleStream_.exchange(streamIndex) == streamIndex) {
        return;
    }
    subtitleSwitchRequested_ = true;
    appendDiagnostic(
        QStringLiteral("track"),
        QStringLiteral("Requested subtitle stream %1").arg(streamIndex));
}

FFmpegPlayer::State FFmpegPlayer::state() const
{
    return state_.load();
}

qint64 FFmpegPlayer::position() const
{
    return positionMs_.load();
}

qint64 FFmpegPlayer::duration() const
{
    return durationMs_.load();
}

double FFmpegPlayer::playbackRate() const
{
    return requestedRate_.load();
}

int FFmpegPlayer::volume() const
{
    return requestedVolume_.load();
}

bool FFmpegPlayer::isMuted() const
{
    return requestedMuted_.load();
}

bool FFmpegPlayer::hasAudio() const
{
    return hasAudio_.load();
}

bool FFmpegPlayer::hasVideo() const
{
    return hasVideo_.load();
}

int FFmpegPlayer::videoRotation() const
{
    return videoRotation_.load();
}

double FFmpegPlayer::videoFrameRate() const
{
    return videoFrameRate_.load();
}

int FFmpegPlayer::requestedAudioStream() const
{
    return requestedAudioStream_.load();
}

int FFmpegPlayer::requestedSubtitleStream() const
{
    return requestedSubtitleStream_.load();
}

int FFmpegPlayer::activeAudioStream() const
{
    return activeAudioStream_.load();
}

int FFmpegPlayer::activeSubtitleStream() const
{
    return activeSubtitleStream_.load();
}

QVector<FFmpegPlayer::TrackInfo> FFmpegPlayer::audioTracks() const
{
    std::lock_guard<std::mutex> lock(tracksMutex_);
    return audioTracks_;
}

QVector<FFmpegPlayer::TrackInfo> FFmpegPlayer::subtitleTracks() const
{
    std::lock_guard<std::mutex> lock(tracksMutex_);
    return subtitleTracks_;
}

QVector<FFmpegPlayer::ChapterInfo> FFmpegPlayer::chapters() const
{
    std::lock_guard<std::mutex> lock(chaptersMutex_);
    return chapters_;
}

QString FFmpegPlayer::embeddedSubtitleTextAt(qint64 positionMs) const
{
    if (positionMs < 0) {
        return {};
    }
    std::lock_guard<std::mutex> lock(subtitleMutex_);
    if (embeddedCues_.empty()) {
        return {};
    }
    const auto iterator = std::upper_bound(
        embeddedCues_.cbegin(),
        embeddedCues_.cend(),
        positionMs,
        [](qint64 value, const EmbeddedCue& cue) { return value < cue.startMs; });
    if (iterator == embeddedCues_.cbegin()) {
        return {};
    }
    const EmbeddedCue& cue = *(iterator - 1);
    return positionMs <= cue.endMs ? cue.text : QString();
}

QString FFmpegPlayer::currentTarget() const
{
    {
        std::lock_guard<std::mutex> lock(hlsMutex_);
        if (!hlsMasterUrl_.isEmpty()) {
            return hlsMasterUrl_.toString();
        }
    }
    std::lock_guard<std::mutex> lock(targetMutex_);
    return target_;
}

QString FFmpegPlayer::mediaInfo() const
{
    std::lock_guard<std::mutex> lock(infoMutex_);
    return info_;
}

QString FFmpegPlayer::lastError() const
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

FFmpegPlayer::PlaybackStats FFmpegPlayer::playbackStats() const
{
    PlaybackStats stats;
    stats.audioFramesWritten = audioFramesWritten_.load();
    stats.videoFramesPresented = videoFramesPresented_.load();
    stats.videoFramesDropped = videoFramesDropped_.load();
    stats.videoPacketsDropped = videoPacketsDropped_.load();
    stats.lastAvDriftMs = lastAvDriftMs_.load();
    stats.audioPacketsDecoded = audioPacketsDecoded_.load();
    stats.videoPacketsDecoded = videoPacketsDecoded_.load();
    stats.audioFramesDecoded = audioFramesDecoded_.load();
    stats.videoFramesDecoded = videoFramesDecoded_.load();
    stats.seekCommandsHandled = seekCommandsHandled_.load();
    stats.bufferedBytes = bufferedBytes_.load();
    stats.bufferedDurationMs = bufferedDurationMs_.load();
    stats.rebufferCount = rebufferCount_.load();
    stats.rebufferTotalMs = rebufferTotalMs_.load();
    stats.lastRebufferMs = lastRebufferMs_.load();
    stats.reconnectCount = reconnectCount_.load();
    stats.framePoolHits = framePool_.hits();
    stats.framePoolMisses = framePool_.misses();
    stats.mediaOpenSerial = mediaOpenSerial_.load();
    stats.hardwareReadbacks = hardwareReadbacks_.load();
    stats.gpuFramesPresented = gpuFramesPresented_.load();
    if (VideoCanvas* canvas = canvas_.load(std::memory_order_acquire)) {
        stats.gpuFramesRendered = canvas->gpuFramesRendered();
    }
    stats.estimatedBandwidthKbps = estimatedHlsKbps_.load();
    stats.controlPhase = controlPhase_.load();
    stats.seekPending = seekPending_.load();
    stats.hlsVariantIndex = activeHlsVariant_.load();
    {
        std::lock_guard<std::mutex> lock(hlsMutex_);
        stats.hlsVariantCount = hlsVariants_.size();
    }
    stats.hlsAutoMode = hlsAutoMode_.load();
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stats.audioQueueDepth = audioQueue_ ? static_cast<int>(audioQueue_->size()) : 0;
        stats.videoQueueDepth = videoQueue_ ? static_cast<int>(videoQueue_->size()) : 0;
    }
    {
        std::lock_guard<std::mutex> lock(clockMutex_);
        stats.clockSource = clock_ && clock_->isAudioDriven()
            ? QStringLiteral("audio")
            : QStringLiteral("wall");
    }
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats.hardwareDecode = hardwareDecodeBackend_;
        stats.renderBackend = renderBackend_;
    }
    if (stats.renderBackend.isEmpty()) {
        stats.renderBackend = canvas_.load() ? canvas_.load()->renderBackend()
                                             : QStringLiteral("software-cpu");
    }
    return stats;
}

QString FFmpegPlayer::performanceSummary() const
{
    const PlaybackStats stats = playbackStats();
    return QStringLiteral(
        "{\"clock\":\"%1\",\"hardware\":\"%2\",\"audioFrames\":%3,"
        "\"videoFrames\":%4,\"droppedFrames\":%5,\"packetsDropped\":%6,"
        "\"avDriftMs\":%7,\"cacheBytes\":%8,\"cacheDurationMs\":%9,"
        "\"rebuffers\":%10,\"rebufferTotalMs\":%11,\"reconnects\":%12,"
        "\"framePoolHits\":%13,\"framePoolMisses\":%14,\"mediaOpenSerial\":%15,"
        "\"hardwareReadbacks\":%16,\"gpuFrames\":%17,\"gpuRendered\":%18,"
        "\"estimatedKbps\":%19,\"hlsVariant\":%20,\"hlsVariants\":%21,"
        "\"hlsAuto\":%22}")
        .arg(stats.clockSource,
             stats.hardwareDecode)
        .arg(stats.audioFramesWritten)
        .arg(stats.videoFramesPresented)
        .arg(stats.videoFramesDropped)
        .arg(stats.videoPacketsDropped)
        .arg(stats.lastAvDriftMs)
        .arg(stats.bufferedBytes)
        .arg(stats.bufferedDurationMs)
        .arg(stats.rebufferCount)
        .arg(stats.rebufferTotalMs)
        .arg(stats.reconnectCount)
        .arg(stats.framePoolHits)
        .arg(stats.framePoolMisses)
        .arg(stats.mediaOpenSerial)
        .arg(stats.hardwareReadbacks)
        .arg(stats.gpuFramesPresented)
             .arg(stats.gpuFramesRendered)
        .arg(stats.estimatedBandwidthKbps)
        .arg(stats.hlsVariantIndex)
        .arg(stats.hlsVariantCount)
        .arg(stats.hlsAutoMode ? QStringLiteral("true") : QStringLiteral("false"));
}

QString FFmpegPlayer::diagnosticsReport() const
{
    QStringList lines;
    lines.append(QStringLiteral("FFmpegQtPlayer diagnostics"));
    lines.append(QStringLiteral("Generated: %1")
                     .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)));
    lines.append(QStringLiteral("Qt: %1").arg(QString::fromLatin1(qVersion())));
    lines.append(QStringLiteral("FFmpeg: %1")
                     .arg(QString::fromUtf8(av_version_info())));
    lines.append(QStringLiteral("OS: %1 %2")
                     .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion()));
    lines.append(QStringLiteral("Target: %1").arg(currentTarget()));
    lines.append(QStringLiteral("State: %1").arg(stateName(state_.load())));
    lines.append(QStringLiteral("Network: %1")
                     .arg(networkTarget_.load() ? QStringLiteral("true")
                                                : QStringLiteral("false")));
    lines.append(QStringLiteral("Last error: %1").arg(lastError()));
    lines.append(QStringLiteral("Stats: %1").arg(performanceSummary()));
    lines.append(QStringLiteral("Media info:"));
    lines.append(mediaInfo());
    lines.append(QStringLiteral("Event log:"));
    {
        std::lock_guard<std::mutex> lock(diagnosticsMutex_);
        for (const QString& line : diagnostics_) {
            lines.append(line);
        }
    }
    return lines.join(QChar('\n'));
}

bool FFmpegPlayer::exportDiagnostics(const QString& path, QString* error) const
{
    if (path.trimmed().isEmpty()) {
        if (error) {
            *error = tr("Diagnostics path is empty.");
        }
        return false;
    }

    const QFileInfo info(path);
    QSaveFile file(path);
    if (!info.dir().exists() || !file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) {
            *error = tr("Cannot write diagnostics: %1").arg(file.errorString());
        }
        return false;
    }
    const QByteArray data = diagnosticsReport().toUtf8();
    if (file.write(data) != data.size() || !file.commit()) {
        if (error) {
            *error = tr("Cannot commit diagnostics: %1").arg(file.errorString());
        }
        return false;
    }
    return true;
}

void FFmpegPlayer::ffmpegLogCallback(
    void* context, int level, const char* format, va_list arguments)
{
    FFmpegPlayer* sink = logSink_.load(std::memory_order_acquire);
    if (!sink) {
        av_log_default_callback(context, level, format, arguments);
        return;
    }

    char buffer[2048]{};
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    QString message = QString::fromUtf8(buffer).trimmed();
    const bool transportReconnect = message.contains(
        QStringLiteral("reconnect at "), Qt::CaseInsensitive);
    if (level > AV_LOG_WARNING && !transportReconnect) {
        return;
    }
    if (!message.isEmpty()) {
        sink->appendDiagnostic(QStringLiteral("ffmpeg"), message);
    }
    if (transportReconnect) {
        sink->reconnectCount_.fetch_add(1, std::memory_order_relaxed);
        const State current = sink->state_.load(std::memory_order_acquire);
        if (current == State::Buffering || current == State::Playing) {
            sink->setState(State::Reconnecting);
        }
    }
}

void FFmpegPlayer::appendDiagnostic(const QString& category, const QString& message)
{
    QString normalized = message;
    normalized.replace(QChar('\n'), QStringLiteral(" | "));
    normalized.replace(QChar('\r'), QChar(' '));
    const quintptr threadId =
        reinterpret_cast<quintptr>(QThread::currentThreadId());
    const QString line = QStringLiteral("%1 [%2] [%3] [0x%4] %5")
                             .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                                  category,
                                  stateName(state_.load()))
                             .arg(threadId, 0, 16)
                             .arg(normalized);
    std::lock_guard<std::mutex> lock(diagnosticsMutex_);
    diagnostics_.push_back(line);
    while (diagnostics_.size() > 500) {
        diagnostics_.pop_front();
    }
}

QString FFmpegPlayer::stateName(State state)
{
    switch (state) {
    case State::Idle:
        return QStringLiteral("Idle");
    case State::Opening:
        return QStringLiteral("Opening");
    case State::Buffering:
        return QStringLiteral("Buffering");
    case State::Playing:
        return QStringLiteral("Playing");
    case State::Paused:
        return QStringLiteral("Paused");
    case State::Stalled:
        return QStringLiteral("Stalled");
    case State::Reconnecting:
        return QStringLiteral("Reconnecting");
    case State::Stopped:
        return QStringLiteral("Stopped");
    case State::Ended:
        return QStringLiteral("Ended");
    case State::Error:
        return QStringLiteral("Error");
    }
    return QStringLiteral("Unknown");
}

void FFmpegPlayer::setState(State state)
{
    const State previous = state_.exchange(state);
    if (previous != state) {
        appendDiagnostic(
            QStringLiteral("state"),
            QStringLiteral("%1 -> %2").arg(stateName(previous), stateName(state)));
    }
    emit stateChanged(state);
}

void FFmpegPlayer::stopThreads()
{
    abortPlayback();
    opening_ = false;
    seekPending_ = false;
    previewPending_ = false;
    videoStepCount_ = 0;
    setClockPaused(true);

    if (controlThread_.joinable()) {
        controlThread_.join();
    }
    closeQueues();
    audioThread_ = std::thread();
    videoThread_ = std::thread();
}

void FFmpegPlayer::abortPlayback()
{
    abort_ = true;
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (audioQueue_) {
        audioQueue_->abort();
    }
    if (videoQueue_) {
        videoQueue_->abort();
    }
}

int FFmpegPlayer::interruptCallback(void* opaque)
{
    auto* self = static_cast<FFmpegPlayer*>(opaque);
    if (!self) {
        return 0;
    }
    if (self->abort_.load(std::memory_order_relaxed)) {
        return 1;
    }
    if (self->opening_.load(std::memory_order_relaxed)
        && monotonicMilliseconds() > self->openDeadlineMs_.load(std::memory_order_relaxed)) {
        return 1;
    }
    return 0;
}

AVPixelFormat FFmpegPlayer::getHardwareFormat(
    AVCodecContext* codecContext, const AVPixelFormat* pixelFormats)
{
    const auto* self = static_cast<const FFmpegPlayer*>(codecContext
        ? codecContext->opaque
        : nullptr);
    if (!self) {
        return AV_PIX_FMT_NONE;
    }
    const int desired = self->hardwarePixelFormat_.load(std::memory_order_acquire);
    for (const AVPixelFormat* format = pixelFormats; format && *format != AV_PIX_FMT_NONE;
         ++format) {
        if (*format == desired) {
            return *format;
        }
    }
    return AV_PIX_FMT_NONE;
}

bool FFmpegPlayer::openVideoDecoder(AVFormatContext* format, int streamIndex)
{
    if (!format || streamIndex < 0 || streamIndex >= static_cast<int>(format->nb_streams)) {
        return false;
    }

    AVStream* stream = format->streams[streamIndex];
    AVCodecContext* codec = stream->codec;
    codec->opaque = this;
    av_buffer_unref(&hwDeviceContext_);
    hardwarePixelFormat_ = AV_PIX_FMT_NONE;
    const AVCodec* decoder = avcodec_find_decoder(codec->codec_id);
    if (!decoder) {
        return false;
    }

    bool hardwareOpened = false;
    bool zeroCopyConfigured = false;
    if (hardwareDecodeEnabled_.load()) {
        AVBufferRef* deviceContext = nullptr;
        VideoCanvas* canvas = canvas_.load(std::memory_order_acquire);
        void* sharedDevice = nullptr;
        void* sharedContext = nullptr;
        if (canvas) {
            canvas->prepareGpuRenderer();
            canvas->waitForD3d11Device(3000);
            if (canvas->gpuRenderReady()) {
                sharedDevice = canvas->d3d11Device();
                sharedContext = canvas->d3d11DeviceContext();
            }
        }

        int deviceResult = AVERROR_UNKNOWN;
        if (sharedDevice && sharedContext) {
            deviceContext = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (deviceContext) {
            auto* device = reinterpret_cast<AVHWDeviceContext*>(deviceContext->data);
                auto* d3d11 = static_cast<AVD3D11VADeviceContext*>(device->hwctx);
                d3d11->device = static_cast<ID3D11Device*>(sharedDevice);
                d3d11->device->AddRef();
                d3d11->device_context = static_cast<ID3D11DeviceContext*>(sharedContext);
                d3d11->device_context->AddRef();
                d3d11->lock = &D3d11ContextSync::lock;
                d3d11->unlock = &D3d11ContextSync::unlock;
                d3d11->lock_ctx = nullptr;
                deviceResult = av_hwdevice_ctx_init(deviceContext);
            }
        } else {
            deviceResult = av_hwdevice_ctx_create(
                &deviceContext, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        }
        if (deviceResult >= 0 && deviceContext) {
            hwDeviceContext_ = deviceContext;
            hardwarePixelFormat_ = AV_PIX_FMT_D3D11;
            codec->get_format = &FFmpegPlayer::getHardwareFormat;
            codec->hw_device_ctx = av_buffer_ref(deviceContext);
            hardwareOpened = avcodec_open2(codec, decoder, nullptr) >= 0;
            zeroCopyConfigured = hardwareOpened && sharedDevice && sharedContext;
        }
        if (!hardwareOpened) {
            avcodec_close(codec);
            codec->get_format = nullptr;
            av_buffer_unref(&codec->hw_device_ctx);
            av_buffer_unref(&hwDeviceContext_);
            hardwarePixelFormat_ = AV_PIX_FMT_NONE;
            zeroCopyConfigured = false;
        }
    }

    if (!hardwareOpened && avcodec_open2(codec, decoder, nullptr) < 0) {
        av_buffer_unref(&hwDeviceContext_);
        hardwarePixelFormat_ = AV_PIX_FMT_NONE;
        hardwareZeroCopyEnabled_ = false;
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            renderBackend_ = QStringLiteral("software-cpu");
        }
        return false;
    }

    videoStream_.index = streamIndex;
    videoStream_.timeBase = stream->time_base;
    videoStream_.startTimestamp = stream->start_time;
    videoCodecContext_ = codec;
    videoWidth_ = codec->width;
    videoHeight_ = codec->height;
    videoFrameRate_ = stream->avg_frame_rate.num > 0
        ? av_q2d(stream->avg_frame_rate)
        : 0.0;
    videoRotation_ = displayRotation(stream);
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        hardwareDecodeBackend_ = hardwareOpened
            ? (zeroCopyConfigured
                   ? QStringLiteral("d3d11va-zero-copy")
                   : QStringLiteral("d3d11va-gpu-copy"))
            : QStringLiteral("software");
        renderBackend_ = zeroCopyConfigured
            ? QStringLiteral("d3d11-rhi-zero-copy")
            : QStringLiteral("software-cpu");
    }
    hardwareZeroCopyEnabled_ = zeroCopyConfigured;
    return true;
}

bool FFmpegPlayer::switchVideoDecoderToSoftware()
{
    if (!videoCodecContext_) {
        return false;
    }
    AVCodecContext* codec = videoCodecContext_;
    const AVCodec* decoder = avcodec_find_decoder(codec->codec_id);
    if (!decoder) {
        return false;
    }
    avcodec_close(codec);
    codec->get_format = nullptr;
    av_buffer_unref(&codec->hw_device_ctx);
    av_buffer_unref(&hwDeviceContext_);
    hardwarePixelFormat_ = AV_PIX_FMT_NONE;
    hardwareZeroCopyEnabled_ = false;
    const int result = avcodec_open2(codec, decoder, nullptr);
    if (result < 0) {
        const QString message = tr("Cannot fall back to software video decoding: %1")
                                    .arg(ffmpegError(result));
        setLastError(message);
        emit errorOccurred(message);
        return false;
    }
    avcodec_flush_buffers(codec);
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        hardwareDecodeBackend_ = QStringLiteral("software");
        renderBackend_ = QStringLiteral("software-cpu");
    }
    return true;
}

void FFmpegPlayer::clearEmbeddedCues()
{
    std::lock_guard<std::mutex> lock(subtitleMutex_);
    embeddedCues_.clear();
}

void FFmpegPlayer::reloadAtCurrentPosition()
{
    const State current = state_.load();
    if (current == State::Idle || current == State::Stopped || current == State::Opening) {
        return;
    }
    const QString target = currentTarget();
    if (target.isEmpty()) {
        return;
    }

    qint64 position = 0;
    bool startPaused = true;
    if (current == State::Playing) {
        position = positionMs_.load();
        startPaused = false;
    } else if (current == State::Paused) {
        position = positionMs_.load();
        startPaused = true;
    }
    open(target, position, startPaused);
}

int FFmpegPlayer::resolveAudioStreamIndex(int requestedIndex) const
{
    QVector<TrackInfo> tracks;
    {
        std::lock_guard<std::mutex> lock(tracksMutex_);
        tracks = audioTracks_;
    }
    if (requestedIndex >= 0) {
        const auto found = std::find_if(
            tracks.cbegin(), tracks.cend(), [requestedIndex](const TrackInfo& track) {
                return track.index == requestedIndex;
            });
        if (found != tracks.cend()) {
            return found->index;
        }
    }
    if (requestedIndex == -2) {
        return -1;
    }
    if (!formatContext_) {
        return -1;
    }
    return av_find_best_stream(
        formatContext_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
}

int FFmpegPlayer::resolveSubtitleStreamIndex(int requestedIndex) const
{
    QVector<TrackInfo> tracks;
    {
        std::lock_guard<std::mutex> lock(tracksMutex_);
        tracks = subtitleTracks_;
    }
    if (requestedIndex >= 0) {
        const auto found = std::find_if(
            tracks.cbegin(), tracks.cend(), [requestedIndex](const TrackInfo& track) {
                return track.index == requestedIndex;
            });
        if (found != tracks.cend()) {
            return found->index;
        }
    }
    if (requestedIndex == -2 || tracks.isEmpty()) {
        return -1;
    }
    return tracks.first().index;
}

bool FFmpegPlayer::openAudioDecoder(int streamIndex)
{
    if (!formatContext_ || streamIndex < 0
        || streamIndex >= static_cast<int>(formatContext_->nb_streams)) {
        return false;
    }
    AVCodecContext* codec = formatContext_->streams[streamIndex]->codec;
    const AVCodec* decoder = avcodec_find_decoder(codec->codec_id);
    if (!decoder || avcodec_open2(codec, decoder, nullptr) < 0) {
        return false;
    }

    audioStream_.index = streamIndex;
    audioStream_.timeBase = formatContext_->streams[streamIndex]->time_base;
    audioStream_.startTimestamp = formatContext_->streams[streamIndex]->start_time;
    audioCodecContext_ = codec;
    audioSampleRate_ = codec->sample_rate;
    audioChannels_ = codec->channels;
    activeAudioStream_ = streamIndex;
    hasAudio_ = true;
    return true;
}

bool FFmpegPlayer::openSubtitleDecoder(int streamIndex)
{
    if (!formatContext_ || streamIndex < 0
        || streamIndex >= static_cast<int>(formatContext_->nb_streams)) {
        return false;
    }
    AVCodecContext* codec = formatContext_->streams[streamIndex]->codec;
    const AVCodec* decoder = avcodec_find_decoder(codec->codec_id);
    if (!decoder || avcodec_open2(codec, decoder, nullptr) < 0) {
        return false;
    }

    subtitleStream_.index = streamIndex;
    subtitleStream_.timeBase = formatContext_->streams[streamIndex]->time_base;
    subtitleStream_.startTimestamp = formatContext_->streams[streamIndex]->start_time;
    subtitleCodecContext_ = codec;
    activeSubtitleStream_ = streamIndex;
    return true;
}

void FFmpegPlayer::closeAudioDecoder()
{
    std::lock_guard<std::mutex> lock(codecMutex_);
    if (audioCodecContext_) {
        avcodec_close(audioCodecContext_);
    }
    audioCodecContext_ = nullptr;
    audioStream_ = {};
    activeAudioStream_ = -1;
}

void FFmpegPlayer::closeSubtitleDecoder()
{
    std::lock_guard<std::mutex> lock(codecMutex_);
    if (subtitleCodecContext_) {
        avcodec_close(subtitleCodecContext_);
    }
    subtitleCodecContext_ = nullptr;
    subtitleStream_ = {};
    activeSubtitleStream_ = -1;
}

bool FFmpegPlayer::switchAudioStreamNow()
{
    const int targetIndex = resolveAudioStreamIndex(requestedAudioStream_.load());
    if (targetIndex < 0) {
        appendDiagnostic(QStringLiteral("track"), QStringLiteral("No audio stream selected"));
        return false;
    }
    if (targetIndex == activeAudioStream_.load() && audioCodecContext_) {
        return true;
    }

    const unsigned oldGeneration = audioGeneration_.load(std::memory_order_acquire);
    audioGeneration_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (audioQueue_) {
            audioQueue_->abort();
        }
    }
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
    closeAudioDecoder();

    bool opened = false;
    {
        std::lock_guard<std::mutex> lock(codecMutex_);
        opened = openAudioDecoder(targetIndex);
    }
    if (!opened) {
        hasAudio_ = false;
        {
            std::lock_guard<std::mutex> lock(clockMutex_);
            audioClock_ = nullptr;
            clock_ = std::make_unique<WallClock>(positionMs_.load());
            clock_->setRate(requestedRate_.load());
            clock_->setPaused(paused_.load());
        }
        appendDiagnostic(
            QStringLiteral("track"),
            QStringLiteral("Audio switch failed: stream %1").arg(targetIndex));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        audioQueue_ = std::make_unique<PacketQueue>(48, 2 * 1024 * 1024);
        audioQueue_->setPaused(state_.load() == State::Buffering);
    }
    audioRebaseSerial_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(clockMutex_);
        if (!audioClock_) {
            auto audioClock = std::make_unique<AudioClock>(positionMs_.load());
            audioClock_ = audioClock.get();
            clock_ = std::move(audioClock);
        }
        if (clock_) {
            clock_->reset(positionMs_.load());
            clock_->setRate(requestedRate_.load());
            clock_->setPaused(paused_.load() || state_.load() == State::Buffering);
        }
    }
    audioThread_ = std::thread(&FFmpegPlayer::audioLoop, this);
    appendDiagnostic(
        QStringLiteral("track"),
        QStringLiteral("Audio stream switched to %1 (generation %2)")
            .arg(targetIndex)
            .arg(oldGeneration + 1));
    return true;
}

bool FFmpegPlayer::switchSubtitleStreamNow()
{
    const int targetIndex = resolveSubtitleStreamIndex(requestedSubtitleStream_.load());
    const int currentIndex = activeSubtitleStream_.load();
    if (targetIndex == currentIndex) {
        return true;
    }

    closeSubtitleDecoder();
    clearEmbeddedCues();
    if (targetIndex < 0) {
        appendDiagnostic(QStringLiteral("track"), QStringLiteral("Subtitles disabled"));
        return true;
    }
    bool opened = false;
    {
        std::lock_guard<std::mutex> lock(codecMutex_);
        opened = openSubtitleDecoder(targetIndex);
    }
    appendDiagnostic(
        QStringLiteral("track"),
        opened
            ? QStringLiteral("Subtitle stream switched to %1").arg(targetIndex)
            : QStringLiteral("Subtitle switch failed: stream %1").arg(targetIndex));
    return opened;
}

void FFmpegPlayer::processTrackSwitchRequests()
{
    if (subtitleSwitchRequested_.exchange(false)) {
        switchSubtitleStreamNow();
    }
    if (audioSwitchRequested_.exchange(false)) {
        switchAudioStreamNow();
    }
}

void FFmpegPlayer::decodeSubtitlePacket(AVPacket* packet)
{
    if (!packet || !subtitleCodecContext_) {
        return;
    }

    AVSubtitle subtitle{};
    int got = 0;
    int result = 0;
    {
        std::lock_guard<std::mutex> lock(codecMutex_);
        result = avcodec_decode_subtitle2(subtitleCodecContext_, &subtitle, &got, packet);
    }
    if (result < 0) {
        if (got) {
            avsubtitle_free(&subtitle);
        }
        return;
    }
    if (!got) {
        avsubtitle_free(&subtitle);
        return;
    }

    qint64 baseMs = packetTimeMs(packet, subtitleStream_.index);
    if (baseMs < 0) {
        baseMs = subtitle.pts != AV_NOPTS_VALUE ? subtitle.pts / (AV_TIME_BASE / 1000) : 0;
    }
    const qint64 startMs = baseMs + static_cast<qint64>(subtitle.start_display_time);
    qint64 endMs = baseMs + static_cast<qint64>(subtitle.end_display_time);
    if (endMs <= startMs) {
        endMs = startMs + 2000;
    }

    QStringList pieces;
    for (unsigned index = 0; index < subtitle.num_rects; ++index) {
        const AVSubtitleRect* rect = subtitle.rects[index];
        if (!rect) {
            continue;
        }
        QString text;
        if (rect->type == SUBTITLE_ASS && rect->ass) {
            text = cleanSubtitleMarkup(assDialogueText(QString::fromUtf8(rect->ass)));
        } else if (rect->type == SUBTITLE_TEXT && rect->text) {
            text = cleanSubtitleMarkup(QString::fromUtf8(rect->text));
        }
        if (!text.isEmpty()) {
            pieces.append(text);
        }
    }
    avsubtitle_free(&subtitle);

    const QString text = pieces.join(QChar('\n'));
    if (text.isEmpty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(subtitleMutex_);
    const auto existing = std::find_if(
        embeddedCues_.cbegin(),
        embeddedCues_.cend(),
        [startMs, &text](const EmbeddedCue& item) {
            return item.startMs == startMs && item.text == text;
        });
    if (existing != embeddedCues_.cend()) {
        return;
    }
    const EmbeddedCue cue{startMs, endMs, text};
    const auto position = std::upper_bound(
        embeddedCues_.begin(),
        embeddedCues_.end(),
        startMs,
        [](qint64 value, const EmbeddedCue& item) { return value < item.startMs; });
    embeddedCues_.insert(position, cue);
}

void FFmpegPlayer::run(const QString& target)
{
    const QByteArray encodedTarget = target.toUtf8();
    AVFormatContext* format = nullptr;
    int result = AVERROR_UNKNOWN;
    int openAttempt = 0;
    const bool network = networkTarget_.load();
    while (!abort_) {
        format = avformat_alloc_context();
        if (!format) {
            result = AVERROR(ENOMEM);
            break;
        }
        format->interrupt_callback.callback = &FFmpegPlayer::interruptCallback;
        format->interrupt_callback.opaque = this;

        AVDictionary* options = nullptr;
        StreamingPolicy::configureOpenOptions(&options, target);
        result = avformat_open_input(
            &format, encodedTarget.constData(), nullptr, &options);
        av_dict_free(&options);
        if (result >= 0) {
            break;
        }
        if (format) {
            avformat_close_input(&format);
        }
        if (!network || openAttempt >= StreamingPolicy::maxReconnectAttempts) {
            break;
        }

        const int delayMs = StreamingPolicy::reconnectDelayMs(openAttempt++);
        reconnectCount_.fetch_add(1, std::memory_order_relaxed);
        setState(State::Reconnecting);
        appendDiagnostic(
            QStringLiteral("network"),
            QStringLiteral("Open retry %1 in %2ms: %3")
                .arg(openAttempt)
                .arg(delayMs)
                .arg(ffmpegError(result)));
        const qint64 retryDeadline = monotonicMilliseconds() + delayMs;
        while (!abort_ && monotonicMilliseconds() < retryDeadline) {
            QThread::msleep(20);
        }
    }
    opening_ = false;
    if (result < 0) {
        if (abort_) {
            return;
        }
        const QString message = tr("Cannot open media: %1").arg(ffmpegError(result));
        if (requestHlsFallback(message)) {
            schedulePendingHlsFallback();
            return;
        }
        setLastError(message);
        appendDiagnostic(QStringLiteral("network"), message);
        emit errorOccurred(message);
        setState(State::Error);
        return;
    }

    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        if (abort_) {
            avformat_close_input(&format);
            return;
        }
        const QString message = tr("Cannot read stream information: %1").arg(ffmpegError(result));
        if (requestHlsFallback(message)) {
            avformat_close_input(&format);
            schedulePendingHlsFallback();
            return;
        }
        setLastError(message);
        avformat_close_input(&format);
        emit errorOccurred(message);
        setState(State::Error);
        return;
    }

    audioStream_ = {};
    videoStream_ = {};
    subtitleStream_ = {};

    QVector<TrackInfo> audioTracks;
    QVector<TrackInfo> subtitleTracks;
    for (unsigned index = 0; index < format->nb_streams; ++index) {
        const AVStream* stream = format->streams[index];
        const AVCodecContext* codec = stream->codec;
        if (codec->codec_type == AVMEDIA_TYPE_AUDIO
            && avcodec_find_decoder(codec->codec_id)) {
            audioTracks.append({static_cast<int>(index), streamDescription(format, static_cast<int>(index))});
        } else if (codec->codec_type == AVMEDIA_TYPE_SUBTITLE
                   && isTextSubtitleCodec(codec->codec_id)
                   && avcodec_find_decoder(codec->codec_id)) {
            subtitleTracks.append({static_cast<int>(index), streamDescription(format, static_cast<int>(index))});
        }
    }
    {
        std::lock_guard<std::mutex> lock(tracksMutex_);
        audioTracks_ = audioTracks;
        subtitleTracks_ = subtitleTracks;
    }

    QVector<ChapterInfo> chapters;
    chapters.reserve(static_cast<int>(format->nb_chapters));
    for (unsigned index = 0; index < format->nb_chapters; ++index) {
        const AVChapter* chapter = format->chapters[index];
        if (!chapter) {
            continue;
        }
        ChapterInfo info;
        info.startMs = qMax<qint64>(0, rationalToMilliseconds(chapter->start, chapter->time_base));
        info.endMs = qMax(info.startMs, rationalToMilliseconds(chapter->end, chapter->time_base));
        const AVDictionaryEntry* title = av_dict_get(chapter->metadata, "title", nullptr, 0);
        if (title && title->value && *title->value) {
            info.title = QString::fromUtf8(title->value).trimmed();
        }
        chapters.append(info);
    }
    std::sort(chapters.begin(), chapters.end(), [](const ChapterInfo& left, const ChapterInfo& right) {
        return left.startMs < right.startMs;
    });
    {
        std::lock_guard<std::mutex> lock(chaptersMutex_);
        chapters_ = chapters;
    }

    {
        std::lock_guard<std::mutex> lock(codecMutex_);
        formatContext_ = format;
    }

    const int videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    hasVideo_ = videoIndex >= 0 && openVideoDecoder(format, videoIndex);

    const int audioIndex = resolveAudioStreamIndex(requestedAudioStream_.load());
    if (audioIndex >= 0) {
        hasAudio_ = openAudioDecoder(audioIndex);
    }

    const int subtitleIndex = resolveSubtitleStreamIndex(requestedSubtitleStream_.load());
    if (subtitleIndex >= 0) {
        openSubtitleDecoder(subtitleIndex);
    }

    if (!hasAudio_ && !hasVideo_) {
        const QString message = tr("The media contains no playable audio or video stream.");
        setLastError(message);
        if (videoCodecContext_) {
            av_buffer_unref(&videoCodecContext_->hw_device_ctx);
            avcodec_close(videoCodecContext_);
        }
        if (audioCodecContext_) {
            avcodec_close(audioCodecContext_);
        }
        if (subtitleCodecContext_) {
            avcodec_close(subtitleCodecContext_);
        }
        audioCodecContext_ = nullptr;
        videoCodecContext_ = nullptr;
        av_buffer_unref(&hwDeviceContext_);
        subtitleCodecContext_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(codecMutex_);
            formatContext_ = nullptr;
        }
        avformat_close_input(&format);
        emit errorOccurred(message);
        setState(State::Error);
        return;
    }

    qint64 duration = format->duration != AV_NOPTS_VALUE
        ? format->duration * 1000 / AV_TIME_BASE
        : 0;
    if (duration <= 0) {
        for (unsigned index = 0; index < format->nb_streams; ++index) {
            AVStream* stream = format->streams[index];
            if (stream->duration != AV_NOPTS_VALUE) {
                duration = qMax(duration, rationalToMilliseconds(stream->duration, stream->time_base));
            }
        }
    }
    durationMs_ = qMax<qint64>(0, duration);

    qint64 startMs = qMax<qint64>(0, startPositionMs_.load());
    if (durationMs_ > 0) {
        startMs = qMin(startMs, durationMs_.load());
    }
    if (startMs > 0) {
        std::lock_guard<std::mutex> lock(codecMutex_);
        const qint64 startTimestamp = startMs * AV_TIME_BASE / 1000;
        if (av_seek_frame(format, -1, startTimestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
            if (audioCodecContext_) {
                avcodec_flush_buffers(audioCodecContext_);
            }
            if (videoCodecContext_) {
                avcodec_flush_buffers(videoCodecContext_);
            }
            if (subtitleCodecContext_) {
                avcodec_flush_buffers(subtitleCodecContext_);
            }
        } else {
            startMs = 0;
        }
    }
    positionMs_ = startMs;
    dropUntilMs_ = startMs;

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        audioQueue_ = hasAudio_
            ? std::make_unique<PacketQueue>(48, 2 * 1024 * 1024)
            : nullptr;
        videoQueue_ = hasVideo_
            ? std::make_unique<PacketQueue>(24, 8 * 1024 * 1024)
            : nullptr;
    }

    QStringList details;
    details.append(tr("Container: %1").arg(format->iformat && format->iformat->name
            ? QString::fromUtf8(format->iformat->name)
            : tr("unknown")));
    if (format->bit_rate > 0) {
        details.append(tr("Bitrate: %1 kb/s").arg(format->bit_rate / 1000));
    }
    details.append(tr("Streams: %1").arg(format->nb_streams));
    if (!chapters.isEmpty()) {
        details.append(tr("Chapters: %1").arg(chapters.size()));
    }
    if (hasVideo_) {
        details.append(tr("Video: %1  %2 x %3  %4 fps%5")
                           .arg(QString::fromUtf8(avcodec_get_name(videoCodecContext_->codec_id)))
                           .arg(videoWidth_.load())
                           .arg(videoHeight_.load())
                           .arg(videoFrameRate_.load(), 0, 'f', 2)
                           .arg(videoRotation_.load() != 0
                                    ? tr("  rotated %1°").arg(videoRotation_.load())
                                    : QString()));
        details.append(tr("Video decode: %1")
                           .arg(playbackStats().hardwareDecode));
        if (videoStream_.index >= 0) {
            const AVCodecParameters* parameters = format->streams[videoStream_.index]->codecpar;
            if (parameters->format != AV_PIX_FMT_NONE) {
                const char* pixelFormat = av_get_pix_fmt_name(
                    static_cast<AVPixelFormat>(parameters->format));
                if (pixelFormat) {
                    details.append(tr("Pixel format: %1").arg(QString::fromUtf8(pixelFormat)));
                }
            }
            if (parameters->bit_rate > 0) {
                details.append(tr("Video bitrate: %1 kb/s").arg(parameters->bit_rate / 1000));
            }
        }
    }
    if (hasAudio_) {
        details.append(tr("Audio: %1  %2 Hz  %3 channel(s)")
                           .arg(QString::fromUtf8(avcodec_get_name(audioCodecContext_->codec_id)))
                           .arg(audioSampleRate_.load())
                           .arg(audioChannels_.load()));
        if (audioStream_.index >= 0) {
            const AVCodecParameters* parameters = format->streams[audioStream_.index]->codecpar;
            if (parameters->bit_rate > 0) {
                details.append(tr("Audio bitrate: %1 kb/s").arg(parameters->bit_rate / 1000));
            }
        }
    }
    if (activeSubtitleStream_.load() >= 0) {
        details.append(tr("Embedded subtitle: %1")
                           .arg(QString::fromUtf8(avcodec_get_name(
                               subtitleCodecContext_->codec_id))));
    }
    details.append(tr("Duration: %1").arg(durationMs_.load()));
    if (format->metadata) {
        const AVDictionaryEntry* title = av_dict_get(format->metadata, "title", nullptr, 0);
        if (title && title->value && *title->value) {
            details.append(tr("Title: %1").arg(QString::fromUtf8(title->value)));
        }
        const AVDictionaryEntry* artist = av_dict_get(format->metadata, "artist", nullptr, 0);
        if (artist && artist->value && *artist->value) {
            details.append(tr("Artist: %1").arg(QString::fromUtf8(artist->value)));
        }
    }
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        info_ = details.join(QChar('\n'));
    }

    emit mediaInfoChanged(mediaInfo());
    emit mediaOpened(durationMs_.load(), hasAudio_, hasVideo_);

    {
        std::lock_guard<std::mutex> lock(clockMutex_);
        if (hasAudio_) {
            auto audioClock = std::make_unique<AudioClock>(startMs);
            audioClock_ = audioClock.get();
            clock_ = std::move(audioClock);
        } else {
            audioClock_ = nullptr;
            clock_ = std::make_unique<WallClock>(startMs);
        }
        clock_->setRate(requestedRate_.load());
        clock_->setPaused(startPaused_.load());
    }
    paused_ = startPaused_.load();

    setState(State::Buffering);
    appendDiagnostic(
        QStringLiteral("buffer"),
        networkTarget_.load()
            ? QStringLiteral("Waiting for initial network buffer")
            : QStringLiteral("Preparing local media"));
    if (primeQueues()) {
        if (hasVideo_) {
            videoThread_ = std::thread(&FFmpegPlayer::videoLoop, this);
        }
        if (hasAudio_) {
            audioThread_ = std::thread(&FFmpegPlayer::audioLoop, this);
        }

        if (paused_.load()) {
            previewPending_ = hasVideo_.load();
            setState(State::Paused);
        } else {
            setState(State::Playing);
        }
        readPackets();
    } else if (!abort_) {
        const QString message = tr("Cannot buffer media: %1")
                                    .arg(lastError().isEmpty()
                                             ? tr("network read failed")
                                             : lastError());
        setLastError(message);
        appendDiagnostic(QStringLiteral("buffer"), message);
        emit errorOccurred(message);
        setState(State::Error);
    }

    if (audioQueue_) {
        AVPacket* drainPacket = av_packet_alloc();
        if (drainPacket) {
            audioQueue_->push(drainPacket);
        }
        audioQueue_->finish();
    }
    if (videoQueue_) {
        AVPacket* drainPacket = av_packet_alloc();
        if (drainPacket) {
            videoQueue_->push(drainPacket);
        }
        videoQueue_->finish();
    }
    if (audioThread_.joinable()) {
        controlPhase_ = 8;
        audioThread_.join();
    }
    if (videoThread_.joinable()) {
        controlPhase_ = 9;
        videoThread_.join();
    }
    controlPhase_ = 10;

    {
        controlPhase_ = 16;
        std::lock_guard<std::mutex> lock(codecMutex_);
        if (videoCodecContext_) {
            av_buffer_unref(&videoCodecContext_->hw_device_ctx);
            avcodec_close(videoCodecContext_);
            videoCodecContext_ = nullptr;
        }
        av_buffer_unref(&hwDeviceContext_);
        if (audioCodecContext_) {
            avcodec_close(audioCodecContext_);
            audioCodecContext_ = nullptr;
        }
        if (subtitleCodecContext_) {
            avcodec_close(subtitleCodecContext_);
            subtitleCodecContext_ = nullptr;
        }
        avformat_close_input(&formatContext_);
    }
    formatContext_ = nullptr;
    closeQueues();
    activeAudioStream_ = -1;
    activeSubtitleStream_ = -1;
    clearEmbeddedCues();

    if (!abort_ && hlsFallbackPending_.load(std::memory_order_acquire)) {
        schedulePendingHlsFallback();
    } else if (!abort_ && state_.load() != State::Error) {
        if (durationMs_ > 0) {
            positionMs_ = durationMs_.load();
        }
        setState(State::Ended);
        emit playbackFinished();
    }
}

bool FFmpegPlayer::primeQueues()
{
    if (!networkTarget_.load()) {
        return true;
    }

    const qint64 deadline = monotonicMilliseconds()
        + StreamingPolicy::initialBufferTimeoutMs;
    bool reachedEnd = false;
    while (!abort_ && monotonicMilliseconds() < deadline) {
        if (buffersReady(true)) {
            return true;
        }

        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            return false;
        }
        const int readResult = readFrameWithRecovery(packet);
        if (readResult == AVERROR_EOF) {
            av_packet_free(&packet);
            reachedEnd = true;
            break;
        }
        if (readResult == AVERROR(EAGAIN)) {
            av_packet_free(&packet);
            QThread::msleep(5);
            continue;
        }
        if (readResult < 0) {
            av_packet_free(&packet);
            if (!abort_) {
                setLastError(tr("Network read failed: %1").arg(ffmpegError(readResult)));
            }
            return false;
        }

        bool queued = false;
        if (packet->stream_index == subtitleStream_.index
            && subtitleCodecContext_) {
            decodeSubtitlePacket(packet);
            av_packet_free(&packet);
        } else if (packet->stream_index == audioStream_.index && hasAudio_.load()) {
            PacketQueue* queue = nullptr;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                queue = audioQueue_.get();
            }
            if (queue) {
                queued = queue->pushTimed(
                    packet,
                    packetDurationMs(packet, audioStream_.index),
                    50);
                if (!queued) {
                    av_packet_free(&packet);
                }
            } else {
                av_packet_free(&packet);
            }
        } else if (packet->stream_index == videoStream_.index && hasVideo_.load()) {
            PacketQueue* queue = nullptr;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                queue = videoQueue_.get();
            }
            if (queue) {
                queued = queue->pushTimed(
                    packet,
                    packetDurationMs(packet, videoStream_.index),
                    50);
                if (!queued) {
                    av_packet_free(&packet);
                }
            } else {
                av_packet_free(&packet);
            }
        } else {
            av_packet_free(&packet);
        }

        if (abort_) {
            return false;
        }

        const StreamBufferSnapshot audio = audioBufferSnapshot();
        const StreamBufferSnapshot video = videoBufferSnapshot();
        bufferedBytes_ = audio.bytes + video.bytes;
        if (audio.present && video.present) {
            bufferedDurationMs_ = qMin(audio.durationMs, video.durationMs);
        } else {
            bufferedDurationMs_ = audio.present
                ? audio.durationMs
                : video.durationMs;
        }
        appendDiagnostic(
            QStringLiteral("buffer"),
            QStringLiteral("Initial level audio=%1ms/%2B video=%3ms/%4B queued=%5")
                .arg(audio.durationMs)
                .arg(audio.bytes)
                .arg(video.durationMs)
                .arg(video.bytes)
                .arg(queued ? QStringLiteral("true") : QStringLiteral("false")));
        if (buffersReady(true)) {
            return true;
        }
    }

    const StreamBufferSnapshot audio = audioBufferSnapshot();
    const StreamBufferSnapshot video = videoBufferSnapshot();
    return !abort_
        && (buffersReady(true)
            || (reachedEnd && (audio.bytes > 0 || video.bytes > 0)));
}

int FFmpegPlayer::readFrameWithRecovery(AVPacket* packet)
{
    if (!packet) {
        return AVERROR(EINVAL);
    }

    int recoveryAttempt = 0;
    qint64 eagainSinceMs = 0;
    const State stateBeforeFailure = state_.load();
    while (!abort_) {
        int result = 0;
        {
            std::lock_guard<std::mutex> lock(codecMutex_);
            if (!formatContext_) {
                return AVERROR_EXIT;
            }
            result = av_read_frame(formatContext_, packet);
        }
        if (result >= 0 || result == AVERROR_EOF || !networkTarget_.load()) {
            if (result >= 0
                && (state_.load() == State::Stalled
                    || state_.load() == State::Reconnecting)) {
                setLastError({});
                setState(stateBeforeFailure == State::Buffering
                             ? State::Buffering
                             : paused_.load() ? State::Paused : State::Playing);
            }
            return result;
        }

        const qint64 now = monotonicMilliseconds();
        if (result == AVERROR(EAGAIN)) {
            if (eagainSinceMs == 0) {
                eagainSinceMs = now;
            }
            if (now - eagainSinceMs < 250) {
                QThread::msleep(10);
                continue;
            }
        } else {
            eagainSinceMs = 0;
        }

        if (recoveryAttempt >= StreamingPolicy::maxReconnectAttempts) {
            if (!abort_) {
                setLastError(tr("Network read stopped: %1").arg(ffmpegError(result)));
            }
            return result;
        }

        const int delayMs = StreamingPolicy::reconnectDelayMs(recoveryAttempt);
        ++recoveryAttempt;
        reconnectCount_.fetch_add(1, std::memory_order_relaxed);
        setState(recoveryAttempt == 1 ? State::Stalled : State::Reconnecting);
        appendDiagnostic(
            QStringLiteral("network"),
            QStringLiteral("Read retry %1/%2 in %3ms: %4")
                .arg(recoveryAttempt)
                .arg(StreamingPolicy::maxReconnectAttempts)
                .arg(delayMs)
                .arg(ffmpegError(result)));

        const qint64 retryDeadline = monotonicMilliseconds() + delayMs;
        while (!abort_ && monotonicMilliseconds() < retryDeadline) {
            QThread::msleep(10);
        }
        if (abort_) {
            return AVERROR_EXIT;
        }

        {
            std::lock_guard<std::mutex> lock(codecMutex_);
            if (formatContext_) {
                avformat_flush(formatContext_);
            }
        }
        setState(State::Reconnecting);
    }
    return AVERROR_EXIT;
}

void FFmpegPlayer::enterBuffering(const QString& reason)
{
    bufferingRequested_ = false;
    if (abort_ || state_.load() == State::Buffering) {
        return;
    }
    bufferingStartedMs_ = monotonicMilliseconds();
    rebufferCount_.fetch_add(1, std::memory_order_relaxed);
    setQueuePaused(true);
    setClockPaused(true);
    setState(State::Buffering);
    appendDiagnostic(QStringLiteral("buffer"), reason);
}

void FFmpegPlayer::leaveBuffering()
{
    if (state_.load() != State::Buffering) {
        return;
    }
    if (bufferingStartedMs_ > 0) {
        const qint64 elapsed = qMax<qint64>(
            0, monotonicMilliseconds() - bufferingStartedMs_);
        lastRebufferMs_ = elapsed;
        rebufferTotalMs_.fetch_add(elapsed, std::memory_order_relaxed);
        bufferingStartedMs_ = 0;
    }
    setQueuePaused(false);
    audioRebaseSerial_.fetch_add(1, std::memory_order_acq_rel);
    resetClock(positionMs_.load(), paused_.load());
    setState(paused_.load() ? State::Paused : State::Playing);
    appendDiagnostic(
        QStringLiteral("buffer"),
        QStringLiteral("Buffer ready audio=%1ms video=%2ms")
            .arg(audioBufferSnapshot().durationMs)
            .arg(videoBufferSnapshot().durationMs));
}

void FFmpegPlayer::setQueuePaused(bool paused)
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (audioQueue_) {
        audioQueue_->setPaused(paused);
    }
    if (videoQueue_) {
        videoQueue_->setPaused(paused);
    }
}

bool FFmpegPlayer::buffersReady(bool initial) const
{
    return StreamingPolicy::buffersReady(
        initial, audioBufferSnapshot(), videoBufferSnapshot());
}

StreamBufferSnapshot FFmpegPlayer::audioBufferSnapshot() const
{
    StreamBufferSnapshot snapshot;
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (audioQueue_) {
        snapshot.present = true;
        snapshot.bytes = static_cast<qint64>(audioQueue_->bufferedBytes());
        snapshot.durationMs = audioQueue_->bufferedDurationMs();
    }
    return snapshot;
}

StreamBufferSnapshot FFmpegPlayer::videoBufferSnapshot() const
{
    StreamBufferSnapshot snapshot;
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (videoQueue_) {
        snapshot.present = true;
        snapshot.bytes = static_cast<qint64>(videoQueue_->bufferedBytes());
        snapshot.durationMs = videoQueue_->bufferedDurationMs();
    }
    return snapshot;
}

void FFmpegPlayer::readPackets()
{
    std::deque<AVPacket*> pendingVideoPackets;
    AVPacket* pendingAudioPacket = nullptr;
    constexpr std::size_t maxPendingVideoPackets = 64;

    auto clearPendingVideoPackets = [&] {
        for (AVPacket* packet : pendingVideoPackets) {
            av_packet_free(&packet);
        }
        pendingVideoPackets.clear();
    };

    auto clearPendingAudioPacket = [&] {
        if (pendingAudioPacket) {
            av_packet_free(&pendingAudioPacket);
        }
    };

    auto audioQueueIsEmpty = [&] {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return audioQueue_ && audioQueue_->size() == 0;
    };

    auto pumpPendingVideoPackets = [&]() -> bool {
        while (!pendingVideoPackets.empty() && !abort_) {
            PacketQueue* videoQueue = nullptr;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                videoQueue = videoQueue_.get();
            }
            if (!videoQueue) {
                return false;
            }
            if (videoQueue->size() < videoQueue->capacity()) {
                AVPacket* packet = pendingVideoPackets.front();
                if (videoQueue->pushTimed(
                        packet,
                        packetDurationMs(packet, videoStream_.index),
                        0)) {
                    pendingVideoPackets.pop_front();
                    continue;
                }
                if (abort_) {
                    return false;
                }
                // Let the control loop consume seek and track-switch requests.
                return true;
            }

            // A full video queue must not block demuxing while the audio
            // queue is empty, or video waits for a clock that has no packet
            // source and all three threads can stall each other.
            if (hasAudio_ && audioQueueIsEmpty()) {
                return true;
            }

            AVPacket* packet = pendingVideoPackets.front();
            if (videoQueue->pushTimed(
                    packet,
                    packetDurationMs(packet, videoStream_.index),
                    0)) {
                pendingVideoPackets.pop_front();
                continue;
            }
            if (!abort_) {
                return true;
            }
            return false;
        }
        return !abort_;
    };

    while (!abort_) {
        controlPhase_ = 0;
        const unsigned audioGenerationBeforeSwitch =
            audioGeneration_.load(std::memory_order_acquire);
        processTrackSwitchRequests();
        if (audioGenerationBeforeSwitch
            != audioGeneration_.load(std::memory_order_acquire)) {
            clearPendingAudioPacket();
        }
        if (bufferingRequested_.exchange(false)) {
            enterBuffering(QStringLiteral("Consumer buffer underrun"));
        }
        const qint64 requestedSeek = seekRequestMs_.exchange(-1);
        if (requestedSeek >= 0) {
            controlPhase_ = 1;
            clearPendingVideoPackets();
            clearPendingAudioPacket();
            handleSeekLocked(requestedSeek);
        }
        const StreamBufferSnapshot audioSnapshot = audioBufferSnapshot();
        const StreamBufferSnapshot videoSnapshot = videoBufferSnapshot();
        bufferedBytes_ = audioSnapshot.bytes + videoSnapshot.bytes;
        bufferedDurationMs_ = audioSnapshot.present && videoSnapshot.present
            ? qMin(audioSnapshot.durationMs, videoSnapshot.durationMs)
            : (audioSnapshot.present
                   ? audioSnapshot.durationMs
                   : videoSnapshot.durationMs);
        if (state_.load() == State::Buffering && buffersReady(false)) {
            leaveBuffering();
        }
        controlPhase_ = 2;
        if (!pumpPendingVideoPackets()) {
            break;
        }
        if (pendingAudioPacket) {
            PacketQueue* queue = nullptr;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                queue = audioQueue_.get();
            }
            if (queue
                && queue->pushTimed(
                    pendingAudioPacket,
                    packetDurationMs(pendingAudioPacket, audioStream_.index),
                    0)) {
                pendingAudioPacket = nullptr;
            } else if (abort_) {
                break;
            } else {
                QThread::msleep(2);
                continue;
            }
        }

        controlPhase_ = 3;
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            break;
        }
        const int result = readFrameWithRecovery(packet);
        if (result == AVERROR(EAGAIN)) {
            av_packet_free(&packet);
            QThread::msleep(2);
            continue;
        }
        if (result < 0) {
            av_packet_free(&packet);
            if (result == AVERROR_EOF) {
                leaveBuffering();
                controlPhase_ = 6;
                bool seekInterruptedDrain = false;
                while (!pendingVideoPackets.empty() && !abort_) {
                    PacketQueue* videoQueue = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(queueMutex_);
                        videoQueue = videoQueue_.get();
                    }
                    if (!videoQueue) {
                        break;
                    }
                    AVPacket* pending = pendingVideoPackets.front();
                    pendingVideoPackets.pop_front();
                    if (!videoQueue->push(pending)) {
                        seekInterruptedDrain = seekPending_.load()
                            || seekRequestMs_.load() >= 0;
                        break;
                    }
                }
                if (seekInterruptedDrain) {
                    continue;
                }
                // A seek may already be queued even when no pending video
                // packet needed to be drained. Consume it instead of ending
                // the demux thread while another frame is still requested.
                if (seekPending_.load() || seekRequestMs_.load() >= 0) {
                    continue;
                }
            }
            if (!abort_ && result != AVERROR_EOF) {
                const QString message = tr("Media read stopped: %1").arg(ffmpegError(result));
                if (requestHlsFallback(message)) {
                    controlPhase_ = 6;
                    break;
                }
                setLastError(message);
                emit errorOccurred(message);
                setState(State::Error);
            }
            break;
        }

        if (activeHlsVariant_.load(std::memory_order_acquire) >= 0) {
            const qint64 now = monotonicMilliseconds();
            if (hlsSampleStartedMs_ <= 0) {
                hlsSampleStartedMs_ = now;
                hlsSampleBytes_ = 0;
            }
            hlsSampleBytes_ += packet->size;
            const qint64 sampleElapsedMs = now - hlsSampleStartedMs_;
            if (sampleElapsedMs >= 1000) {
                hlsAbrController_.observeThroughput(
                    hlsSampleBytes_, sampleElapsedMs);
                estimatedHlsKbps_ = hlsAbrController_.estimatedKbps();
                hlsSampleStartedMs_ = now;
                hlsSampleBytes_ = 0;
                const QVector<HlsVariant> variants = hlsVariants();
                if (!variants.isEmpty()) {
                    const int chosen = hlsAbrController_.chooseVariant(
                        variants,
                        bufferedDurationMs_.load(std::memory_order_relaxed),
                        state_.load() == State::Buffering);
                    requestAutomaticHlsSwitch(chosen);
                }
            }
        }

        bool queued = false;
        if (subtitleCodecContext_ && packet->stream_index == subtitleStream_.index) {
            decodeSubtitlePacket(packet);
            av_packet_free(&packet);
            continue;
        }
        if (packet->stream_index == audioStream_.index && audioQueue_) {
            controlPhase_ = 4;
            queued = audioQueue_->pushTimed(
                packet,
                packetDurationMs(packet, audioStream_.index),
                0);
            if (!queued) {
                if (abort_) {
                    av_packet_free(&packet);
                } else {
                    pendingAudioPacket = packet;
                    queued = true;
                }
            }
        } else if (packet->stream_index == videoStream_.index && videoQueue_) {
            controlPhase_ = 5;
            if (pendingVideoPackets.empty()
                && videoQueue_->size() < videoQueue_->capacity()) {
                queued = videoQueue_->pushTimed(
                    packet,
                    packetDurationMs(packet, videoStream_.index),
                    0);
            }
            if (!queued && !abort_) {
                pendingVideoPackets.push_back(packet);
                queued = true;
                if (pendingVideoPackets.size() >= maxPendingVideoPackets
                    && audioQueueIsEmpty()) {
                    AVPacket* dropped = pendingVideoPackets.front();
                    pendingVideoPackets.pop_front();
                    av_packet_free(&dropped);
                    videoPacketsDropped_.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (!queued) {
                av_packet_free(&packet);
            }
        } else {
            av_packet_free(&packet);
        }
        if (!queued && abort_) {
            break;
        }
    }
    clearPendingAudioPacket();
    clearPendingVideoPackets();
    controlPhase_ = 7;
}

void FFmpegPlayer::audioLoop()
{
    AVFrame* frame = av_frame_alloc();
    AVFrame* filteredFrame = av_frame_alloc();
    SwrContext* resampler = nullptr;
    QAudioSink* sink = nullptr;
    QIODevice* output = nullptr;
    AVFilterGraph* filterGraph = nullptr;
    AVFilterContext* filterSource = nullptr;
    AVFilterContext* filterSink = nullptr;
    int sinkRate = 0;
    int sinkChannels = 2;
    QAudioFormat sinkFormat;
    int filterSourceRate = 0;
    int filterSourceFormat = AV_SAMPLE_FMT_NONE;
    int filterSourceChannels = 0;
    qint64 filterSourceLayout = 0;
    double filterRate = 1.0;
    int swrSourceRate = 0;
    int swrSourceChannels = 0;
    AVSampleFormat swrSourceFormat = AV_SAMPLE_FMT_NONE;
    int swrOutputRate = 0;
    int swrOutputChannels = 0;
    QByteArray pcmBuffer;
    QString stopReason = QStringLiteral("unknown");
    qint64 outputStallSinceMs = 0;
    bool outputStallReported = false;

    AudioClock* audioClock = nullptr;
    const unsigned generation = audioGeneration_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(clockMutex_);
        audioClock = audioClock_;
    }
    unsigned observedRebaseSerial = audioRebaseSerial_.load();

    auto closeOutput = [&] {
        if (audioClock) {
            audioClock->clearProcessedUsProvider();
        }
        if (sink) {
            sink->stop();
        }
        delete sink;
        sink = nullptr;
        output = nullptr;
        sinkRate = 0;
        sinkFormat = QAudioFormat();
    };

    auto freeFilterGraph = [&] {
        if (filterGraph) {
            avfilter_graph_free(&filterGraph);
        }
        filterSource = nullptr;
        filterSink = nullptr;
        filterSourceRate = 0;
        filterSourceFormat = AV_SAMPLE_FMT_NONE;
        filterSourceChannels = 0;
        filterSourceLayout = 0;
        filterRate = 1.0;
    };

    auto resetAudioPipeline = [&] {
        closeOutput();
        freeFilterGraph();
        if (resampler) {
            swr_free(&resampler);
        }
        swrSourceRate = 0;
        swrSourceChannels = 0;
        swrSourceFormat = AV_SAMPLE_FMT_NONE;
        swrOutputRate = 0;
        swrOutputChannels = 0;
    };

    auto configureResampler = [&](const AVFrame* sourceFrame) -> bool {
        const int sourceRate = sourceFrame->sample_rate;
        const AVSampleFormat sourceFormat = static_cast<AVSampleFormat>(sourceFrame->format);
        const int sourceChannels = channelCount(sourceFrame, audioCodecContext_);
        const int64_t sourceLayout = channelLayout(sourceFrame, audioCodecContext_, sourceChannels);

        const bool resamplerStale = !resampler
            || swrSourceRate != sourceRate
            || swrSourceFormat != sourceFormat
            || swrSourceChannels != sourceChannels
            || swrOutputRate != sinkRate
            || swrOutputChannels != sinkChannels;
        if (!resamplerStale) {
            return true;
        }

        if (resampler) {
            swr_free(&resampler);
        }
        resampler = swr_alloc_set_opts(
            nullptr,
            av_get_default_channel_layout(sinkChannels),
            AV_SAMPLE_FMT_S16,
            sinkRate,
            sourceLayout,
            sourceFormat,
            sourceRate,
            0,
            nullptr);
        if (!resampler || swr_init(resampler) < 0) {
            if (resampler) {
                swr_free(&resampler);
            }
            emit errorOccurred(tr("Cannot initialize the FFmpeg audio resampler."));
            return false;
        }
        swrSourceRate = sourceRate;
        swrSourceFormat = sourceFormat;
        swrSourceChannels = sourceChannels;
        swrOutputRate = sinkRate;
        swrOutputChannels = sinkChannels;
        return true;
    };

    auto ensureOutput = [&](const AVFrame* sourceFrame, qint64 anchorMediaMs) -> bool {
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull()) {
            emit errorOccurred(tr("No audio output device is available."));
            return false;
        }

        int desiredRate = sourceFrame->sample_rate > 0 ? sourceFrame->sample_rate : 48000;
        desiredRate = qBound(
            device.minimumSampleRate(),
            desiredRate,
            device.maximumSampleRate());

        sinkChannels = 2;
        QAudioFormat format;
        format.setSampleRate(desiredRate);
        format.setChannelCount(sinkChannels);
        format.setSampleFormat(QAudioFormat::Int16);
        if (!device.isFormatSupported(format)) {
            format = device.preferredFormat();
            if (format.sampleFormat() != QAudioFormat::Int16) {
                format.setSampleFormat(QAudioFormat::Int16);
            }
            sinkChannels = format.channelCount();
            if (!device.isFormatSupported(format)) {
                emit errorOccurred(tr("The default audio device does not support 16-bit PCM output."));
                return false;
            }
        }
        desiredRate = format.sampleRate();

        if (!sink || sinkFormat != format) {
            closeOutput();
            sink = new QAudioSink(device, format);
            sink->setBufferSize(32768);
            output = sink->start();
            sinkRate = desiredRate;
            sinkFormat = format;
            if (!output) {
                return false;
            }
            if (audioClock) {
                audioClock->setProcessedUsProvider([sink] {
                    return sink->processedUSecs();
                });
                audioClock->setRate(requestedRate_.load());
                audioClock->anchorAt(anchorMediaMs, sink->processedUSecs());
            }
        } else {
            sinkRate = desiredRate;
        }

        return configureResampler(sourceFrame);
    };

    auto buildFilterGraph = [&](const AVFrame* sourceFrame) -> bool {
        const double requestedRate = std::clamp(requestedRate_.load(), 0.25, 4.0);
        const int sourceRate = sourceFrame->sample_rate;
        const int sourceFormat = sourceFrame->format;
        const int sourceChannels = channelCount(sourceFrame, audioCodecContext_);
        const qint64 sourceLayout = channelLayout(
            sourceFrame, audioCodecContext_, sourceChannels);
        const bool stale = !filterGraph
            || filterSourceRate != sourceRate
            || filterSourceFormat != sourceFormat
            || filterSourceChannels != sourceChannels
            || filterSourceLayout != sourceLayout
            || std::abs(filterRate - requestedRate) > 0.0001;
        if (!stale) {
            return true;
        }
        freeFilterGraph();

        const AVFilter* sourceFilter = avfilter_get_by_name("abuffer");
        const AVFilter* sinkFilter = avfilter_get_by_name("abuffersink");
        const AVFilter* tempoFilter = avfilter_get_by_name("atempo");
        if (!sourceFilter || !sinkFilter || !tempoFilter) {
            emit errorOccurred(tr("The FFmpeg atempo filter is unavailable."));
            return false;
        }

        AVFilterGraph* graph = avfilter_graph_alloc();
        if (!graph) {
            return false;
        }

        char layoutText[64]{};
        av_get_channel_layout_string(
            layoutText, sizeof(layoutText), sourceChannels, sourceLayout);
        const QByteArray arguments = QStringLiteral(
            "sample_rate=%1:sample_fmt=%2:channel_layout=%3:time_base=1/%1")
                                         .arg(sourceRate)
                                         .arg(QString::fromUtf8(
                                             av_get_sample_fmt_name(
                                                 static_cast<AVSampleFormat>(sourceFormat))))
                                         .arg(QString::fromUtf8(layoutText))
                                         .toUtf8();

        AVFilterContext* sourceContext = nullptr;
        AVFilterContext* sinkContext = nullptr;
        int result = avfilter_graph_create_filter(
            &sourceContext,
            sourceFilter,
            "audio_in",
            arguments.constData(),
            nullptr,
            graph);
        if (result >= 0) {
            result = avfilter_graph_create_filter(
                &sinkContext, sinkFilter, "audio_out", nullptr, nullptr, graph);
        }
        if (result < 0) {
            avfilter_graph_free(&graph);
            return false;
        }

        AVFilterContext* previous = sourceContext;
        auto appendTempo = [&](double tempo) -> bool {
            AVFilterContext* context = avfilter_graph_alloc_filter(
                graph, tempoFilter, nullptr);
            if (!context
                || av_opt_set_double(context, "tempo", tempo, AV_OPT_SEARCH_CHILDREN) < 0
                || avfilter_init_str(context, nullptr) < 0) {
                return false;
            }
            if (avfilter_link(previous, 0, context, 0) < 0) {
                return false;
            }
            previous = context;
            return true;
        };

        bool filtersReady = false;
        if (requestedRate < 0.5 || requestedRate > 2.0) {
            const double factor = std::sqrt(requestedRate);
            filtersReady = appendTempo(factor) && appendTempo(factor);
        } else {
            filtersReady = appendTempo(requestedRate);
        }
        if (!filtersReady
            || avfilter_link(previous, 0, sinkContext, 0) < 0
            || avfilter_graph_config(graph, nullptr) < 0) {
            avfilter_graph_free(&graph);
            return false;
        }

        filterGraph = graph;
        filterSource = sourceContext;
        filterSink = sinkContext;
        filterSourceRate = sourceRate;
        filterSourceFormat = sourceFormat;
        filterSourceChannels = sourceChannels;
        filterSourceLayout = sourceLayout;
        filterRate = requestedRate;
        return true;
    };

    auto writeFilteredFrame = [&](const AVFrame* filtered, qint64 anchorMediaMs) -> bool {
        if (!ensureOutput(filtered, anchorMediaMs)) {
            return false;
        }
        const int inputRate = filtered->sample_rate;
        const int maxOutSamples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(resampler, inputRate) + filtered->nb_samples,
            sinkRate,
            inputRate,
            AV_ROUND_UP));
        pcmBuffer.resize(
            maxOutSamples * sinkChannels * static_cast<int>(sizeof(qint16)));
        uint8_t* destination[1] = {
            reinterpret_cast<uint8_t*>(pcmBuffer.data())
        };
        const int converted = swr_convert(
            resampler,
            destination,
            maxOutSamples,
            const_cast<const uint8_t**>(filtered->extended_data),
            filtered->nb_samples);
        if (converted <= 0) {
            return true;
        }

        pcmBuffer.resize(converted * sinkChannels * static_cast<int>(sizeof(qint16)));
        audioFramesWritten_.fetch_add(converted, std::memory_order_relaxed);
        if (recognitionAudioEnabled_.load()) {
            emit audioSamplesReady(
                pcmBuffer,
                sinkRate,
                sinkChannels,
                anchorMediaMs);
        }
        const float gain = requestedMuted_.load()
            ? 0.0f
            : static_cast<float>(requestedVolume_.load()) / 100.0f;
        applyVolume(&pcmBuffer, gain);

        qsizetype offset = 0;
        while (!abort_
               && generation == audioGeneration_.load(std::memory_order_acquire)
               && offset < pcmBuffer.size()) {
            if (paused_.load()) {
                break;
            }
            const qsizetype available = sink ? qMax<qsizetype>(0, sink->bytesFree()) : 0;
            if (available <= 0) {
                if (outputStallSinceMs == 0) {
                    outputStallSinceMs = monotonicMilliseconds();
                    outputStallReported = false;
                } else if (!outputStallReported
                           && monotonicMilliseconds() - outputStallSinceMs > 1000) {
                    appendDiagnostic(
                        QStringLiteral("audio"),
                        QStringLiteral("Audio output has no free space for >1s state=%1")
                            .arg(sink ? static_cast<int>(sink->state()) : -1));
                    outputStallReported = true;
                }
                QThread::msleep(1);
                continue;
            }
            outputStallSinceMs = 0;
            const qsizetype chunk = qMin(available, pcmBuffer.size() - offset);
            const qint64 written = output->write(
                pcmBuffer.constData() + offset, chunk);
            if (written > 0) {
                offset += written;
            } else {
                QThread::msleep(1);
            }
        }
        updatePosition(clockMediaTimeMs());
        return true;
    };

    bool suspended = false;
    AVPacket* packet = nullptr;
    while (!abort_
           && generation == audioGeneration_.load(std::memory_order_acquire)) {
        const unsigned rebaseSerial = audioRebaseSerial_.load();
        if (rebaseSerial != observedRebaseSerial) {
            resetAudioPipeline();
            observedRebaseSerial = rebaseSerial;
            suspended = false;
        }

        if (paused_.load()) {
            if (sink && !suspended) {
                sink->suspend();
                suspended = true;
            }
            QThread::msleep(10);
            continue;
        }
        if (sink && suspended) {
            sink->resume();
            suspended = false;
        }

        int receiveResult = 0;
        {
            std::lock_guard<std::mutex> lock(codecMutex_);
            receiveResult = avcodec_receive_frame(audioCodecContext_, frame);
        }
        if (receiveResult == AVERROR_EOF) {
            stopReason = QStringLiteral("decoder eof");
            if (filterSource) {
                av_buffersrc_add_frame_flags(
                    filterSource, nullptr, AV_BUFFERSRC_FLAG_PUSH);
                while (!abort_ && av_buffersink_get_frame(
                           filterSink, filteredFrame) >= 0) {
                    const qint64 anchor = qMax(
                        dropUntilMs_.load(), clockMediaTimeMs());
                    if (!writeFilteredFrame(filteredFrame, anchor)) {
                        abortPlayback();
                        break;
                    }
                    av_frame_unref(filteredFrame);
                }
            }
            break;
        }
        if (receiveResult < 0 && receiveResult != AVERROR(EAGAIN)) {
            stopReason = QStringLiteral("decoder error %1").arg(ffmpegError(receiveResult));
            av_frame_unref(frame);
            continue;
        }
        if (receiveResult == AVERROR(EAGAIN)) {
            if (!packet) {
                bool sourceFinished = false;
                PacketQueue* queue = nullptr;
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    queue = audioQueue_.get();
                }
                if (queue) {
                    packet = queue->pop();
                    if (packet) {
                        audioPacketsDecoded_.fetch_add(1, std::memory_order_relaxed);
                    }
                    sourceFinished = queue->isFinishedAndEmpty();
                }
                if (!packet && sourceFinished) {
                    break;
                }
                if (!packet && !sourceFinished
                    && networkTarget_.load()
                    && !seekPending_.load()) {
                    bufferingRequested_ = true;
                }
            }
            if (!packet) {
                continue;
            }

            int sendResult = 0;
            {
                std::lock_guard<std::mutex> lock(codecMutex_);
                sendResult = avcodec_send_packet(audioCodecContext_, packet);
            }
            if (sendResult == AVERROR(EAGAIN)) {
                // Keep packet ownership until the decoder accepts it. Releasing
                // it here can lose a drain sentinel and strand the decode thread.
                QThread::msleep(1);
                continue;
            }
            if (sendResult < 0) {
                av_packet_free(&packet);
                continue;
            }
            av_packet_free(&packet);
            continue;
        }

        const qint64 frameMs = frameTimeMs(frame, audioStream_.index);
        audioFramesDecoded_.fetch_add(1, std::memory_order_relaxed);
        if (frameMs >= 0 && frameMs + 40 < dropUntilMs_.load()) {
            av_frame_unref(frame);
            continue;
        }
        if (!buildFilterGraph(frame)) {
            av_frame_unref(frame);
            abortPlayback();
            break;
        }

        frame->pts = av_rescale_q(
            qMax<qint64>(0, frameMs),
            AVRational{1, 1000},
            AVRational{1, qMax(1, frame->sample_rate)});
        if (av_buffersrc_add_frame_flags(
                filterSource, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
            av_frame_unref(frame);
            continue;
        }

        const qint64 anchorMediaMs = qMax(dropUntilMs_.load(), frameMs);
        int filteredResult = 0;
        while (!abort_
               && (filteredResult = av_buffersink_get_frame(
                       filterSink, filteredFrame)) >= 0) {
            if (!writeFilteredFrame(filteredFrame, anchorMediaMs)) {
                abortPlayback();
                break;
            }
            av_frame_unref(filteredFrame);
        }
        av_frame_unref(frame);
    }
    av_packet_free(&packet);

    resetAudioPipeline();
    av_frame_free(&frame);
    av_frame_free(&filteredFrame);
    appendDiagnostic(
        QStringLiteral("audio"),
        QStringLiteral("Audio loop stopped reason=%1 generation=%2 current=%3 abort=%4")
            .arg(stopReason)
            .arg(generation)
            .arg(audioGeneration_.load())
            .arg(abort_.load() ? QStringLiteral("true") : QStringLiteral("false")));
}

void FFmpegPlayer::videoLoop()
{
    AVFrame* frame = av_frame_alloc();
    AVFrame* softwareFrame = av_frame_alloc();
    SwsContext* scaler = nullptr;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int sourceFormat = -1;
    AVPacket* packet = nullptr;
    double videoJitterMs = 0.0;
    int hardwareTransferFailures = 0;

    while (!abort_) {
        const bool pausedNow = paused_.load();
        const bool stepping = pausedNow && videoStepCount_.load() > 0;
        const bool previewing = pausedNow && previewPending_.load();
        if (pausedNow && !stepping && !previewing) {
            QThread::msleep(5);
            continue;
        }

        const quint64 requestSerialAtDecode =
            seekRequestSerial_.load(std::memory_order_acquire);
        const quint64 appliedSerialAtDecode =
            seekAppliedSerial_.load(std::memory_order_acquire);
        int receiveResult = 0;
        {
            std::lock_guard<std::mutex> lock(codecMutex_);
            receiveResult = avcodec_receive_frame(videoCodecContext_, frame);
        }
        if (receiveResult == AVERROR_EOF) {
            break;
        }
        if (receiveResult < 0 && receiveResult != AVERROR(EAGAIN)) {
            av_frame_unref(frame);
            continue;
        }
        if (receiveResult == AVERROR(EAGAIN)) {
            if (!packet) {
                bool sourceFinished = false;
                PacketQueue* queue = nullptr;
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    queue = videoQueue_.get();
                }
                if (queue) {
                    packet = queue->pop();
                    if (packet) {
                        videoPacketsDecoded_.fetch_add(1, std::memory_order_relaxed);
                    }
                    sourceFinished = queue->isFinishedAndEmpty();
                }
                if (!packet && sourceFinished) {
                    break;
                }
                if (!packet && !sourceFinished
                    && networkTarget_.load()
                    && !seekPending_.load()) {
                    bufferingRequested_ = true;
                }
            }
            if (!packet) {
                continue;
            }

            int sendResult = 0;
            {
                std::lock_guard<std::mutex> lock(codecMutex_);
                sendResult = avcodec_send_packet(videoCodecContext_, packet);
            }
            if (sendResult == AVERROR(EAGAIN)) {
                // Keep the unconsumed packet and retry after receive drains the
                // decoder's output queue.
                QThread::msleep(1);
                continue;
            }
            if (sendResult < 0) {
                av_packet_free(&packet);
                continue;
            }
            av_packet_free(&packet);
            continue;
        }

        if (requestSerialAtDecode
                != seekRequestSerial_.load(std::memory_order_acquire)
            || appliedSerialAtDecode
                != seekAppliedSerial_.load(std::memory_order_acquire)) {
            av_frame_unref(frame);
            continue;
        }

        const qint64 frameMs = frameTimeMs(frame, videoStream_.index);
        videoFramesDecoded_.fetch_add(1, std::memory_order_relaxed);
        if (frameMs >= 0 && frameMs < dropUntilMs_.load()) {
            av_frame_unref(frame);
            continue;
        }
        const qint64 clockMs = clockMediaTimeMs();
        const qint64 latenessMs = frameMs >= 0 ? clockMs - frameMs : 0;
        const double dropThreshold = videoDropThresholdMs(
            videoFrameRate_.load(), videoJitterMs);
        if (!stepping && !previewing && frameMs >= 0
            && latenessMs > static_cast<qint64>(std::lround(dropThreshold))) {
            // The decoder fell far behind the media clock (rate change, slow
            // decode, or GUI stall). Drop the stale frame instead of playing
            // the backlog as a burst.
            videoFramesDropped_.fetch_add(1, std::memory_order_relaxed);
            videoJitterMs = videoJitterMs * 0.9 + std::abs(
                static_cast<double>(latenessMs)) * 0.1;
            av_frame_unref(frame);
            continue;
        }
        if (!stepping && !previewing && frameMs >= 0
            && !waitUntil(
                frameMs,
                true,
                requestSerialAtDecode,
                appliedSerialAtDecode)) {
            av_frame_unref(frame);
            continue;
        }
        if (seekPending_.load()
            || requestSerialAtDecode
                != seekRequestSerial_.load(std::memory_order_acquire)
            || appliedSerialAtDecode
                != seekAppliedSerial_.load(std::memory_order_acquire)) {
            av_frame_unref(frame);
            continue;
        }

        bool gpuPresented = false;
        VideoCanvas* canvas = canvas_.load(std::memory_order_acquire);
        const int hardwarePixelFormat = hardwarePixelFormat_.load(
            std::memory_order_acquire);
        const bool hardwareFrame = hardwarePixelFormat != AV_PIX_FMT_NONE
            && frame->format == hardwarePixelFormat;
        if (hardwareFrame
            && hardwareZeroCopyEnabled_.load(std::memory_order_acquire)
            && canvas
            && canvas->gpuRenderReady()
            && frame->hw_frames_ctx) {
            auto* framesContext = reinterpret_cast<AVHWFramesContext*>(
                frame->hw_frames_ctx->data);
            auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
            const intptr_t arrayIndex = reinterpret_cast<intptr_t>(frame->data[1]);
            int gpuPixelFormat = -1;
            if (framesContext->sw_format == AV_PIX_FMT_NV12) {
                gpuPixelFormat = 0;
            } else if (framesContext->sw_format == AV_PIX_FMT_P010) {
                gpuPixelFormat = 1;
            }

            D3D11_TEXTURE2D_DESC textureDescription{};
            if (texture) {
                texture->GetDesc(&textureDescription);
            }
            AVFrame* retained = (texture && gpuPixelFormat >= 0)
                ? av_frame_clone(frame)
                : nullptr;
            if (retained && frame->width > 0 && frame->height > 0) {
                std::shared_ptr<AVFrame> lifetime(
                    retained, [](AVFrame* value) { av_frame_free(&value); });
                retained = nullptr;
                GpuVideoFrame gpuFrame;
                gpuFrame.frame = std::move(lifetime);
                gpuFrame.texture = texture;
                gpuFrame.arrayIndex = static_cast<int>(arrayIndex);
                gpuFrame.arraySize = static_cast<int>(
                    std::max<UINT>(1, textureDescription.ArraySize));
                gpuFrame.size = QSize(frame->width, frame->height);
                gpuFrame.pixelFormat = gpuPixelFormat;
                gpuFrame.fullRange = frame->color_range == AVCOL_RANGE_JPEG;
                gpuFrame.bt709 = frame->colorspace == AVCOL_SPC_BT709
                    || (frame->colorspace == AVCOL_SPC_UNSPECIFIED
                        && frame->height >= 720);

                if (canvas) {
                    canvas->setHardwareFrame(std::move(gpuFrame));
                    videoFramesPresented_.fetch_add(1, std::memory_order_relaxed);
                    gpuFramesPresented_.fetch_add(1, std::memory_order_relaxed);
                    gpuPresented = true;
                }
            }
            av_frame_free(&retained);
        }

        AVFrame* displayFrame = frame;
        if (hardwareFrame && !gpuPresented) {
            av_frame_unref(softwareFrame);
            const int transferResult = av_hwframe_transfer_data(
                softwareFrame, frame, 0);
            if (transferResult < 0) {
                ++hardwareTransferFailures;
                if (hardwareTransferFailures >= 3) {
                    bool switched = false;
                    {
                        std::lock_guard<std::mutex> lock(codecMutex_);
                        switched = switchVideoDecoderToSoftware();
                    }
                    hardwareTransferFailures = 0;
                    if (!switched) {
                        abortPlayback();
                    }
                }
                av_frame_unref(frame);
                continue;
            }
            hardwareTransferFailures = 0;
            hardwareReadbacks_.fetch_add(1, std::memory_order_relaxed);
            displayFrame = softwareFrame;
        }

        if (!gpuPresented) {
            if (!scaler
                || sourceWidth != displayFrame->width
                || sourceHeight != displayFrame->height
                || sourceFormat != displayFrame->format) {
                if (scaler) {
                    sws_freeContext(scaler);
                }
                scaler = sws_getContext(
                    displayFrame->width,
                    displayFrame->height,
                    static_cast<AVPixelFormat>(displayFrame->format),
                    displayFrame->width,
                    displayFrame->height,
                    AV_PIX_FMT_BGRA,
                    SWS_BILINEAR,
                    nullptr,
                    nullptr,
                    nullptr);
                sourceWidth = displayFrame->width;
                sourceHeight = displayFrame->height;
                sourceFormat = displayFrame->format;
            }

            if (scaler && displayFrame->width > 0 && displayFrame->height > 0) {
                QImage image = framePool_.acquire(
                    displayFrame->width,
                    displayFrame->height,
                    QImage::Format_ARGB32);
                if (image.isNull()) {
                    av_frame_unref(frame);
                    av_frame_unref(softwareFrame);
                    continue;
                }
                uint8_t* destination[4] = {image.bits(), nullptr, nullptr, nullptr};
                int destinationStride[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
                sws_scale(
                    scaler,
                    displayFrame->data,
                    displayFrame->linesize,
                    0,
                    displayFrame->height,
                    destination,
                    destinationStride);

                if (canvas) {
                    canvas->setFrame(image);
                }
                videoFramesPresented_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (frameMs >= 0) {
            const qint64 driftMs = clockMediaTimeMs() - frameMs;
            lastAvDriftMs_ = driftMs;
            videoJitterMs = videoJitterMs * 0.9
                + std::abs(static_cast<double>(driftMs)) * 0.1;
            updatePosition(frameMs);
        }
        if (previewing) {
            previewPending_ = false;
            resetClock(positionMs_.load(), true);
        }
        if (stepping) {
            resetClock(positionMs_.load(), true);
            videoStepCount_.fetch_sub(1);
        }
        av_frame_unref(frame);
        av_frame_unref(softwareFrame);
    }

    av_packet_free(&packet);
    if (scaler) {
        sws_freeContext(scaler);
    }
    av_frame_free(&frame);
    av_frame_free(&softwareFrame);
}

void FFmpegPlayer::updatePosition(qint64 milliseconds)
{
    if (milliseconds < 0) {
        return;
    }
    qint64 current = positionMs_.load();
    if (milliseconds > current) {
        positionMs_ = milliseconds;
    }
}

qint64 FFmpegPlayer::clockMediaTimeMs() const
{
    std::lock_guard<std::mutex> lock(clockMutex_);
    return clock_ ? clock_->mediaTimeMs() : 0;
}

void FFmpegPlayer::setClockPaused(bool paused)
{
    std::lock_guard<std::mutex> lock(clockMutex_);
    if (clock_) {
        clock_->setPaused(paused);
    }
}

void FFmpegPlayer::setClockRate(double rate)
{
    std::lock_guard<std::mutex> lock(clockMutex_);
    if (clock_) {
        clock_->setRate(rate);
    }
}

void FFmpegPlayer::resetClock(qint64 mediaMs, bool paused)
{
    std::lock_guard<std::mutex> lock(clockMutex_);
    if (!clock_) {
        return;
    }
    clock_->reset(mediaMs);
    clock_->setPaused(paused);
}

bool FFmpegPlayer::waitUntil(
    qint64 mediaMilliseconds,
    bool allowFrameStep,
    quint64 requestSerial,
    quint64 appliedSerial)
{
    while (!abort_) {
        if (seekPending_.load()
            || requestSerial
                != seekRequestSerial_.load(std::memory_order_acquire)
            || appliedSerial
                != seekAppliedSerial_.load(std::memory_order_acquire)) {
            return false;
        }
        if (paused_.load()) {
            // A frame-step request is the only event that may release the
            // video thread while the media clock is paused.
            if (allowFrameStep && videoStepCount_.load() > 0) {
                return false;
            }
            QThread::msleep(5);
            continue;
        }
        const qint64 current = clockMediaTimeMs();
        if (current >= mediaMilliseconds) {
            return true;
        }
        // mediaMilliseconds is media time; the wall-clock sleep must be
        // divided by the playback rate, otherwise high rates oversleep and
        // audio underruns or video runs ahead of the clock.
        double clockRate = 1.0;
        {
            std::lock_guard<std::mutex> lock(clockMutex_);
            clockRate = clock_ ? qMax(0.01, clock_->rate()) : 1.0;
        }
        const qint64 wallMs = static_cast<qint64>(std::lround(
            static_cast<double>(mediaMilliseconds - current) / clockRate));
        QThread::msleep(static_cast<unsigned long>(
            std::clamp<qint64>(wallMs, 1, 10)));
    }
    return false;
}

qint64 FFmpegPlayer::packetTimeMs(const AVPacket* packet, int streamIndex) const
{
    if (!packet || streamIndex < 0) {
        return -1;
    }
    const StreamInfo* stream = nullptr;
    if (streamIndex == audioStream_.index) {
        stream = &audioStream_;
    } else if (streamIndex == videoStream_.index) {
        stream = &videoStream_;
    } else if (streamIndex == subtitleStream_.index) {
        stream = &subtitleStream_;
    } else {
        return -1;
    }
    qint64 timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (timestamp == AV_NOPTS_VALUE) {
        return -1;
    }
    if (stream->startTimestamp != AV_NOPTS_VALUE) {
        timestamp -= stream->startTimestamp;
    }
    return qMax<qint64>(0, rationalToMilliseconds(timestamp, stream->timeBase));
}

qint64 FFmpegPlayer::packetDurationMs(const AVPacket* packet, int streamIndex) const
{
    if (!packet) {
        return 0;
    }
    const StreamInfo* stream = nullptr;
    if (streamIndex == audioStream_.index) {
        stream = &audioStream_;
    } else if (streamIndex == videoStream_.index) {
        stream = &videoStream_;
    } else if (streamIndex == subtitleStream_.index) {
        stream = &subtitleStream_;
    }
    if (stream && packet->duration > 0) {
        return qMax<qint64>(
            0, rationalToMilliseconds(packet->duration, stream->timeBase));
    }
    if (stream == &audioStream_ && audioSampleRate_.load() > 0) {
        return qMax<qint64>(1, 1024LL * 1000LL / audioSampleRate_.load());
    }
    if (stream == &videoStream_) {
        const double frameRate = videoFrameRate_.load();
        if (frameRate > 0.0) {
            return qMax<qint64>(1, static_cast<qint64>(std::llround(1000.0 / frameRate)));
        }
    }
    return 20;
}

qint64 FFmpegPlayer::frameTimeMs(const AVFrame* frame, int streamIndex) const
{
    if (!frame || streamIndex < 0) {
        return -1;
    }
    const StreamInfo* stream = nullptr;
    if (streamIndex == audioStream_.index) {
        stream = &audioStream_;
    } else if (streamIndex == videoStream_.index) {
        stream = &videoStream_;
    } else if (streamIndex == subtitleStream_.index) {
        stream = &subtitleStream_;
    } else {
        return -1;
    }
    qint64 timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame->pts;
    }
    if (timestamp == AV_NOPTS_VALUE) {
        return -1;
    }
    if (stream->startTimestamp != AV_NOPTS_VALUE) {
        timestamp -= stream->startTimestamp;
    }
    return qMax<qint64>(0, rationalToMilliseconds(timestamp, stream->timeBase));
}

void FFmpegPlayer::setLastError(const QString& message)
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = message;
}

bool FFmpegPlayer::handleSeekLocked(qint64 targetMs)
{
    if (!formatContext_) {
        seekPending_ = false;
        return false;
    }

    const qint64 targetTimestamp = targetMs * AV_TIME_BASE / 1000;
    int result = 0;
    {
        std::lock_guard<std::mutex> lock(codecMutex_);
        controlPhase_ = 11;
        result = av_seek_frame(formatContext_, -1, targetTimestamp, AVSEEK_FLAG_BACKWARD);
        if (result >= 0) {
            if (audioCodecContext_) {
                controlPhase_ = 12;
                avcodec_flush_buffers(audioCodecContext_);
            }
            if (videoCodecContext_) {
                controlPhase_ = 13;
                avcodec_flush_buffers(videoCodecContext_);
            }
            if (subtitleCodecContext_) {
                controlPhase_ = 14;
                avcodec_flush_buffers(subtitleCodecContext_);
            }
        }
    }

    controlPhase_ = 15;
    const bool wasBuffering = state_.load() == State::Buffering;
    bufferingRequested_ = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (audioQueue_) {
            audioQueue_->clear();
            audioQueue_->setBlocked(false);
            audioQueue_->setPaused(false);
        }
        if (videoQueue_) {
            videoQueue_->clear();
            videoQueue_->setBlocked(false);
            videoQueue_->setPaused(false);
        }
    }

    if (wasBuffering && bufferingStartedMs_ > 0) {
        const qint64 elapsed = qMax<qint64>(
            0, monotonicMilliseconds() - bufferingStartedMs_);
        lastRebufferMs_ = elapsed;
        rebufferTotalMs_.fetch_add(elapsed, std::memory_order_relaxed);
    }
    bufferingStartedMs_ = 0;

    seekPending_ = false;
    seekCommandsHandled_.fetch_add(1, std::memory_order_relaxed);
    if (result < 0) {
        seekAppliedSerial_.fetch_add(1, std::memory_order_acq_rel);
        const QString message = tr("Seek failed: %1").arg(ffmpegError(result));
        setLastError(message);
        emit errorOccurred(message);
        return false;
    }

    positionMs_ = targetMs;
    dropUntilMs_ = targetMs;
    previewPending_ = paused_.load() && hasVideo_.load();
    audioRebaseSerial_.fetch_add(1, std::memory_order_acq_rel);
    resetClock(targetMs, paused_.load());
    seekAppliedSerial_.fetch_add(1, std::memory_order_acq_rel);
    if (networkTarget_.load() && !paused_.load()) {
        setQueuePaused(true);
        setClockPaused(true);
        setState(State::Buffering);
    } else {
        setState(paused_.load() ? State::Paused : State::Playing);
    }
    return true;
}

void FFmpegPlayer::closeQueues()
{
    std::lock_guard<std::mutex> lock(queueMutex_);
    audioQueue_.reset();
    videoQueue_.reset();
}

#include "PlaybackClock.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

using SteadyClock = std::chrono::steady_clock;

qint64 monotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               SteadyClock::now().time_since_epoch())
        .count();
}

} // namespace

bool PlaybackClock::isAudioDriven() const
{
    return false;
}

WallClock::WallClock(qint64 mediaMs)
    : mediaMs_(qMax<qint64>(0, mediaMs))
    , wallAtResetMs_(monotonicMilliseconds())
{
}

qint64 WallClock::mediaTimeMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return currentMediaTimeLocked();
}

void WallClock::reset(qint64 mediaMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    mediaMs_ = qMax<qint64>(0, mediaMs);
    wallAtResetMs_ = monotonicMilliseconds();
}

void WallClock::setPaused(bool paused)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_ == paused) {
        return;
    }
    mediaMs_ = currentMediaTimeLocked();
    wallAtResetMs_ = monotonicMilliseconds();
    paused_ = paused;
}

void WallClock::setRate(double rate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (rate <= 0.0) {
        return;
    }
    mediaMs_ = currentMediaTimeLocked();
    wallAtResetMs_ = monotonicMilliseconds();
    rate_ = rate;
}

double WallClock::rate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rate_;
}

qint64 WallClock::currentMediaTimeLocked() const
{
    if (paused_) {
        return mediaMs_;
    }
    const double elapsed = static_cast<double>(monotonicMilliseconds() - wallAtResetMs_);
    return mediaMs_ + static_cast<qint64>(elapsed * rate_);
}

AudioClock::AudioClock(qint64 mediaMs)
    : anchorMediaMs_(qMax<qint64>(0, mediaMs))
{
}

AudioClock::~AudioClock() = default;

qint64 AudioClock::mediaTimeMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return currentMediaTimeLocked();
}

void AudioClock::reset(qint64 mediaMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    anchorMediaMs_ = qMax<qint64>(0, mediaMs);
    anchorProcessedUs_ = 0;
    anchored_ = false;
}

void AudioClock::setPaused(bool paused)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_ == paused) {
        return;
    }
    if (paused) {
        anchorMediaMs_ = currentMediaTimeLocked();
        anchorProcessedUs_ = processedUsLocked();
        anchored_ = false;
        paused_ = true;
        return;
    }

    paused_ = false;
    rebaseLocked(anchorMediaMs_);
}

void AudioClock::setRate(double rate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (rate <= 0.0) {
        return;
    }
    if (!paused_) {
        const qint64 current = currentMediaTimeLocked();
        anchorMediaMs_ = current;
        anchorProcessedUs_ = processedUsLocked();
    }
    rate_ = rate;
}

double AudioClock::rate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rate_;
}

bool AudioClock::isAudioDriven() const
{
    return true;
}

void AudioClock::setProcessedUsProvider(ProcessedUsProvider provider)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const qint64 current = currentMediaTimeLocked();
    processedUsProvider_ = std::move(provider);
    if (processedUsProvider_ && !paused_) {
        anchorMediaMs_ = current;
        anchorProcessedUs_ = processedUsLocked();
        anchored_ = true;
    }
}

void AudioClock::clearProcessedUsProvider()
{
    std::lock_guard<std::mutex> lock(mutex_);
    anchorMediaMs_ = currentMediaTimeLocked();
    processedUsProvider_ = {};
    anchorProcessedUs_ = 0;
    anchored_ = false;
}

void AudioClock::anchorAt(qint64 mediaMs, qint64 processedUs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    anchorMediaMs_ = qMax<qint64>(0, mediaMs);
    anchorProcessedUs_ = qMax<qint64>(0, processedUs);
    anchored_ = true;
}

bool AudioClock::isAnchored() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return anchored_;
}

qint64 AudioClock::currentMediaTimeLocked() const
{
    if (paused_ || !anchored_) {
        return anchorMediaMs_;
    }
    const qint64 processedUs = processedUsLocked();
    const qint64 deltaUs = qMax<qint64>(0, processedUs - anchorProcessedUs_);
    return anchorMediaMs_
        + static_cast<qint64>(std::llround(
            static_cast<double>(deltaUs) * rate_ / 1000.0));
}

qint64 AudioClock::processedUsLocked() const
{
    return processedUsProvider_ ? qMax<qint64>(0, processedUsProvider_())
                                : anchorProcessedUs_;
}

void AudioClock::rebaseLocked(qint64 mediaMs)
{
    anchorMediaMs_ = qMax<qint64>(0, mediaMs);
    if (processedUsProvider_) {
        anchorProcessedUs_ = processedUsLocked();
        anchored_ = true;
    } else {
        anchorProcessedUs_ = 0;
        anchored_ = false;
    }
}

double videoDropThresholdMs(double frameRate, double jitterMs)
{
    const double normalizedRate = frameRate > 1.0 ? frameRate : 30.0;
    const double frameIntervalMs = 1000.0 / normalizedRate;
    return std::clamp(frameIntervalMs * 1.5 + qMax(0.0, jitterMs) * 2.0, 35.0, 180.0);
}


#pragma once

#include <QtGlobal>

#include <functional>
#include <mutex>

class PlaybackClock
{
public:
    virtual ~PlaybackClock() = default;

    virtual qint64 mediaTimeMs() const = 0;
    virtual void reset(qint64 mediaMs) = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void setRate(double rate) = 0;
    virtual double rate() const = 0;
    virtual bool isAudioDriven() const;
};

class WallClock final : public PlaybackClock
{
public:
    explicit WallClock(qint64 mediaMs = 0);

    qint64 mediaTimeMs() const override;
    void reset(qint64 mediaMs) override;
    void setPaused(bool paused) override;
    void setRate(double rate) override;
    double rate() const override;

private:
    qint64 currentMediaTimeLocked() const;

    mutable std::mutex mutex_;
    qint64 mediaMs_ = 0;
    qint64 wallAtResetMs_ = 0;
    double rate_ = 1.0;
    bool paused_ = false;
};

class AudioClock final : public PlaybackClock
{
public:
    using ProcessedUsProvider = std::function<qint64()>;

    explicit AudioClock(qint64 mediaMs = 0);
    ~AudioClock() override;

    qint64 mediaTimeMs() const override;
    void reset(qint64 mediaMs) override;
    void setPaused(bool paused) override;
    void setRate(double rate) override;
    double rate() const override;
    bool isAudioDriven() const override;

    void setProcessedUsProvider(ProcessedUsProvider provider);
    void clearProcessedUsProvider();
    void anchorAt(qint64 mediaMs, qint64 processedUs);
    bool isAnchored() const;

private:
    qint64 currentMediaTimeLocked() const;
    qint64 processedUsLocked() const;
    void rebaseLocked(qint64 mediaMs);

    mutable std::mutex mutex_;
    ProcessedUsProvider processedUsProvider_;
    qint64 anchorMediaMs_ = 0;
    qint64 anchorProcessedUs_ = 0;
    double rate_ = 1.0;
    bool paused_ = false;
    bool anchored_ = false;
};

double videoDropThresholdMs(double frameRate, double jitterMs);


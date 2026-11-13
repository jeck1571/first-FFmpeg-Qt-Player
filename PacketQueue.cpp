#include "PacketQueue.h"

#include <chrono>

PacketQueue::PacketQueue(std::size_t capacity, std::size_t maxBytes)
    : capacity_(capacity > 0 ? capacity : 1)
    , maxBytes_(maxBytes)
{
}

PacketQueue::~PacketQueue()
{
    clear();
}

bool PacketQueue::push(AVPacket* packet, int timeoutMs)
{
    const bool pushed = pushTimed(packet, 0, timeoutMs);
    if (!pushed && packet) {
        av_packet_free(&packet);
    }
    return pushed;
}

bool PacketQueue::pushTimed(AVPacket* packet, qint64 durationMs, int timeoutMs)
{
    if (!packet) {
        return false;
    }
    const std::size_t packetBytes = packet->size > 0
        ? static_cast<std::size_t>(packet->size)
        : 0;
    const qint64 packetDuration = qMax<qint64>(0, durationMs);

    std::unique_lock<std::mutex> lock(mutex_);
    const auto ready = [this, packetBytes] {
        const bool packetRoom = packets_.size() < capacity_;
        const bool byteRoom = maxBytes_ == 0
            || packets_.empty()
            || bufferedBytes_ + packetBytes <= maxBytes_;
        return aborted_ || blocked_ || finished_ || (packetRoom && byteRoom);
    };
    if (timeoutMs < 0) {
        wake_.wait(lock, ready);
    } else if (!wake_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) {
        return false;
    }

    if (aborted_ || blocked_ || finished_) {
        return false;
    }
    packets_.push_back({packet, packetDuration});
    bufferedBytes_ += packetBytes;
    bufferedDurationMs_ += packetDuration;
    changed_.notify_all();
    return true;
}

bool PacketQueue::tryPush(AVPacket* packet)
{
    return push(packet, 0);
}

AVPacket* PacketQueue::pop(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return aborted_ || paused_ || !packets_.empty()
            || (finished_ && packets_.empty());
    });
    if (aborted_ || paused_ || packets_.empty()) {
        return nullptr;
    }
    const QueuedPacket queued = packets_.front();
    packets_.pop_front();
    bufferedBytes_ -= queued.packet->size > 0
        ? static_cast<std::size_t>(queued.packet->size)
        : 0;
    bufferedDurationMs_ -= queued.durationMs;
    if (bufferedDurationMs_ < 0) {
        bufferedDurationMs_ = 0;
    }
    wake_.notify_all();
    return queued.packet;
}

void PacketQueue::setBlocked(bool blocked)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocked_ = blocked;
    }
    wake_.notify_all();
}

void PacketQueue::setPaused(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = paused;
    }
    changed_.notify_all();
    wake_.notify_all();
}

void PacketQueue::clear()
{
    std::deque<QueuedPacket> oldPackets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        oldPackets.swap(packets_);
        bufferedBytes_ = 0;
        bufferedDurationMs_ = 0;
    }
    for (QueuedPacket& queued : oldPackets) {
        av_packet_free(&queued.packet);
    }
    changed_.notify_all();
    wake_.notify_all();
}

void PacketQueue::finish()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    changed_.notify_all();
    wake_.notify_all();
}

void PacketQueue::abort()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
    }
    changed_.notify_all();
    wake_.notify_all();
}

std::size_t PacketQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return packets_.size();
}

std::size_t PacketQueue::capacity() const
{
    return capacity_;
}

std::size_t PacketQueue::bufferedBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bufferedBytes_;
}

qint64 PacketQueue::bufferedDurationMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bufferedDurationMs_;
}

bool PacketQueue::isFinishedAndEmpty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_ && packets_.empty();
}

bool PacketQueue::isAborted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return aborted_;
}

bool PacketQueue::isPaused() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}


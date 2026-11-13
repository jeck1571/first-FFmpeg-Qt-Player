#pragma once

#include <QtGlobal>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
}

class PacketQueue
{
public:
    explicit PacketQueue(std::size_t capacity, std::size_t maxBytes = 0);
    ~PacketQueue();

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    bool push(AVPacket* packet, int timeoutMs = -1);
    // Takes ownership only when the packet is queued. A false return leaves
    // the packet with the caller so it can be retried or released explicitly.
    bool pushTimed(AVPacket* packet, qint64 durationMs, int timeoutMs = -1);
    bool tryPush(AVPacket* packet);
    AVPacket* pop(int timeoutMs = 50);

    void setBlocked(bool blocked);
    void setPaused(bool paused);
    void clear();
    void finish();
    void abort();

    std::size_t size() const;
    std::size_t capacity() const;
    std::size_t bufferedBytes() const;
    qint64 bufferedDurationMs() const;
    bool isFinishedAndEmpty() const;
    bool isAborted() const;
    bool isPaused() const;

private:
    struct QueuedPacket
    {
        AVPacket* packet = nullptr;
        qint64 durationMs = 0;
    };

    const std::size_t capacity_;
    const std::size_t maxBytes_;
    std::deque<QueuedPacket> packets_;
    std::size_t bufferedBytes_ = 0;
    qint64 bufferedDurationMs_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::condition_variable wake_;
    bool finished_ = false;
    bool aborted_ = false;
    bool blocked_ = false;
    bool paused_ = false;
};


#pragma once

#include <QImage>

#include <memory>
#include <mutex>
#include <vector>

class VideoFramePool final
{
public:
    explicit VideoFramePool(int slotCount = 3);

    QImage acquire(int width, int height, QImage::Format format);
    void reset();

    qint64 hits() const;
    qint64 misses() const;
    int slotCount() const;

private:
    struct Slot
    {
        std::shared_ptr<std::vector<uchar>> buffer;
    };

    const int slotCount_;
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    int width_ = 0;
    int height_ = 0;
    QImage::Format format_ = QImage::Format_Invalid;
    int nextSlot_ = 0;
    qint64 hits_ = 0;
    qint64 misses_ = 0;
};

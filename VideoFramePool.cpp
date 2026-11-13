#include "VideoFramePool.h"

#include <algorithm>

namespace {

void releasePooledBuffer(void* info)
{
    delete static_cast<std::shared_ptr<std::vector<uchar>>*>(info);
}

} // namespace

VideoFramePool::VideoFramePool(int slotCount)
    : slotCount_(std::max(1, slotCount))
{
}

QImage VideoFramePool::acquire(int width, int height, QImage::Format format)
{
    if (width <= 0 || height <= 0 || format == QImage::Format_Invalid) {
        return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (slots_.size() != static_cast<std::size_t>(slotCount_)
        || width_ != width
        || height_ != height
        || format_ != format) {
        slots_.clear();
        slots_.resize(static_cast<std::size_t>(slotCount_));
        width_ = width;
        height_ = height;
        format_ = format;
        nextSlot_ = 0;
    }

    Slot& slot = slots_.at(static_cast<std::size_t>(nextSlot_));
    nextSlot_ = (nextSlot_ + 1) % slotCount_;
    const qsizetype rowBytes = static_cast<qsizetype>(width_) * 4;
    const qsizetype byteCount = rowBytes * static_cast<qsizetype>(height_);
    if (!slot.buffer
        || slot.buffer->size() != static_cast<std::size_t>(byteCount)
        || slot.buffer.use_count() > 1) {
        slot.buffer = std::make_shared<std::vector<uchar>>(
            static_cast<std::size_t>(byteCount));
        ++misses_;
    } else {
        ++hits_;
    }

    auto* lifetime = new std::shared_ptr<std::vector<uchar>>(slot.buffer);
    QImage image(
        slot.buffer->data(),
        width_,
        height_,
        static_cast<int>(rowBytes),
        format_,
        &releasePooledBuffer,
        lifetime);
    if (image.isNull()) {
        delete lifetime;
        return {};
    }
    return image;
}

void VideoFramePool::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.clear();
    width_ = 0;
    height_ = 0;
    format_ = QImage::Format_Invalid;
    nextSlot_ = 0;
    hits_ = 0;
    misses_ = 0;
}

qint64 VideoFramePool::hits() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

qint64 VideoFramePool::misses() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_;
}

int VideoFramePool::slotCount() const
{
    return slotCount_;
}

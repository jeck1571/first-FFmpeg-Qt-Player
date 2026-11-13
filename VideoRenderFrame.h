#pragma once

#include <QSize>

#include <memory>

extern "C" {
struct AVFrame;
}

struct GpuVideoFrame
{
    std::shared_ptr<AVFrame> frame;
    void* texture = nullptr;
    int arrayIndex = 0;
    int arraySize = 1;
    QSize size;
    int pixelFormat = -1;
    bool fullRange = false;
    bool bt709 = true;

    bool isValid() const
    {
        return frame && texture && arrayIndex >= 0 && arrayIndex < arraySize
            && !size.isEmpty();
    }
};

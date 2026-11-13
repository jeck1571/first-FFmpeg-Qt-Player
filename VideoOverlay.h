#pragma once

#include <QWidget>

class VideoCanvas;

class VideoOverlay final : public QWidget
{
public:
    explicit VideoOverlay(VideoCanvas* canvas);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    VideoCanvas* canvas_ = nullptr;
};

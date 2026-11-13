#include "VideoOverlay.h"

#include "VideoCanvas.h"

#include <QPainter>
#include <QPaintEvent>

VideoOverlay::VideoOverlay(VideoCanvas* canvas)
    : QWidget(canvas)
    , canvas_(canvas)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
}

void VideoOverlay::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (canvas_) {
        canvas_->paintOverlay(painter);
    }
}

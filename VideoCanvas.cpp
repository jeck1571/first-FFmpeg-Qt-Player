#include "VideoCanvas.h"

#include "RhiVideoSurface.h"
#include "VideoOverlay.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QMimeData>
#include <QMutexLocker>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QThread>
#include <QTransform>
#include <QUrl>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <chrono>
#include <thread>

VideoCanvas::VideoCanvas(QWidget* parent)
    : QWidget(parent)
{
    setAcceptDrops(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(320, 180);
    setMouseTracking(true);
    placeholder_ = tr("Open media to begin");

    gpuSurface_ = new RhiVideoSurface(this);
    gpuSurface_->hide();
    overlay_ = new VideoOverlay(this);
    overlay_->setGeometry(rect());
    overlay_->show();
    overlay_->raise();

    connect(gpuSurface_, &RhiVideoSurface::d3d11DeviceReady,
            this, [this](void* device, void* context) {
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            d3d11Device_ = device;
            d3d11DeviceContext_ = context;
            renderBackend_ = QStringLiteral("d3d11-rhi-zero-copy");
        }
        deviceReadyCondition_.notify_all();
    });
    connect(gpuSurface_, &RhiVideoSurface::renderBackendFailed,
            this, [this](const QString& reason) {
        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            d3d11Device_ = nullptr;
            d3d11DeviceContext_ = nullptr;
            renderBackend_ = QStringLiteral("software-cpu");
        }
        deviceReadyCondition_.notify_all();
        {
            QMutexLocker locker(&mutex_);
            osdText_ = reason;
        }
        osdTimer_->start(4000);
        overlay_->update();
    });
    connect(gpuSurface_, &QRhiWidget::frameSubmitted, overlay_, qOverload<>(&QWidget::update));
    connect(gpuSurface_, &RhiVideoSurface::zeroCopyFramePresented, this, [this] {
        gpuFramesRendered_.fetch_add(1, std::memory_order_relaxed);
        if (overlay_) {
            overlay_->update();
        }
    });

    osdTimer_ = new QTimer(this);
    osdTimer_->setSingleShot(true);
    connect(osdTimer_, &QTimer::timeout, this, [this] {
        {
            QMutexLocker locker(&mutex_);
            osdText_.clear();
        }
        if (overlay_) {
            overlay_->update();
        }
        update();
    });

    resizeSettleTimer_ = new QTimer(this);
    resizeSettleTimer_->setSingleShot(true);
    resizeSettleTimer_->setInterval(120);
    connect(resizeSettleTimer_, &QTimer::timeout, this, [this] {
        smoothScaling_ = true;
        update();
    });
}

void VideoCanvas::scheduleRepaint()
{
    // Called from the decoder thread; keep at most one queued update in flight
    // so a fast frame rate cannot flood the GUI event queue while resizing.
    if (!repaintQueued_.exchange(true)) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

void VideoCanvas::setFrame(const QImage& frame)
{
    {
        QMutexLocker locker(&mutex_);
        frame_ = frame;
        sourceSize_ = frame.size();
        gpuFrameActive_ = false;
        orientedValid_ = false;
    }
    QMetaObject::invokeMethod(this, [this] {
        if (gpuSurface_) {
            gpuSurface_->clearVideoFrame();
            gpuSurface_->hide();
        }
        updateSurfaceGeometry();
        if (overlay_) {
            overlay_->update();
        }
    }, Qt::QueuedConnection);
    scheduleRepaint();
}

void VideoCanvas::setHardwareFrame(GpuVideoFrame frame)
{
    QMetaObject::invokeMethod(this, [this, frame = std::move(frame)]() mutable {
        if (!frame.isValid() || !gpuSurface_) {
            return;
        }
        {
            QMutexLocker locker(&mutex_);
            sourceSize_ = frame.size;
            gpuFrameActive_ = true;
            frame_ = QImage();
            oriented_ = QImage();
            orientedValid_ = false;
        }
        gpuSurface_->setRotationDegrees(rotation());
        gpuSurface_->show();
        gpuSurface_->raise();
        if (overlay_) {
            overlay_->show();
            overlay_->raise();
        }
        updateSurfaceGeometry();
        gpuSurface_->setVideoFrame(std::move(frame));
        update();
        if (overlay_) {
            overlay_->update();
        }
    }, Qt::QueuedConnection);
}

void VideoCanvas::clearFrame()
{
    {
        QMutexLocker locker(&mutex_);
        frame_ = QImage();
        oriented_ = QImage();
        orientedValid_ = false;
        subtitleText_.clear();
        gpuFrameActive_ = false;
        sourceSize_ = {};
    }
    if (gpuSurface_) {
        gpuSurface_->clearVideoFrame();
        gpuSurface_->hide();
    }
    if (overlay_) {
        overlay_->update();
    }
    update();
}

void VideoCanvas::prepareGpuRenderer()
{
    auto prepare = [this] {
        if (!gpuSurface_) {
            return;
        }
        gpuSurface_->show();
        updateSurfaceGeometry();
        if (overlay_) {
            overlay_->show();
            overlay_->raise();
        }
    };
    if (QThread::currentThread() == thread()) {
        prepare();
    } else {
        QMetaObject::invokeMethod(this, prepare, Qt::BlockingQueuedConnection);
    }
}

bool VideoCanvas::hasD3d11Device() const
{
    if (gpuSurface_
        && gpuSurface_->hasD3d11Device()
        && gpuSurface_->d3d11DeviceContext()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(deviceMutex_);
    return d3d11Device_ && d3d11DeviceContext_;
}

void* VideoCanvas::d3d11Device() const
{
    if (gpuSurface_ && gpuSurface_->hasD3d11Device()) {
        return gpuSurface_->d3d11Device();
    }
    std::lock_guard<std::mutex> lock(deviceMutex_);
    return d3d11Device_;
}

void* VideoCanvas::d3d11DeviceContext() const
{
    if (gpuSurface_ && gpuSurface_->hasD3d11Device()) {
        return gpuSurface_->d3d11DeviceContext();
    }
    std::lock_guard<std::mutex> lock(deviceMutex_);
    return d3d11DeviceContext_;
}

bool VideoCanvas::waitForD3d11Device(int timeoutMs) const
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(0, timeoutMs));
    do {
        if (hasD3d11Device()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return hasD3d11Device();
}

bool VideoCanvas::gpuRenderReady() const
{
    return gpuSurface_ && gpuSurface_->isRenderReady();
}

qint64 VideoCanvas::gpuFramesRendered() const
{
    return gpuFramesRendered_.load(std::memory_order_relaxed);
}

QString VideoCanvas::renderBackend() const
{
    if (gpuSurface_ && gpuSurface_->isRenderReady()) {
        return gpuSurface_->renderBackendName();
    }
    std::lock_guard<std::mutex> lock(deviceMutex_);
    return renderBackend_;
}

void VideoCanvas::setRotation(int degrees)
{
    degrees %= 360;
    if (degrees < 0) {
        degrees += 360;
    }
    {
        QMutexLocker locker(&mutex_);
        if (rotation_ == degrees) {
            return;
        }
        rotation_ = degrees;
        orientedValid_ = false;
    }
    if (gpuSurface_) {
        gpuSurface_->setRotationDegrees(degrees);
    }
    updateSurfaceGeometry();
    if (overlay_) {
        overlay_->update();
    }
    update();
}

int VideoCanvas::rotation() const
{
    QMutexLocker locker(&mutex_);
    return rotation_;
}

void VideoCanvas::setFitMode(FitMode mode)
{
    {
        QMutexLocker locker(&mutex_);
        if (fitMode_ == mode) {
            return;
        }
        fitMode_ = mode;
    }
    updateSurfaceGeometry();
    if (overlay_) {
        overlay_->update();
    }
    update();
}

VideoCanvas::FitMode VideoCanvas::fitMode() const
{
    QMutexLocker locker(&mutex_);
    return fitMode_;
}

void VideoCanvas::setSubtitleText(const QString& text)
{
    {
        QMutexLocker locker(&mutex_);
        if (subtitleText_ == text) {
            return;
        }
        subtitleText_ = text;
    }
    if (overlay_) {
        overlay_->update();
    }
    update();
}

void VideoCanvas::setSubtitleScale(double scale)
{
    scale = std::clamp(scale, 0.5, 2.5);
    {
        QMutexLocker locker(&mutex_);
        if (qFuzzyCompare(subtitleScale_, scale)) {
            return;
        }
        subtitleScale_ = scale;
    }
    if (overlay_) {
        overlay_->update();
    }
    update();
}

void VideoCanvas::setSubtitleBottomMargin(double margin)
{
    margin = std::clamp(margin, 0.02, 0.45);
    {
        QMutexLocker locker(&mutex_);
        if (qFuzzyCompare(subtitleBottomMargin_, margin)) {
            return;
        }
        subtitleBottomMargin_ = margin;
    }
    if (overlay_) {
        overlay_->update();
    }
    update();
}

void VideoCanvas::setSubtitleStyle(const SubtitleStyle& style)
{
    {
        QMutexLocker locker(&mutex_);
        subtitleStyle_ = style;
    }
    if (overlay_) {
        overlay_->update();
    }
    update();
}

VideoCanvas::SubtitleStyle VideoCanvas::subtitleStyle() const
{
    QMutexLocker locker(&mutex_);
    return subtitleStyle_;
}

void VideoCanvas::setPlaceholderText(const QString& text)
{
    {
        QMutexLocker locker(&mutex_);
        if (placeholder_ == text) {
            return;
        }
        placeholder_ = text;
    }
    if (overlay_) {
        overlay_->update();
    }
    update();
}

void VideoCanvas::showOsd(const QString& text, int durationMs)
{
    {
        QMutexLocker locker(&mutex_);
        osdText_ = text;
    }
    osdTimer_->start(qMax(400, durationMs));
    if (overlay_) {
        overlay_->update();
    }
    update();
}

QSize VideoCanvas::displaySizeLocked() const
{
    if (sourceSize_.isEmpty()) {
        return {};
    }
    if (rotation_ % 180 == 0) {
        return sourceSize_;
    }
    return sourceSize_.transposed();
}

void VideoCanvas::updateSurfaceGeometry()
{
    if (!gpuSurface_) {
        return;
    }
    QSize source;
    FitMode mode = FitMode::Fit;
    {
        QMutexLocker locker(&mutex_);
        source = displaySizeLocked();
        mode = fitMode_;
    }
    if (source.isEmpty()) {
        gpuSurface_->setGeometry(rect());
        return;
    }
    const QRectF target = targetRectFor(source, size(), mode);
    gpuSurface_->setGeometry(target.toAlignedRect());
}

QImage VideoCanvas::orientedFrameLocked() const
{
    if (frame_.isNull()) {
        return {};
    }
    if (rotation_ % 360 == 0) {
        return frame_;
    }
    if (!orientedValid_ || oriented_.isNull()) {
        // FFmpeg reports the display matrix rotation counter-clockwise,
        // while QTransform::rotate() is clockwise for positive angles.
        QTransform transform;
        transform.rotate(-rotation_);
        oriented_ = frame_.transformed(transform, Qt::SmoothTransformation);
        orientedValid_ = true;
    }
    return oriented_;
}

QImage VideoCanvas::snapshot(bool withSubtitles) const
{
    QImage image;
    QString subtitle;
    SubtitleStyle style;
    double scale = 1.0;
    double margin = subtitleBottomMargin_;
    bool useGpuSnapshot = false;
    {
        QMutexLocker locker(&mutex_);
        useGpuSnapshot = gpuFrameActive_;
        if (!useGpuSnapshot) {
            image = orientedFrameLocked().copy();
        }
        if (withSubtitles) {
            subtitle = subtitleText_;
            style = subtitleStyle_;
            scale = subtitleScale_;
            margin = subtitleBottomMargin_;
        }
    }
    if (useGpuSnapshot && gpuSurface_) {
        image = gpuSurface_->snapshot();
    }
    if (image.isNull() || subtitle.isEmpty()) {
        return image;
    }

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawSubtitle(painter, subtitle, QRectF(image.rect()), style, scale, margin);
    painter.end();
    return image;
}

QRectF VideoCanvas::targetRectFor(const QSize& source, const QSize& viewport, FitMode mode)
{
    if (source.isEmpty() || viewport.isEmpty()) {
        return QRectF();
    }

    const QSizeF sourceSize(source);
    const QSizeF viewportSize(viewport);
    QSizeF drawSize = sourceSize;

    switch (mode) {
    case FitMode::Fit:
        drawSize = sourceSize.scaled(viewportSize, Qt::KeepAspectRatio);
        break;
    case FitMode::Fill:
        drawSize = sourceSize.scaled(viewportSize, Qt::KeepAspectRatioByExpanding);
        break;
    case FitMode::Stretch:
        drawSize = viewportSize;
        break;
    case FitMode::Ratio16x9:
    case FitMode::Ratio4x3: {
        const double ratio = mode == FitMode::Ratio16x9 ? 16.0 / 9.0 : 4.0 / 3.0;
        double width = viewportSize.width();
        double height = width / ratio;
        if (height > viewportSize.height()) {
            height = viewportSize.height();
            width = height * ratio;
        }
        drawSize = QSizeF(width, height);
        break;
    }
    case FitMode::Original:
        drawSize = sourceSize;
        break;
    }

    // Snap to whole device-independent pixels so letterbox bars keep an even
    // width and the frame edges stay crisp while the window is being resized.
    return QRectF(qRound((viewportSize.width() - drawSize.width()) / 2.0),
                  qRound((viewportSize.height() - drawSize.height()) / 2.0),
                  qRound(drawSize.width()),
                  qRound(drawSize.height()));
}

void VideoCanvas::drawSubtitle(QPainter& painter, const QString& text, const QRectF& videoRect,
                               const SubtitleStyle& style, double scale, double bottomMargin)
{
    if (text.isEmpty() || videoRect.isEmpty()) {
        return;
    }

    const int baseSize = static_cast<int>(videoRect.width() / 42.0 * scale);
    QFont subtitleFont(style.fontFamily.isEmpty() ? QStringLiteral("Segoe UI")
                                                  : style.fontFamily);
    subtitleFont.setPixelSize(std::clamp(baseSize, 14, 72));
    subtitleFont.setWeight(QFont::DemiBold);
    painter.setFont(subtitleFont);

    const QFontMetrics metrics(subtitleFont);
    const int maxTextWidth = qMax(120, static_cast<int>(videoRect.width()) - 72);
    const QRect textBounds = metrics.boundingRect(
        QRect(0, 0, maxTextWidth, static_cast<int>(videoRect.height())),
        Qt::AlignHCenter | Qt::TextWordWrap,
        text);
    const int boxWidth = qMin(maxTextWidth, textBounds.width() + 28);
    const int boxHeight = textBounds.height() + 16;
    const int margin = std::clamp(
        static_cast<int>(videoRect.height() * bottomMargin),
        8,
        qMax(8, static_cast<int>(videoRect.height()) / 2));

    const double boxLeft = qBound(videoRect.left() + 12.0,
                                  videoRect.center().x() - boxWidth / 2.0,
                                  qMax(videoRect.left() + 12.0,
                                       videoRect.right() - boxWidth - 12.0));
    const double boxTop = qMax(videoRect.top() + 12.0,
                               videoRect.bottom() - boxHeight - margin);
    const QRectF box(boxLeft, boxTop, boxWidth, boxHeight);

    if (style.backgroundOpacity > 0) {
        QColor background = style.backgroundColor;
        background.setAlpha(std::clamp(style.backgroundOpacity, 0, 255));
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(box, 4, 4);
    }

    const QRectF textRect = box.adjusted(14, 8, -14, -8);
    const int outline = std::clamp(style.outlineWidth, 0, 8);
    if (outline > 0) {
        painter.setPen(style.outlineColor);
        for (int dx = -outline; dx <= outline; dx += outline) {
            for (int dy = -outline; dy <= outline; dy += outline) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                painter.drawText(textRect.translated(dx, dy),
                                 Qt::AlignCenter | Qt::TextWordWrap,
                                 text);
            }
        }
    }
    painter.setPen(style.textColor);
    painter.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, text);
}

void VideoCanvas::paintVideo(QPainter& painter)
{
    repaintQueued_.store(false);
    painter.fillRect(rect(), QColor(7, 8, 10));

    QImage frame;
    QSize source;
    FitMode fitMode = FitMode::Fit;
    bool gpuActive = false;
    {
        QMutexLocker locker(&mutex_);
        if (!gpuFrameActive_) {
            frame = orientedFrameLocked();
        }
        source = displaySizeLocked();
        fitMode = fitMode_;
        gpuActive = gpuFrameActive_;
    }
    if (gpuActive || frame.isNull()) {
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, smoothScaling_);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF targetRect = targetRectFor(source, size(), fitMode);
    painter.drawImage(targetRect, frame);
}

void VideoCanvas::paintOverlay(QPainter& painter)
{
    QString subtitle;
    QString placeholder;
    QString osd;
    FitMode fitMode = FitMode::Fit;
    SubtitleStyle subtitleStyle;
    QSize source;
    double subtitleScale = 1.0;
    double subtitleMargin = subtitleBottomMargin_;
    bool hasFrame = false;
    {
        QMutexLocker locker(&mutex_);
        subtitle = subtitleText_;
        subtitleStyle = subtitleStyle_;
        placeholder = placeholder_;
        osd = osdText_;
        fitMode = fitMode_;
        source = displaySizeLocked();
        subtitleScale = subtitleScale_;
        subtitleMargin = subtitleBottomMargin_;
        hasFrame = gpuFrameActive_ || !frame_.isNull();
    }

    if (!hasFrame) {
        painter.setPen(QColor(128, 132, 141));
        QFont font = painter.font();
        font.setPixelSize(16);
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, placeholder);
        return;
    }

    QRectF subtitleArea = targetRectFor(source, size(), fitMode)
        .intersected(QRectF(rect()));
    if (subtitleArea.isEmpty()) {
        subtitleArea = QRectF(rect());
    }
    drawSubtitle(
        painter, subtitle, subtitleArea, subtitleStyle,
        subtitleScale, subtitleMargin);

    if (!osd.isEmpty()) {
        QFont osdFont(QStringLiteral("Segoe UI"));
        osdFont.setPixelSize(15);
        osdFont.setWeight(QFont::DemiBold);
        painter.setFont(osdFont);
        const QFontMetrics metrics(osdFont);
        const QRect textBounds = metrics.boundingRect(osd);
        const QRectF box(width() - textBounds.width() - 44.0,
                         18.0,
                         textBounds.width() + 28.0,
                         textBounds.height() + 16.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 175));
        painter.drawRoundedRect(box, 5, 5);
        painter.setPen(QColor(255, 255, 255, 235));
        painter.drawText(box, Qt::AlignCenter, osd);
    }
}

void VideoCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    paintVideo(painter);
}

void VideoCanvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // Qt only repaints the exposed strip when a widget grows unless the whole
    // surface is marked dirty; with an opaque canvas that leaves stale video
    // pixels around the edges, so always repaint the full viewport.
    smoothScaling_ = false;
    if (resizeSettleTimer_) {
        resizeSettleTimer_->start();
    }
    if (overlay_) {
        overlay_->setGeometry(rect());
    }
    updateSurfaceGeometry();
    if (overlay_) {
        overlay_->raise();
        overlay_->update();
    }
    update();
}

void VideoCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        emit doubleClicked();
    }
    QWidget::mouseDoubleClickEvent(event);
}

void VideoCanvas::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (delta != 0) {
        int steps = delta / 120;
        if (steps == 0) {
            steps = delta > 0 ? 1 : -1;
        }
        emit volumeWheelSteps(steps);
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void VideoCanvas::contextMenuEvent(QContextMenuEvent* event)
{
    emit contextMenuRequested(event->globalPos());
    event->accept();
}

void VideoCanvas::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void VideoCanvas::dropEvent(QDropEvent* event)
{
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    if (!paths.isEmpty()) {
        emit mediaDropped(paths);
        event->acceptProposedAction();
    }
}

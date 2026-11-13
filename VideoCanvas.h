#pragma once

#include "VideoRenderFrame.h"

#include <QColor>
#include <QImage>
#include <QMutex>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <atomic>
#include <condition_variable>

class QContextMenuEvent;
class QPainter;
class QResizeEvent;
class QTimer;
class QWheelEvent;
class RhiVideoSurface;
class VideoOverlay;

class VideoCanvas final : public QWidget
{
    Q_OBJECT

public:
    enum class FitMode {
        Fit,
        Fill,
        Stretch,
        Ratio16x9,
        Ratio4x3,
        Original
    };
    Q_ENUM(FitMode)

    struct SubtitleStyle
    {
        QString fontFamily;
        QColor textColor{255, 255, 255};
        QColor outlineColor{0, 0, 0};
        int outlineWidth = 0;
        QColor backgroundColor{0, 0, 0};
        int backgroundOpacity = 180;
    };

    explicit VideoCanvas(QWidget* parent = nullptr);

    void setFrame(const QImage& frame);
    void setHardwareFrame(GpuVideoFrame frame);
    void clearFrame();
    void prepareGpuRenderer();
    bool hasD3d11Device() const;
    void* d3d11Device() const;
    void* d3d11DeviceContext() const;
    bool waitForD3d11Device(int timeoutMs) const;
    bool gpuRenderReady() const;
    qint64 gpuFramesRendered() const;
    QString renderBackend() const;
    void setRotation(int degrees);
    int rotation() const;
    void setFitMode(FitMode mode);
    FitMode fitMode() const;
    void setSubtitleText(const QString& text);
    void setSubtitleScale(double scale);
    void setSubtitleBottomMargin(double margin);
    void setSubtitleStyle(const SubtitleStyle& style);
    SubtitleStyle subtitleStyle() const;
    void setPlaceholderText(const QString& text);
    void showOsd(const QString& text, int durationMs = 1500);
    QImage snapshot(bool withSubtitles = false) const;

signals:
    void doubleClicked();
    void mediaDropped(const QStringList& paths);
    void volumeWheelSteps(int steps);
    void contextMenuRequested(const QPoint& globalPosition);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    friend class VideoOverlay;

    void scheduleRepaint();
    QImage orientedFrameLocked() const;
    QSize displaySizeLocked() const;
    void updateSurfaceGeometry();
    void paintVideo(QPainter& painter);
    void paintOverlay(QPainter& painter);
    static QRectF targetRectFor(const QSize& source, const QSize& viewport, FitMode mode);
    static void drawSubtitle(QPainter& painter, const QString& text, const QRectF& videoRect,
                             const SubtitleStyle& style, double scale, double bottomMargin);

    mutable QMutex mutex_;
    QImage frame_;
    mutable QImage oriented_;
    mutable bool orientedValid_ = false;
    int rotation_ = 0;
    FitMode fitMode_ = FitMode::Fit;
    QString subtitleText_;
    SubtitleStyle subtitleStyle_;
    double subtitleScale_ = 1.0;
    double subtitleBottomMargin_ = 0.055;
    QString placeholder_;
    QString osdText_;
    QTimer* osdTimer_ = nullptr;
    QTimer* resizeSettleTimer_ = nullptr;
    RhiVideoSurface* gpuSurface_ = nullptr;
    VideoOverlay* overlay_ = nullptr;
    std::atomic<bool> repaintQueued_{false};
    std::atomic<qint64> gpuFramesRendered_{0};
    bool smoothScaling_ = true;
    bool gpuFrameActive_ = false;
    QSize sourceSize_;
    mutable std::mutex deviceMutex_;
    mutable std::condition_variable deviceReadyCondition_;
    void* d3d11Device_ = nullptr;
    void* d3d11DeviceContext_ = nullptr;
    QString renderBackend_ = QStringLiteral("software-cpu");
};

#pragma once

#include "VideoRenderFrame.h"

#include <QColor>
#include <QImage>
#include <QRhiWidget>
#include <QSize>

#include <atomic>
#include <deque>
#include <memory>
#include <vector>

class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QPaintEvent;
class QShader;
struct ID3D11VideoContext;
struct ID3D11VideoDevice;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessorInputView;
struct ID3D11VideoProcessorOutputView;

class RhiVideoSurface final : public QRhiWidget
{
    Q_OBJECT

public:
    explicit RhiVideoSurface(QWidget* parent = nullptr);
    ~RhiVideoSurface() override;

    bool hasD3d11Device() const;
    void* d3d11Device() const;
    void* d3d11DeviceContext() const;
    bool isRenderReady() const;
    QString renderBackendName() const;

    void setVideoFrame(GpuVideoFrame frame);
    void clearVideoFrame();
    void setRotationDegrees(int degrees);
    QImage snapshot() const;

signals:
    void d3d11DeviceReady(void* device, void* context);
    void zeroCopyFramePresented();
    void renderBackendFailed(const QString& reason);

protected:
    void paintEvent(QPaintEvent* event) override;
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    struct InputView
    {
        void* texture = nullptr;
        int arrayIndex = -1;
        ID3D11VideoProcessorInputView* view = nullptr;
    };

    bool ensurePipeline();
    bool ensureVideoProcessor();
    bool blitVideoFrame();
    ID3D11VideoProcessorInputView* inputViewForFrame();
    void clearInputViews();
    void releaseVideoProcessor();
    void releasePipeline();
    static QShader loadShader(const QString& path);

    std::atomic<void*> device_{nullptr};
    std::atomic<void*> deviceContext_{nullptr};
    std::atomic<bool> renderReady_{false};
    std::atomic<int> rotationDegrees_{0};

    std::unique_ptr<QRhiBuffer> uniformBuffer_;
    std::unique_ptr<QRhiSampler> sampler_;
    std::unique_ptr<QRhiShaderResourceBindings> shaderResources_;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
    std::unique_ptr<QRhiTexture> dummyTexture_;
    std::unique_ptr<QRhiTexture> videoTexture_;

    GpuVideoFrame pendingFrame_;
    GpuVideoFrame currentFrame_;
    std::deque<GpuVideoFrame> retiredFrames_;
    ID3D11VideoDevice* videoDevice_ = nullptr;
    ID3D11VideoContext* videoContext_ = nullptr;
    ID3D11VideoProcessorEnumerator* videoProcessorEnumerator_ = nullptr;
    ID3D11VideoProcessor* videoProcessor_ = nullptr;
    ID3D11VideoProcessorOutputView* outputView_ = nullptr;
    std::vector<InputView> inputViews_;
    int processorPixelFormat_ = -1;
    QSize processorSize_;
    bool backendFailureReported_ = false;
};

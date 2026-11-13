#include "RhiVideoSurface.h"

#include "D3d11ContextSync.h"

#include <QFile>
#include <QDebug>
#include <QPaintEvent>
#include <QPixmap>
#include <QScreen>
#include <QVector4D>
#include <rhi/qrhi.h>

#include <d3d11.h>

#include <algorithm>

namespace {

QShader loadShaderFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

} // namespace

RhiVideoSurface::RhiVideoSurface(QWidget* parent)
    : QRhiWidget(parent)
{
    setApi(Api::Direct3D11);
    setSampleCount(1);
    setColorBufferFormat(TextureFormat::RGBA8);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TransparentForMouseEvents);

    connect(this, &QRhiWidget::renderFailed, this, [this] {
        qWarning() << "RhiVideoSurface: QRhiWidget renderFailed";
        renderReady_.store(false, std::memory_order_release);
        emit renderBackendFailed(tr("Qt RHI failed to initialize the D3D11 render target."));
    });
}

RhiVideoSurface::~RhiVideoSurface() = default;

bool RhiVideoSurface::hasD3d11Device() const
{
    return device_.load(std::memory_order_acquire) != nullptr;
}

void* RhiVideoSurface::d3d11Device() const
{
    return device_.load(std::memory_order_acquire);
}

void* RhiVideoSurface::d3d11DeviceContext() const
{
    return deviceContext_.load(std::memory_order_acquire);
}

bool RhiVideoSurface::isRenderReady() const
{
    return renderReady_.load(std::memory_order_acquire);
}

QString RhiVideoSurface::renderBackendName() const
{
    return QStringLiteral("d3d11-rhi-zero-copy");
}

void RhiVideoSurface::paintEvent(QPaintEvent* event)
{
    // D3D11VA and Qt RHI share an immediate context. Keep decoding, video
    // processing, command submission, and presentation from overlapping.
    std::lock_guard<std::recursive_mutex> lock(D3d11ContextSync::mutex());
    QRhiWidget::paintEvent(event);
}

QShader RhiVideoSurface::loadShader(const QString& path)
{
    return loadShaderFile(path);
}

void RhiVideoSurface::initialize(QRhiCommandBuffer* cb)
{
    Q_UNUSED(cb)

    qInfo() << "RhiVideoSurface: initialize";
    const auto* d3d11 = static_cast<const QRhiD3D11NativeHandles*>(
        rhi()->nativeHandles());
    if (!d3d11 || !d3d11->dev || !d3d11->context) {
        qWarning() << "RhiVideoSurface: no native D3D11 device";
        emit renderBackendFailed(tr("Qt RHI did not expose a D3D11 device."));
        return;
    }

    device_.store(d3d11->dev, std::memory_order_release);
    deviceContext_.store(d3d11->context, std::memory_order_release);
    if (!D3d11ContextSync::enableMultithreadProtection(d3d11->dev)) {
        qWarning() << "RhiVideoSurface: D3D11 multithread protection unavailable";
    }
    if (!ensurePipeline()) {
        qWarning() << "RhiVideoSurface: pipeline creation failed";
        emit renderBackendFailed(tr("Cannot create the zero-copy video pipeline."));
        return;
    }

    renderReady_.store(true, std::memory_order_release);
    qInfo() << "RhiVideoSurface: render ready";
    emit d3d11DeviceReady(d3d11->dev, d3d11->context);
}

bool RhiVideoSurface::ensurePipeline()
{
    if (pipeline_) {
        return true;
    }

    const QShader vertexShader = loadShader(QStringLiteral(":/shader_assets/video.vert.qsb"));
    const QShader fragmentShader = loadShader(QStringLiteral(":/shader_assets/video.frag.qsb"));
    if (!vertexShader.isValid() || !fragmentShader.isValid()) {
        qWarning() << "RhiVideoSurface: invalid shader"
                   << vertexShader.isValid() << fragmentShader.isValid();
        return false;
    }
    qInfo() << "RhiVideoSurface: shader stages"
            << vertexShader.availableShaders()
            << fragmentShader.availableShaders();

    uniformBuffer_.reset(rhi()->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
    if (!uniformBuffer_ || !uniformBuffer_->create()) {
        qWarning() << "RhiVideoSurface: uniform buffer creation failed";
        return false;
    }

    sampler_.reset(rhi()->newSampler(
        QRhiSampler::Linear,
        QRhiSampler::Linear,
        QRhiSampler::None,
        QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge));
    if (!sampler_ || !sampler_->create()) {
        qWarning() << "RhiVideoSurface: sampler creation failed";
        return false;
    }

    dummyTexture_.reset(rhi()->newTexture(
        QRhiTexture::BGRA8, QSize(1, 1)));
    if (!dummyTexture_ || !dummyTexture_->create()) {
        qWarning() << "RhiVideoSurface: dummy texture creation failed";
        return false;
    }

    shaderResources_.reset(rhi()->newShaderResourceBindings());
    if (!shaderResources_) {
        return false;
    }
    shaderResources_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0,
            QRhiShaderResourceBinding::VertexStage
                | QRhiShaderResourceBinding::FragmentStage,
            uniformBuffer_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            dummyTexture_.get(), sampler_.get())
    });
    if (!shaderResources_->create()) {
        qWarning() << "RhiVideoSurface: shader resources creation failed";
        return false;
    }

    pipeline_.reset(rhi()->newGraphicsPipeline());
    pipeline_->setShaderStages({
        { QRhiShaderStage::Vertex, vertexShader },
        { QRhiShaderStage::Fragment, fragmentShader }
    });
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline_->setCullMode(QRhiGraphicsPipeline::None);
    pipeline_->setShaderResourceBindings(shaderResources_.get());
    pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline_->create()) {
        qWarning() << "RhiVideoSurface: graphics pipeline create failed";
        releasePipeline();
        return false;
    }
    return true;
}

bool RhiVideoSurface::ensureVideoProcessor()
{
    if (!currentFrame_.isValid()) {
        return false;
    }
    if (videoProcessor_ && outputView_ && videoTexture_
        && processorPixelFormat_ == currentFrame_.pixelFormat
        && processorSize_ == currentFrame_.size) {
        return true;
    }

    releaseVideoProcessor();
    auto* device = static_cast<ID3D11Device*>(device_.load(std::memory_order_acquire));
    auto* context = static_cast<ID3D11DeviceContext*>(
        deviceContext_.load(std::memory_order_acquire));
    auto* sourceTexture = static_cast<ID3D11Texture2D*>(currentFrame_.texture);
    if (!device || !context || !sourceTexture) {
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDescription{};
    sourceTexture->GetDesc(&sourceDescription);
    if (sourceDescription.Format != DXGI_FORMAT_NV12
        && sourceDescription.Format != DXGI_FORMAT_P010) {
        return false;
    }

    HRESULT result = device->QueryInterface(
        __uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&videoDevice_));
    if (FAILED(result)) {
        return false;
    }
    result = context->QueryInterface(
        __uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&videoContext_));
    if (FAILED(result)) {
        releaseVideoProcessor();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDescription{};
    contentDescription.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDescription.InputFrameRate = {30, 1};
    contentDescription.InputWidth = static_cast<UINT>(currentFrame_.size.width());
    contentDescription.InputHeight = static_cast<UINT>(currentFrame_.size.height());
    contentDescription.OutputFrameRate = {30, 1};
    contentDescription.OutputWidth = static_cast<UINT>(currentFrame_.size.width());
    contentDescription.OutputHeight = static_cast<UINT>(currentFrame_.size.height());
    contentDescription.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    result = videoDevice_->CreateVideoProcessorEnumerator(
        &contentDescription, &videoProcessorEnumerator_);
    if (FAILED(result)) {
        releaseVideoProcessor();
        return false;
    }

    UINT inputFlags = 0;
    UINT outputFlags = 0;
    if (FAILED(videoProcessorEnumerator_->CheckVideoProcessorFormat(
            sourceDescription.Format, &inputFlags))
        || !(inputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)
        || FAILED(videoProcessorEnumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &outputFlags))
        || !(outputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        releaseVideoProcessor();
        return false;
    }

    result = videoDevice_->CreateVideoProcessor(
        videoProcessorEnumerator_, 0, &videoProcessor_);
    if (FAILED(result)) {
        releaseVideoProcessor();
        return false;
    }
    videoContext_->VideoProcessorSetStreamFrameFormat(
        videoProcessor_, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetOutputTargetRect(
        videoProcessor_, TRUE, nullptr);

    videoTexture_.reset(rhi()->newTexture(
        QRhiTexture::BGRA8,
        currentFrame_.size,
        1,
        QRhiTexture::RenderTarget));
    if (!videoTexture_ || !videoTexture_->create()) {
        releaseVideoProcessor();
        return false;
    }
    auto* outputTexture = reinterpret_cast<ID3D11Texture2D*>(
        videoTexture_->nativeTexture().object);
    if (!outputTexture) {
        releaseVideoProcessor();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDescription{};
    outputDescription.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDescription.Texture2D.MipSlice = 0;
    result = videoDevice_->CreateVideoProcessorOutputView(
        outputTexture,
        videoProcessorEnumerator_,
        &outputDescription,
        &outputView_);
    if (FAILED(result)) {
        releaseVideoProcessor();
        return false;
    }

    shaderResources_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0,
            QRhiShaderResourceBinding::VertexStage
                | QRhiShaderResourceBinding::FragmentStage,
            uniformBuffer_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            videoTexture_.get(), sampler_.get())
    });
    shaderResources_->updateResources(
        QRhiShaderResourceBindings::BindingsAreSorted);

    processorPixelFormat_ = currentFrame_.pixelFormat;
    processorSize_ = currentFrame_.size;
    return true;
}

ID3D11VideoProcessorInputView* RhiVideoSurface::inputViewForFrame()
{
    for (const InputView& cached : inputViews_) {
        if (cached.texture == currentFrame_.texture
            && cached.arrayIndex == currentFrame_.arrayIndex) {
            return cached.view;
        }
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC description{};
    description.FourCC = 0;
    description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    description.Texture2D.MipSlice = 0;
    description.Texture2D.ArraySlice = static_cast<UINT>(currentFrame_.arrayIndex);

    ID3D11VideoProcessorInputView* view = nullptr;
    const HRESULT result = videoDevice_->CreateVideoProcessorInputView(
        static_cast<ID3D11Texture2D*>(currentFrame_.texture),
        videoProcessorEnumerator_,
        &description,
        &view);
    if (FAILED(result)) {
        return nullptr;
    }
    inputViews_.push_back({currentFrame_.texture, currentFrame_.arrayIndex, view});
    return view;
}

bool RhiVideoSurface::blitVideoFrame()
{
    ID3D11VideoProcessorInputView* inputView = inputViewForFrame();
    if (!inputView || !videoContext_ || !videoProcessor_ || !outputView_) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.pInputSurface = inputView;
    const HRESULT result = videoContext_->VideoProcessorBlt(
        videoProcessor_, outputView_, 0, 1, &stream);
    return SUCCEEDED(result);
}

void RhiVideoSurface::render(QRhiCommandBuffer* cb)
{
    if (!renderReady_.load(std::memory_order_acquire)) {
        return;
    }
    if (pendingFrame_.isValid()) {
        if (currentFrame_.isValid()) {
            retiredFrames_.push_back(std::move(currentFrame_));
        }
        currentFrame_ = std::move(pendingFrame_);
        while (retiredFrames_.size() > 4) {
            retiredFrames_.pop_front();
        }
    }

    QRhiResourceUpdateBatch* updates = rhi()->nextResourceUpdateBatch();
    const QColor clearColor(0, 0, 0, 255);
    if (!currentFrame_.isValid()) {
        cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 }, updates);
        cb->endPass();
        return;
    }
    if (!ensureVideoProcessor() || !blitVideoFrame()) {
        renderReady_.store(false, std::memory_order_release);
        if (!backendFailureReported_) {
            backendFailureReported_ = true;
            emit renderBackendFailed(
                tr("D3D11 video processing is unavailable; using CPU rendering."));
        }
        cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 }, updates);
        cb->endPass();
        return;
    }

    const int quarterTurns = ((rotationDegrees_.load(std::memory_order_acquire) % 360) + 360) % 360 / 90;
    const float values[4] = {
        0.0f,
        static_cast<float>(quarterTurns),
        0.0f,
        0.0f
    };
    updates->updateDynamicBuffer(
        uniformBuffer_.get(),
        0,
        QByteArray(reinterpret_cast<const char*>(values), sizeof(values)));

    cb->beginPass(renderTarget(), clearColor, { 1.0f, 0 }, updates);
    cb->setGraphicsPipeline(pipeline_.get());
    const QSize outputSize = renderTarget()->pixelSize();
    cb->setViewport(QRhiViewport(
        0, 0, static_cast<float>(outputSize.width()),
        static_cast<float>(outputSize.height())));
    cb->setShaderResources(shaderResources_.get());
    cb->draw(4);
    cb->endPass();

    emit zeroCopyFramePresented();
}

void RhiVideoSurface::setVideoFrame(GpuVideoFrame frame)
{
    pendingFrame_ = std::move(frame);
    update();
}

void RhiVideoSurface::clearVideoFrame()
{
    pendingFrame_ = {};
    currentFrame_ = {};
    retiredFrames_.clear();
    update();
}

void RhiVideoSurface::setRotationDegrees(int degrees)
{
    rotationDegrees_.store(degrees, std::memory_order_release);
    update();
}

QImage RhiVideoSurface::snapshot() const
{
    QWidget* topLevel = window();
    QScreen* captureScreen = topLevel ? topLevel->screen() : nullptr;
    if (!topLevel || !captureScreen) {
        return {};
    }

    const QPixmap windowImage = captureScreen->grabWindow(topLevel->winId());
    if (windowImage.isNull()) {
        return {};
    }

    const QPoint topLeft = mapTo(topLevel, QPoint(0, 0));
    const qreal scale = topLevel->devicePixelRatioF();
    const QRect captureRect(
        qRound(topLeft.x() * scale),
        qRound(topLeft.y() * scale),
        qRound(width() * scale),
        qRound(height() * scale));
    const QImage image = windowImage.toImage();
    return image.copy(captureRect.intersected(image.rect()));
}

void RhiVideoSurface::clearInputViews()
{
    for (InputView& input : inputViews_) {
        if (input.view) {
            input.view->Release();
        }
    }
    inputViews_.clear();
}

void RhiVideoSurface::releaseVideoProcessor()
{
    clearInputViews();
    if (outputView_) {
        outputView_->Release();
        outputView_ = nullptr;
    }
    videoTexture_.reset();
    if (videoProcessor_) {
        videoProcessor_->Release();
        videoProcessor_ = nullptr;
    }
    if (videoProcessorEnumerator_) {
        videoProcessorEnumerator_->Release();
        videoProcessorEnumerator_ = nullptr;
    }
    if (videoContext_) {
        videoContext_->Release();
        videoContext_ = nullptr;
    }
    if (videoDevice_) {
        videoDevice_->Release();
        videoDevice_ = nullptr;
    }
    processorPixelFormat_ = -1;
    processorSize_ = {};
}

void RhiVideoSurface::releasePipeline()
{
    pipeline_.reset();
    shaderResources_.reset();
    dummyTexture_.reset();
    sampler_.reset();
    uniformBuffer_.reset();
}

void RhiVideoSurface::releaseResources()
{
    renderReady_.store(false, std::memory_order_release);
    releaseVideoProcessor();
    releasePipeline();
    currentFrame_ = {};
    pendingFrame_ = {};
    retiredFrames_.clear();
}

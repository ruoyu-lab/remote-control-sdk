// Qt Quick through QRhi (Metal on macOS and iOS, D3D11 on Windows): the
// frame's NV12 planes as two textures (native_planes), drawn as one quad by
// the demo's own shaders.
#include "native_planes.hpp"
#include "remote_video.hpp"
#include "video_node.hpp"

#include <QFile>
#include <QQuickWindow>
#include <rhi/qshader.h>

#include <deque>

namespace {

QShader loadShader(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader();
}

// Draws the current frame's two planes as one textured quad.
class VideoNode final : public FrameNode {
public:
    VideoNode(QQuickWindow* window, RemoteVideo* item) : window_(window), item_(item) {}
    ~VideoNode() override { releaseResources(); }

    // Synchronization (GUI thread blocked): what to draw next.
    void update(const rcsdk::GpuFrame& frame, const QRectF& target, const QRectF& source, bool smooth,
                RemoteSession* session) override {
        if (frame.handle() != pending_.handle()) {
            pending_ = frame;
            fresh_ = true;
        }
        session_ = session;
        target_ = target;
        source_ = source;
        if (smooth != smooth_) {
            smooth_ = smooth;
            sampler_.reset();
            bindings_.reset();
        }
        markDirty(QSGNode::DirtyMaterial);
    }

    void prepare() override {
        QRhi* rhi = window_->rhi();
        QRhiRenderTarget* target = renderTarget();
        if (!rhi || !target) {
            return;
        }
        if (!importer_) {
            importer_ = PlaneImporter::create(rhi);
            if (!importer_) {
                item_->reportImport(QStringLiteral("Unsupported graphics backend (Metal or D3D11 required)"));
                return;
            }
        }
        if (auto newer = newerFrame(session_, pending_)) { // Drawn again without a synchronization.
            pending_ = std::move(newer);
            fresh_ = true;
        }
        if (fresh_) {
            fresh_ = false;
            QString error;
            if (auto planes = pending_ ? importer_->import(pending_, error) : nullptr) {
                item_->framePresented();
                planes_.push_back(std::move(planes));
                // Earlier frames may still be read by frames in flight.
                while (planes_.size() > 3) {
                    planes_.pop_front();
                }
                bindings_.reset();
            } else if (!pending_) {
                planes_.clear();
                bindings_.reset();
            }
            item_->reportImport(error);
        }
        if (planes_.empty()) {
            return;
        }
        auto* batch = rhi->nextResourceUpdateBatch();
        if (!vertices_) {
            vertices_.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, sizeof(float) * 16));
            vertices_->create();
            uniforms_.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 80));
            uniforms_->create();
        }
        if (!sampler_) {
            const auto filter = smooth_ ? QRhiSampler::Linear : QRhiSampler::Nearest;
            sampler_.reset(
                rhi->newSampler(filter, filter, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
            sampler_->create();
        }
        const auto& planes = *planes_.back();
        if (!bindings_) {
            bindings_.reset(rhi->newShaderResourceBindings());
            const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
            bindings_->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(0, stages, uniforms_.get()),
                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                          planes.luma.get(), sampler_.get()),
                QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                          planes.chroma.get(), sampler_.get()),
            });
            bindings_->create();
            if (pipeline_) {
                pipeline_->setShaderResourceBindings(bindings_.get()); // Layout-compatible.
            }
        }
        if (!pipeline_ || pipelinePass_ != target->renderPassDescriptor()) {
            createPipeline(rhi, target);
        }

        const float x0 = float(target_.left()), y0 = float(target_.top());
        const float x1 = float(target_.right()), y1 = float(target_.bottom());
        const float u0 = float(source_.left()), v0 = float(source_.top());
        const float u1 = float(source_.right()), v1 = float(source_.bottom());
        const float quad[16] = {x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1};
        batch->updateDynamicBuffer(vertices_.get(), 0, sizeof(quad), quad);
        const QMatrix4x4 mvp = *projectionMatrix() * *matrix();
        const bool bt709 = planes.frame.info().color.matrix != RCS_MATRIX_BT601; // NV12: BT.709 or BT.601.
        const float parameters[2] = {bt709 ? 1.0f : 0.0f, float(inheritedOpacity())};
        batch->updateDynamicBuffer(uniforms_.get(), 0, 64, mvp.constData());
        batch->updateDynamicBuffer(uniforms_.get(), 64, sizeof(parameters), parameters);
        commandBuffer()->resourceUpdate(batch);
    }

    void render(const RenderState* state) override {
        if (planes_.empty() || !pipeline_) {
            return;
        }
        QRhiCommandBuffer* commands = commandBuffer();
        const QSize size = renderTarget()->pixelSize();
        commands->setGraphicsPipeline(pipeline_.get());
        commands->setViewport(QRhiViewport(0, 0, float(size.width()), float(size.height())));
        const QRect scissor = state->scissorEnabled() ? state->scissorRect() : QRect(QPoint(), size);
        commands->setScissor(QRhiScissor(scissor.x(), scissor.y(), scissor.width(), scissor.height()));
        commands->setShaderResources(bindings_.get());
        const QRhiCommandBuffer::VertexInput input(vertices_.get(), 0);
        commands->setVertexInput(0, 1, &input);
        commands->draw(4);
    }

    void releaseResources() override {
        pipeline_.reset();
        bindings_.reset();
        sampler_.reset();
        uniforms_.reset();
        vertices_.reset();
        planes_.clear();
        importer_.reset();
    }

    StateFlags changedStates() const override { return ViewportState | ScissorState; }
    RenderingFlags flags() const override { return BoundedRectRendering; }
    QRectF rect() const override { return target_; }

private:
    void createPipeline(QRhi* rhi, QRhiRenderTarget* target) {
        pipeline_.reset(rhi->newGraphicsPipeline());
        pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        pipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
        pipeline_->setShaderStages({
            {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/nv12.vert.qsb"))},
            {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/nv12.frag.qsb"))},
        });
        QRhiVertexInputLayout layout;
        layout.setBindings({{4 * sizeof(float)}});
        layout.setAttributes(
            {{0, 0, QRhiVertexInputAttribute::Float2, 0}, {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)}});
        pipeline_->setVertexInputLayout(layout);
        QRhiGraphicsPipeline::TargetBlend blend; // Premultiplied alpha, for item opacity.
        blend.enable = true;
        blend.srcColor = QRhiGraphicsPipeline::One;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        pipeline_->setTargetBlends({blend});
        pipeline_->setSampleCount(target->sampleCount());
        pipeline_->setShaderResourceBindings(bindings_.get());
        pipeline_->setRenderPassDescriptor(target->renderPassDescriptor());
        pipeline_->create();
        pipelinePass_ = target->renderPassDescriptor();
    }

    QQuickWindow* window_;
    RemoteVideo* item_;
    rcsdk::GpuFrame pending_;
    bool fresh_ = false;
    QRectF target_, source_;
    bool smooth_ = true;
    RemoteSession* session_ = nullptr;

    std::unique_ptr<PlaneImporter> importer_;
    std::deque<std::unique_ptr<NativePlanes>> planes_;
    std::unique_ptr<QRhiBuffer> vertices_, uniforms_;
    std::unique_ptr<QRhiSampler> sampler_;
    std::unique_ptr<QRhiShaderResourceBindings> bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
    QRhiRenderPassDescriptor* pipelinePass_ = nullptr;
};

} // namespace

FrameNode* createFrameNode(QQuickWindow* window, RemoteVideo* item) {
    return new VideoNode(window, item);
}

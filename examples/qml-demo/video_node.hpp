// The scene graph node that draws a RemoteVideo's frames into Qt Quick's
// render target, without a copy: through QRhi with the frame's NV12 planes as
// textures (Metal, D3D11: video_node_rhi.cpp), or with OpenGL ES straight
// from the frame's hardware buffer as an external texture (Android:
// video_node_gles.cpp).
#pragma once

#include <rcsdk/rcsdk.hpp>

#include <QRectF>
#include <QSGRenderNode>

class QQuickWindow;
class RemoteSession;
class RemoteVideo;

class FrameNode : public QSGRenderNode {
public:
    // Synchronization (GUI thread blocked): what to draw next, where in item
    // coordinates, and what of the frame's surface (normalized); and the
    // session whose newer frames it draws between synchronizations.
    virtual void update(const rcsdk::GpuFrame& frame, const QRectF& target, const QRectF& source, bool smooth,
                        RemoteSession* session) = 0;
};

// Render thread: `session`'s newest frame where it is newer than `drawn` and
// of its shape (so the target and source rectangles still hold: the session
// asks the render thread to draw it, RemoteSession::setRenderWindow), else
// an empty one.
rcsdk::GpuFrame newerFrame(RemoteSession* session, const rcsdk::GpuFrame& drawn);

FrameNode* createFrameNode(QQuickWindow* window, RemoteVideo* item);

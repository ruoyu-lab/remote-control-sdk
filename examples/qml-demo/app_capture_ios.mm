#include "app_capture.hpp"

#include "remote_session.hpp"

#include <rcsdk/gpu.h>

#import <ReplayKit/ReplayKit.h>
#import <VideoToolbox/VideoToolbox.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <mutex>
#include <utility>

namespace {

// What turns a picture of CGImagePropertyOrientation `orientation`
// (RPVideoSampleOrientationKey) upright.
CFStringRef uprightRotation(uint32_t orientation) {
    switch (orientation) {
    case kCGImagePropertyOrientationRight:
        return kVTRotation_CW90;
    case kCGImagePropertyOrientationLeft:
        return kVTRotation_CCW90;
    case kCGImagePropertyOrientationDown:
        return kVTRotation_180;
    default:
        return kVTRotation_0;
    }
}

// IOSurface-backed buffers of one size and format, made again when those change.
class Buffers {
public:
    ~Buffers() { CVPixelBufferPoolRelease(pool_); }

    // Retained; null when the system has none.
    CVPixelBufferRef take(size_t width, size_t height, OSType format) {
        if (pool_ && (width != width_ || height != height_ || format != format_)) {
            CVPixelBufferPoolRelease(pool_);
            pool_ = nullptr;
        }
        if (!pool_) {
            NSDictionary* attributes = @{
                (id)kCVPixelBufferWidthKey : @(width),
                (id)kCVPixelBufferHeightKey : @(height),
                (id)kCVPixelBufferPixelFormatTypeKey : @(format),
                (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
            };
            if (CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attributes, &pool_) !=
                kCVReturnSuccess) {
                return nullptr;
            }
            width_ = width;
            height_ = height;
            format_ = format;
        }
        CVPixelBufferRef buffer = nullptr;
        CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_, &buffer);
        return buffer;
    }

private:
    CVPixelBufferPoolRef pool_ = nullptr;
    size_t width_ = 0, height_ = 0;
    OSType format_ = 0;
};

} // namespace

struct AppCapture::Pipeline {
    explicit Pipeline(RemoteSession& s) : session(s) {}
    ~Pipeline() {
        if (rotation) {
            VTPixelRotationSessionInvalidate(rotation);
            CFRelease(rotation);
        }
        if (transfer) {
            VTPixelTransferSessionInvalidate(transfer);
            CFRelease(transfer);
        }
    }

    // ReplayKit's queue: one picture of the screen, upright and of the stream's size.
    void push(CMSampleBufferRef sample) {
        std::lock_guard lock(mutex);
        CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample);
        const rcs_stream_info stream = session.streamInfo();
        if (!running || !image || !stream.width) { // No viewer yet.
            return;
        }
        const OSType format = CVPixelBufferGetPixelFormatType(image);
        if (format != kCVPixelFormatType_32BGRA && format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
            format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
            return;
        }
        CVPixelBufferRetain(image);
        const auto replace = [&](CVPixelBufferRef by) {
            CVPixelBufferRelease(image);
            image = by;
        };
        NSNumber* orientation =
            (__bridge NSNumber*)CMGetAttachment(sample, (__bridge CFStringRef)RPVideoSampleOrientationKey, nullptr);
        if (CFStringRef turn = uprightRotation(orientation ? orientation.unsignedIntValue : 1);
            !CFEqual(turn, kVTRotation_0)) {
            size_t width = CVPixelBufferGetWidth(image), height = CVPixelBufferGetHeight(image);
            if (!CFEqual(turn, kVTRotation_180)) {
                std::swap(width, height);
            }
            if (!rotation && VTPixelRotationSessionCreate(nullptr, &rotation) != noErr) {
                return replace(nullptr);
            }
            VTSessionSetProperty(rotation, kVTPixelRotationPropertyKey_Rotation, turn);
            CVPixelBufferRef upright = rotated.take(width, height, format);
            if (!upright || VTPixelRotationSessionRotateImage(rotation, image, upright) != noErr) {
                CVPixelBufferRelease(upright);
                return replace(nullptr);
            }
            replace(upright);
        }
        if (CVPixelBufferGetWidth(image) != stream.width || CVPixelBufferGetHeight(image) != stream.height) {
            if (!transfer) {
                if (VTPixelTransferSessionCreate(nullptr, &transfer) != noErr) {
                    return replace(nullptr);
                }
                VTSessionSetProperty(transfer, kVTPixelTransferPropertyKey_ScalingMode, kVTScalingMode_Letterbox);
            }
            CVPixelBufferRef sized = scaled.take(stream.width, stream.height, format);
            if (!sized || VTPixelTransferSessionTransferImage(transfer, image, sized) != noErr) {
                CVPixelBufferRelease(sized);
                return replace(nullptr);
            }
            replace(sized);
        }
        rcs_gpu_frame_info frame = RCS_GPU_FRAME_INFO_INIT;
        frame.resource_type = RCS_GPU_CVPIXELBUFFER;
        frame.resource = image;
        frame.width = stream.width;
        frame.height = stream.height;
        if (format == kCVPixelFormatType_32BGRA) {
            frame.format = RCS_PIXEL_BGRA8;
        } else {
            NSString* matrix = CFBridgingRelease(CVBufferCopyAttachment(image, kCVImageBufferYCbCrMatrixKey, nullptr));
            frame.format = RCS_PIXEL_NV12;
            frame.color = {RCS_PRIMARIES_BT709, RCS_TRANSFER_BT709,
                           [matrix isEqualToString:(__bridge NSString*)kCVImageBufferYCbCrMatrix_ITU_R_601_4]
                               ? RCS_MATRIX_BT601
                               : RCS_MATRIX_BT709,
                           format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ? RCS_RANGE_FULL
                                                                                     : RCS_RANGE_LIMITED};
        }
        session.pushGpuFrame(frame); // Refused while the stream restarts (a new size): the next one goes.
        CVPixelBufferRelease(image);
    }

    RemoteSession& session;
    std::mutex mutex;
    bool running = false;
    VTPixelRotationSessionRef rotation = nullptr;
    VTPixelTransferSessionRef transfer = nullptr;
    Buffers rotated, scaled;
};

AppCapture::AppCapture(RemoteSession& session, QObject* parent)
    : QObject(parent), pipeline_(std::make_shared<Pipeline>(session)) {}

AppCapture::~AppCapture() {
    stop();
}

void AppCapture::start() {
    {
        std::lock_guard lock(pipeline_->mutex);
        if (pipeline_->running) {
            return;
        }
        pipeline_->running = true;
    }
    RPScreenRecorder* recorder = RPScreenRecorder.sharedRecorder;
    if (!recorder.available) { // E.g. screen recording is restricted, or another app records.
        {
            std::lock_guard lock(pipeline_->mutex);
            pipeline_->running = false;
        }
        emit failed(QStringLiteral("ReplayKit is not available now"));
        return;
    }
    recorder.microphoneEnabled = NO; // The session records the microphone itself when asked to.
    const std::shared_ptr<Pipeline> pipeline = pipeline_;
    QPointer<AppCapture> self(this);
    [recorder
        startCaptureWithHandler:^(CMSampleBufferRef sample, RPSampleBufferType type, NSError* error) {
          if (!error && type == RPSampleBufferTypeVideo) {
              pipeline->push(sample);
          }
        }
        completionHandler:^(NSError* error) {
          if (!error) {
              return;
          }
          const QString reason = QString::fromNSString(error.localizedDescription);
          QMetaObject::invokeMethod(
              qApp,
              [self, pipeline, reason] {
                  {
                      std::lock_guard lock(pipeline->mutex);
                      pipeline->running = false;
                  }
                  if (self) {
                      emit self->failed(reason);
                  }
              },
              Qt::QueuedConnection);
        }];
}

void AppCapture::stop() {
    {
        std::lock_guard lock(pipeline_->mutex);
        if (!pipeline_->running) {
            return;
        }
        pipeline_->running = false;
    }
    [RPScreenRecorder.sharedRecorder stopCaptureWithHandler:^(NSError*) {}];
}

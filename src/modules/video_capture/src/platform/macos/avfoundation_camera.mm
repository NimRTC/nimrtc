// modules/video_capture/src/platform/macos/avfoundation_camera.mm
// ----------------------------------------------------------------------------
// The macOS AVFoundation camera (remote-desktop plan A9).
//
// The camera path on macOS goes through **AVFoundation**:
//
//   1. `AVCaptureDeviceDiscoverySession` enumerates the video devices.
//   2. An `AVCaptureSession` is configured with the device's input and
//      an `AVCaptureVideoDataOutput`.
//   3. The output's pixel format is set to
//      `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange` (NV12), which
//      is the format the encoder path wants.
//   4. The output's delegate receives the frames in
//      `captureOutput:didOutputSampleBuffer:fromConnection:`.
//
// ## The threading
//
// AVFoundation calls the delegate on its **own** queue.  The frames are
// handed over through a small lock-protected slot, and the camera
// device's thread picks them up — so the engine's tick thread is never
// parked by the capture callback (RD-6).
//
// ## Why the delegate is a separate Objective-C object
//
// The frame source is a C++ class (`ICameraFrameSource` is a plain
// C++ interface).  It cannot *also* inherit
// `AVCaptureVideoDataOutputSampleBufferDelegate`: in Objective-C++ a
// protocol-qualified name is a template, and a C++ class cannot
// inherit from a template base.  So the delegate lives in its own
// small Objective-C class, which forwards the samples into the C++
// source through a plain C function pointer.
//
// ## The zero-copy path
//
// The `CVPixelBuffer` is a native GPU-backed surface, so the frames
// *could* be delivered as `GpuBackend::kAppleCFPixelBuffer`.  This
// adapter takes the CPU path (the portable layer's queue copies the
// bytes), and the copy is counted honestly in
// `CaptureStats::bytes_copied`.
//
// @note Remote-desktop plan A9.  Platform adapter — the portable half
//       lives in `src/camera_capture.cpp`.
// ----------------------------------------------------------------------------

#include "platform/camera_factory.hpp"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// The C callback the Objective-C delegate uses to hand a sample over.
// It is a plain C function pointer so that the Objective-C class does
// not have to name any C++ type in its interface.
using NimAvfSampleDeliverer = void (*)(void* ctx, CMSampleBufferRef sample);

/**
 * @brief The Objective-C half of the AVFoundation delegate.
 *
 * It owns nothing but the callback: the C++ side owns the session, the
 * output and the slot.
 */
@interface NimAVFCameraDelegate
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
- (void)setDeliverer:(NimAvfSampleDeliverer)deliverer context:(void*)context;
@end

@implementation NimAVFCameraDelegate {
    NimAvfSampleDeliverer deliverer_;
    void* context_;
}

- (void)setDeliverer:(NimAvfSampleDeliverer)deliverer context:(void*)context {
    deliverer_ = deliverer;
    context_ = context;
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    if (deliverer_ == nullptr) return;
    deliverer_(context_, sample);
}

@end

namespace nimrtc::video_capture {

namespace {

/**
 * @brief The AVFoundation camera frame source.
 */
class AvfoundationCameraSource final : public ICameraFrameSource {
  public:
    AvfoundationCameraSource() = default;

    ~AvfoundationCameraSource() override { close(); }

    AvfoundationCameraSource(const AvfoundationCameraSource&) = delete;
    AvfoundationCameraSource& operator=(const AvfoundationCameraSource&) = delete;

    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_cameras() const noexcept override {
        std::vector<plugins::CaptureDevice> devices;

        AVCaptureDeviceDiscoverySession* discovery =
            [AVCaptureDeviceDiscoverySession
                discoverySessionWithDeviceTypes:@[
                    AVCaptureDeviceTypeBuiltInWideAngleCamera,
                    AVCaptureDeviceTypeExternalUnknown
                ]
                                      mediaType:AVMediaTypeVideo
                                       position:AVCaptureDevicePositionUnspecified];
        if (discovery == nil) return devices;

        for (AVCaptureDevice* device in discovery.devices) {
            plugins::CaptureDevice out;
            // The `?:` with an omitted middle operand is a GNU
            // extension, and the project's warning set rejects it.
            const char* uid = device.uniqueID.UTF8String;
            out.id = std::string(uid != nullptr ? uid : "");
            if (out.id.empty()) continue;
            const char* label = device.localizedName.UTF8String;
            out.name = std::string(label != nullptr ? label : "");
            out.kind = plugins::CaptureDeviceKind::kCamera;

            // The largest format the device offers.
            double max_w = 0;
            double max_h = 0;
            for (AVCaptureDeviceFormat* format in device.formats) {
                const CMVideoDimensions dims =
                    CMVideoFormatDescriptionGetDimensions(format.formatDescription);
                if (static_cast<double>(dims.width) > max_w) {
                    max_w = static_cast<double>(dims.width);
                }
                if (static_cast<double>(dims.height) > max_h) {
                    max_h = static_cast<double>(dims.height);
                }
            }
            out.max_width = static_cast<std::uint32_t>(max_w);
            out.max_height = static_cast<std::uint32_t>(max_h);
            out.max_fps = 30;

            devices.push_back(std::move(out));
        }

        return devices;
    }

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override {
        std::vector<plugins::CaptureFormat> formats;

        const auto devices = enumerate_cameras();
        std::uint32_t max_w = 0;
        std::uint32_t max_h = 0;
        for (const auto& d : devices) {
            if (!device_id.empty() && d.id != device_id) continue;
            if (d.max_width > max_w) max_w = d.max_width;
            if (d.max_height > max_h) max_h = d.max_height;
        }
        if (max_w == 0 || max_h == 0) return formats;

        const std::uint32_t sizes[][2] = {{640, 480}, {1280, 720}, {max_w, max_h}};
        for (const auto& s : sizes) {
            if (s[0] > max_w || s[1] > max_h) continue;
            plugins::CaptureFormat format;
            format.width = s[0];
            format.height = s[1];
            format.fps = 30;
            format.tag = "nv12-avfoundation";
            format.zero_copy = false;
            format.has_dirty_rects = false;
            formats.push_back(std::move(format));
        }
        return formats;
    }

    plugins::Status open(const std::string& device_id,
                         const plugins::CaptureFormat& format) noexcept override {
        (void)format;
        close();

        AVCaptureDevice* device = nil;
        AVCaptureDeviceDiscoverySession* discovery =
            [AVCaptureDeviceDiscoverySession
                discoverySessionWithDeviceTypes:@[
                    AVCaptureDeviceTypeBuiltInWideAngleCamera,
                    AVCaptureDeviceTypeExternalUnknown
                ]
                                      mediaType:AVMediaTypeVideo
                                       position:AVCaptureDevicePositionUnspecified];
        for (AVCaptureDevice* candidate in discovery.devices) {
            const char* uid = candidate.uniqueID.UTF8String;
            if (uid == nullptr) continue;
            if (device_id == std::string(uid)) {
                device = candidate;
                break;
            }
        }
        if (device == nil) return plugins::kErrInvalidParam;

        NSError* error = nil;
        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device
                                                                            error:&error];
        if (input == nil || error != nil) return plugins::kErrHardwareError;

        AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
        output.videoSettings = @{
            (__bridge id)kCVPixelBufferPixelFormatTypeKey :
                @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
        };
        output.alwaysDiscardsLateVideoFrames = YES;

        AVCaptureSession* session = [[AVCaptureSession alloc] init];
        if (![session canAddInput:input]) return plugins::kErrHardwareError;
        [session addInput:input];
        if (![session canAddOutput:output]) return plugins::kErrHardwareError;
        [session addOutput:output];

        // The output's delegate receives the frames on its own queue.
        dispatch_queue_t queue =
            dispatch_queue_create("com.nimrtc.camera.avfoundation", DISPATCH_QUEUE_SERIAL);

        NimAVFCameraDelegate* delegate = [[NimAVFCameraDelegate alloc] init];
        [delegate setDeliverer:&AvfoundationCameraSource::deliver_sample
                        context:this];
        [output setSampleBufferDelegate:delegate queue:queue];

        [session startRunning];

        session_ = session;
        output_ = output;
        delegate_ = delegate;
        queue_ = queue;
        return plugins::kOk;
    }

    void close() noexcept override {
        // The delegate is detached *first*: after this call AVFoundation
        // makes no further callbacks, so the C++ object can be torn
        // down safely.
        if (output_ != nil) {
            [output_ setSampleBufferDelegate:nil queue:NULL];
            output_ = nil;
        }
        delegate_ = nil;
        queue_ = nil;

        if (session_ != nil) {
            [session_ stopRunning];
            session_ = nil;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        has_pending_ = false;
        width_ = 0;
        height_ = 0;
    }

    plugins::Status next_frame(std::vector<std::uint8_t>& pixels, std::size_t& size,
                               std::int32_t& stride, std::uint32_t& width,
                               std::uint32_t& height, std::int64_t& ts) noexcept override {
        std::vector<std::uint8_t> frame;
        std::uint32_t frame_width = 0;
        std::uint32_t frame_height = 0;
        std::int64_t frame_ts = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!has_pending_) return plugins::kErrNotReady;
            frame.swap(pending_);
            has_pending_ = false;
            frame_width = width_;
            frame_height = height_;
            frame_ts = timestamp_;
        }

        pixels.swap(frame);
        size = pixels.size();
        stride = static_cast<std::int32_t>(frame_width);
        width = frame_width;
        height = frame_height;
        ts = frame_ts;
        return plugins::kOk;
    }

  private:
    // ---- The delegate callback -------------------------------------------
    //
    // AVFoundation hands the frame over on its own queue, and the
    // camera's thread picks it up from the slot.
    static void deliver_sample(void* ctx, CMSampleBufferRef sample) {
        if (ctx == nullptr) return;
        static_cast<AvfoundationCameraSource*>(ctx)->handle_sample(sample);
    }

    void handle_sample(CMSampleBufferRef sample) {
        if (sample == nullptr) return;

        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        if (image == nullptr) return;

        const std::size_t plane_count = CVPixelBufferGetPlaneCount(image);
        const std::size_t y_size = plane_count > 0
                                       ? CVPixelBufferGetBytesPerRowOfPlane(image, 0) *
                                             CVPixelBufferGetHeightOfPlane(image, 0)
                                       : 0;
        const std::size_t uv_size = plane_count > 1
                                        ? CVPixelBufferGetBytesPerRowOfPlane(image, 1) *
                                              CVPixelBufferGetHeightOfPlane(image, 1)
                                        : 0;
        if (y_size == 0) return;

        std::vector<std::uint8_t> bytes;
        bytes.reserve(y_size + uv_size);

        const std::uint8_t* y_plane =
            static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 0));
        const std::size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
        const std::size_t y_height = CVPixelBufferGetHeightOfPlane(image, 0);
        for (std::size_t row = 0; row < y_height; ++row) {
            bytes.insert(bytes.end(), y_plane + row * y_stride,
                         y_plane + row * y_stride + CVPixelBufferGetWidthOfPlane(image, 0));
        }

        if (uv_size > 0 && plane_count > 1) {
            const std::uint8_t* uv_plane =
                static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 1));
            const std::size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
            const std::size_t uv_height = CVPixelBufferGetHeightOfPlane(image, 1);
            for (std::size_t row = 0; row < uv_height; ++row) {
                bytes.insert(bytes.end(), uv_plane + row * uv_stride,
                             uv_plane + row * uv_stride +
                                 CVPixelBufferGetWidthOfPlane(image, 1));
            }
        }

        const std::int64_t ts =
            (CMSampleBufferGetPresentationTimeStamp(sample).value / 1000);

        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(bytes);
        width_ = static_cast<std::uint32_t>(CVPixelBufferGetWidth(image));
        height_ = static_cast<std::uint32_t>(CVPixelBufferGetHeight(image));
        timestamp_ = ts;
        has_pending_ = true;
    }

    AVCaptureSession* session_ = nil;
    AVCaptureVideoDataOutput* output_ = nil;
    NimAVFCameraDelegate* delegate_ = nil;
    dispatch_queue_t queue_ = nil;

    std::mutex mutex_;
    std::vector<std::uint8_t> pending_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::int64_t timestamp_ = 0;
    bool has_pending_ = false;
};

} // anonymous namespace

namespace detail {

plugins::IVideoCapture* create_camera_capture(plugins::CaptureConfig cfg, CameraBackend backend) {
    if (backend != CameraBackend::kAvFoundation) {
        // The other backends are not available on macOS.
        return nullptr;
    }
    auto source = std::make_unique<AvfoundationCameraSource>();
    CameraCaptureOptions opts;
    opts.backend = backend;
    opts.pixel_format = plugins::VideoPixelFormat::kNV12;
    return new CameraCapture(std::move(cfg), std::move(source), opts);
}

} // namespace detail

} // namespace nimrtc::video_capture

#endif // defined(__APPLE__)

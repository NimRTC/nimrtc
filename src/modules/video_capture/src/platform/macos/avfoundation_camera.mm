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

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace nimrtc::video_capture {

namespace {

/**
 * @brief The AVFoundation camera frame source.
 */
class AvfoundationCameraSource final : public ICameraFrameSource,
                                      public AVCaptureVideoDataOutputSampleBufferDelegate {
  public:
    AvfoundationCameraSource() = default;

    ~AvfoundationCameraSource() override { close(); }

    AvfoundationCameraSource(const AvfoundationCameraSource&) = delete;
    AvfoundationCameraSource& operator=(const AvfoundationCameraSource&) = delete;

    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_cameras() const noexcept override {
        std::vector<plugins::CaptureDevice> devices;

        AVCaptureDeviceDiscoverySession* discovery = [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera,
                                               AVCaptureDeviceTypeExternalUnknown ]
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];
        if (discovery == nil) return devices;

        for (AVCaptureDevice* device in discovery.devices) {
            plugins::CaptureDevice out;
            out.id = std::string(device.uniqueID.UTF8String ?: "");
            if (out.id.empty()) continue;
            out.name = std::string(device.localizedName.UTF8String ?: "");
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
        close();

        AVCaptureDevice* device = nil;
        AVCaptureDeviceDiscoverySession* discovery = [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera,
                                               AVCaptureDeviceTypeExternalUnknown ]
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];
        for (AVCaptureDevice* candidate in discovery.devices) {
            if (device_id == std::string(candidate.uniqueID.UTF8String ?: "")) {
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
            (id)kCVPixelBufferPixelFormatTypeKey :
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
        [output setSampleBufferDelegate:(id<AVCaptureVideoDataOutputSampleBufferDelegate>)self
                                  queue:queue];
        self.queue_ = queue;

        [session startRunning];

        session_ = session;
        output_ = output;
        return plugins::kOk;
    }

    void close() noexcept {
        if (session_ != nil) {
            [session_ stopRunning];
            session_ = nil;
        }
        if (output_ != nil) {
            [output_ setSampleBufferDelegate:nil queue:NULL];
            output_ = nil;
        }
        queue_ = nil;

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
    // ---- AVCaptureVideoDataOutputSampleBufferDelegate ---------------------
    //
    // AVFoundation hands the frame over on its own queue, and the
    // camera's thread picks it up from the slot.
    void captureOutput:(AVCaptureOutput*)output
        didOutputSampleBuffer:(CMSampleBufferRef)sample
               fromConnection:(AVCaptureConnection*)connection {
        (void)output;
        (void)connection;
        handle_sample(sample);
    }

    // The delegate callback: AVFoundation hands the frame over on its
    // own queue, and the camera's thread picks it up from the slot.
    void handle_sample(CMSampleBufferRef sample) {
        if (sample == nullptr) return;

        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        if (image == nullptr) return;

        const std::size_t y_size = CVPixelBufferGetPlaneCount(image) > 0
                                       ? CVPixelBufferGetBytesPerRowOfPlane(image, 0) *
                                             CVPixelBufferGetHeightOfPlane(image, 0)
                                       : 0;
        const std::size_t uv_size = CVPixelBufferGetPlaneCount(image) > 1
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

        if (uv_size > 0 && CVPixelBufferGetPlaneCount(image) > 1) {
            const std::uint8_t* uv_plane =
                static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, 1));
            const std::size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
            const std::size_t uv_height = CVPixelBufferGetHeightOfPlane(image, 1);
            for (std::size_t row = 0; row < uv_height; ++row) {
                bytes.insert(bytes.end(), uv_plane + row * uv_stride,
                             uv_plane + row * uv_stride + CVPixelBufferGetWidthOfPlane(image, 1));
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

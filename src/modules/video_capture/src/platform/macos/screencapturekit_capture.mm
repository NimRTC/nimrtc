// modules/video_capture/src/platform/macos/screencapturekit_capture.mm
// ----------------------------------------------------------------------------
// The macOS ScreenCaptureKit capture device (remote-desktop plan A4).
//
// Objective-C++ on purpose: ScreenCaptureKit is an Objective-C
// framework, and the only way to call `SCShareableContent` / `SCStream`
// from a C++ engine is through an Objective-C++ translation unit.
//
// The adapter is *thin*: it configures the stream, receives the sample
// buffers, and hands each `CVPixelBuffer` to the portable core.  All
// the mechanism (the queue, the drop policy, the dirty-rect
// bookkeeping, the statistics, the negotiation) lives in
// `UnixCaptureCore`.
//
// See screencapturekit_capture.hpp for the contract.
//
// ## A note on the layout
//
// The Objective-C class declaration has to sit at *global* scope --
// clang rejects an `@interface` inside a C++ namespace -- so the
// Objective-C parts of this file live in a global-scope helper
// namespace `sck_internal`, and the C++ class lives in
// `nimrtc::video_capture` as usual.
// ----------------------------------------------------------------------------

#if defined(__APPLE__)

#import <Foundation/Foundation.h>

#include <AvailabilityMacros.h>

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 121300
    #import <ScreenCaptureKit/ScreenCaptureKit.h>
    #import <CoreMedia/CoreMedia.h>
    #import <CoreVideo/CoreVideo.h>
    #import <CoreGraphics/CoreGraphics.h>
    #define NIMRTC_SCK_AVAILABLE 1
#else
    #define NIMRTC_SCK_AVAILABLE 0
#endif

#include "screencapturekit_capture.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#if NIMRTC_SCK_AVAILABLE

// ---------------------------------------------------------------------------
// The global-scope Objective-C helpers
// ---------------------------------------------------------------------------

namespace sck_internal {

/**
 * @brief Holds the most recent sample buffer the stream produced.
 *
 * ScreenCaptureKit delivers the frames on the queue the stream was
 * configured with; the sink keeps the *latest* buffer and the capture
 * thread picks it up.  Keeping only the latest is deliberate: a stale
 * desktop picture is worth nothing, and the bounded queue downstream
 * would drop it anyway.
 */
class FrameSink {
public:
    ~FrameSink() {
        if (latest_ != nullptr) CVPixelBufferRelease(latest_);
    }

    /** The most recent sample buffer, or null when there is none.  The
     *  caller owns the returned reference. */
    CVPixelBufferRef take_latest() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        CVPixelBufferRef out = latest_;
        latest_ = nullptr;
        return out;
    }

    /** Store a buffer (retaining it). */
    void store(CVPixelBufferRef buffer) noexcept {
        if (buffer == nullptr) return;
        CVPixelBufferRetain(buffer);
        std::lock_guard<std::mutex> lock(mutex_);
        if (latest_ != nullptr) {
            CVPixelBufferRelease(latest_);
        }
        latest_ = buffer;
        ++received_;
    }

    std::uint64_t received() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

private:
    mutable std::mutex mutex_;
    CVPixelBufferRef latest_ = nullptr;
    std::uint64_t received_ = 0;
};

} // namespace sck_internal

/**
 * @brief The Objective-C object that plugs into ScreenCaptureKit.
 *
 * It owns the `SCStream` and forwards the sample buffers into the
 * `FrameSink`.
 */
@interface NimSCKSink : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) sck_internal::FrameSink* sink;
@property(nonatomic, assign) BOOL failed;
- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type;
- (void)stream:(SCStream*)stream
    didStopWithError:(NSError*)error;
@end

@implementation NimSCKSink

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
    (void)stream;
    (void)type;
    if (sampleBuffer == nullptr) return;
    CVPixelBufferRef pixel_buffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (pixel_buffer == nullptr) return;
    if (self.sink != nullptr) self.sink->store(pixel_buffer);
}

- (void)stream:(SCStream*)stream
    didStopWithError:(NSError*)error {
    (void)stream;
    (void)error;
    self.failed = YES;
}

@end

namespace nimrtc::video_capture {

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** The device-id prefixes the backend hands out. */
constexpr const char* kDisplayPrefix = "sc-display:";
constexpr const char* kWindowPrefix  = "sc-window:";

/** The format tag the backend reports.  BGRA is what every desktop
 *  compositor produces natively. */
constexpr const char* kFormatTag = "bgra-sck";

/** True iff this build and this machine can use ScreenCaptureKit. */
bool sck_available() noexcept {
#if NIMRTC_SCK_AVAILABLE
    // The framework is 12.3+, so the *runtime* check matters as much as
    // the compile-time one.
    if (@available(macOS 12.3, *)) {
        return true;
    }
#endif
    return false;
}

/** The ScreenCaptureKit permission state. */
enum class PermissionState : std::uint8_t {
    kGranted,
    kDenied,
    kUnavailable,
};

PermissionState permission_state() noexcept {
#if NIMRTC_SCK_AVAILABLE
    if (@available(macOS 12.3, *)) {
        if (CGPreflightScreenCaptureAccess()) return PermissionState::kGranted;
        // Not granted yet: `CGRequestScreenCaptureAccess()` prompts the
        // user.  It returns false when the prompt was dismissed -- which
        // is the refusal case the plan asks to be a clean error.
        if (CGRequestScreenCaptureAccess()) return PermissionState::kGranted;
        return PermissionState::kDenied;
    }
#endif
    return PermissionState::kUnavailable;
}

std::int64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** The numeric id out of a `"sc-display:123"` / `"sc-window:456"`
 *  device id. */
std::uint64_t parse_id(const std::string& id) noexcept {
    const std::size_t colon = id.rfind(':');
    if (colon == std::string::npos) return 0;
    try {
        return std::stoull(id.substr(colon + 1));
    } catch (const std::exception&) {
        return 0;
    }
}

/** The shareable content, fetched synchronously. */
SCShareableContent* fetch_shareable_content() noexcept {
    __block SCShareableContent* content = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [SCShareableContent
        getShareableContentWithCompletionHandler:^(
            SCShareableContent* shareable, NSError* error) {
            (void)error;
            content = shareable;
            dispatch_semaphore_signal(done);
        }];
    // The call returns immediately when the user has already granted
    // access; the timeout keeps a wedged compositor from parking the
    // caller forever.
    dispatch_semaphore_wait(
        done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    return content;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ScreenCaptureKitCapture::Impl
// ---------------------------------------------------------------------------

class ScreenCaptureKitCapture::Impl {
public:
    Impl(plugins::CaptureConfig cfg, UnixCaptureOptions opts)
        : cfg_(std::move(cfg)), opts_(opts) {}

    ~Impl() { close_device(); }

    // ---- Enumeration ------------------------------------------------------

    std::vector<plugins::CaptureDevice> enumerate_devices() const noexcept {
        std::vector<plugins::CaptureDevice> devices;
        SCShareableContent* content = fetch_shareable_content();
        if (content == nil) return devices;

        for (SCDisplay* display in content.displays) {
            plugins::CaptureDevice device;
            device.id = std::string(kDisplayPrefix) +
                        std::to_string(display.displayID);
            device.name = "Display " + std::to_string(display.displayID);
            device.kind = plugins::CaptureDeviceKind::kScreen;
            device.max_width = display.width;
            device.max_height = display.height;
            device.max_fps = display.refreshRate > 0
                                 ? static_cast<std::uint32_t>(
                                       display.refreshRate)
                                 : 60u;
            devices.push_back(std::move(device));
        }

        for (SCWindow* window in content.windows) {
            plugins::CaptureDevice device;
            device.id = std::string(kWindowPrefix) +
                        std::to_string(window.windowID);
            device.name = window.title != nil
                              ? std::string(window.title.UTF8String)
                              : ("Window " + std::to_string(window.windowID));
            device.kind = plugins::CaptureDeviceKind::kWindow;
            device.max_width =
                static_cast<std::uint32_t>(window.frame.size.width);
            device.max_height =
                static_cast<std::uint32_t>(window.frame.size.height);
            device.max_fps = 60u;
            devices.push_back(std::move(device));
        }
        return devices;
    }

    std::vector<plugins::CaptureFormat> supported_formats(
        const std::string& device_id) const noexcept {
        std::vector<plugins::CaptureFormat> formats;

        std::uint32_t max_w = 0;
        std::uint32_t max_h = 0;
        for (const auto& device : enumerate_devices()) {
            if (!device_id.empty() && device.id != device_id) continue;
            if (device.max_width > max_w) max_w = device.max_width;
            if (device.max_height > max_h) max_h = device.max_height;
        }
        if (max_w == 0 || max_h == 0) return formats;

        const std::uint32_t sizes[][2] = {
            {1280, 720}, {1920, 1080}, {max_w, max_h},
        };
        for (const auto& size : sizes) {
            if (size[0] > max_w || size[1] > max_h) continue;
            plugins::CaptureFormat format;
            format.width = size[0];
            format.height = size[1];
            format.fps = 30;
            format.tag = kFormatTag;
            format.zero_copy = true;
            // ScreenCaptureKit does not report the changed region, so
            // the portable pixel-diff supplies the rectangles.
            format.has_dirty_rects = opts_.enable_pixel_diff;
            formats.push_back(std::move(format));
        }
        return formats;
    }

    // ---- Lifecycle --------------------------------------------------------

    plugins::Status open_device(UnixCaptureError& error) noexcept {
        if (!sck_available()) {
            error = UnixCaptureError::kUnsupportedFormat;
            return plugins::kErrUnsupported;
        }

        const PermissionState permission = permission_state();
        if (permission == PermissionState::kUnavailable) {
            error = UnixCaptureError::kUnsupportedFormat;
            return plugins::kErrUnsupported;
        }
        if (permission == PermissionState::kDenied) {
            // The refusal is a clean, *distinct* error: the host knows
            // it has to ask the user for screen recording.
            error = UnixCaptureError::kPermissionDenied;
            return plugins::kErrHardwareError;
        }

        SCShareableContent* content = fetch_shareable_content();
        if (content == nil) {
            error = UnixCaptureError::kPermissionDenied;
            return plugins::kErrHardwareError;
        }

        const bool wants_window = cfg_.device_id.rfind(kWindowPrefix, 0) == 0;
        const std::uint64_t wanted_id = parse_id(cfg_.device_id);

        SCContentFilter* filter = nil;
        if (wants_window) {
            for (SCWindow* window in content.windows) {
                if (window.windowID != wanted_id) continue;
                filter = [SCContentFilter
                    contentFilterForIncludingWindow:window];
                break;
            }
        } else {
            SCDisplay* chosen = nil;
            for (SCDisplay* display in content.displays) {
                if (wanted_id != 0 && display.displayID != wanted_id) continue;
                chosen = display;
                break;
            }
            if (chosen == nil && content.displays.count > 0) {
                chosen = content.displays.firstObject;
            }
            if (chosen != nil) {
                filter = [SCContentFilter
                    contentFilterForDisplay:chosen
                          excludingWindows:@[]];
            }
        }

        if (filter == nil) {
            error = UnixCaptureError::kDeviceNotFound;
            return plugins::kErrInvalidParam;
        }

        // ---- The format -------------------------------------------------
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        for (const auto& device : enumerate_devices()) {
            if (!cfg_.device_id.empty() && device.id != cfg_.device_id) continue;
            width = device.max_width;
            height = device.max_height;
            break;
        }
        if (cfg_.format.width != 0 && cfg_.format.height != 0) {
            width = std::min(cfg_.format.width,
                             width != 0 ? width : cfg_.format.width);
            height = std::min(cfg_.format.height,
                              height != 0 ? height : cfg_.format.height);
        }
        if (width == 0 || height == 0) {
            error = UnixCaptureError::kDeviceNotFound;
            return plugins::kErrInvalidParam;
        }

        SCStreamConfiguration* configuration =
            [[SCStreamConfiguration alloc] init];
        configuration.width = width;
        configuration.height = height;
        configuration.minimumFrameInterval =
            CMTimeMake(1, static_cast<int32_t>(
                              cfg_.format.fps != 0 ? cfg_.format.fps : 30));
        configuration.showsCursor = cfg_.want_cursor ? YES : NO;
        configuration.queueDepth = 3;
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;

        // ---- The stream -------------------------------------------------
        sink_ = [[NimSCKSink alloc] init];
        sink_.sink = &frame_sink_;

        NSError* error_ns = nil;
        SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                              configuration:configuration
                                                  delegate:sink_];
        if (stream == nil) {
            error = UnixCaptureError::kCaptureFailed;
            return plugins::kErrHardwareError;
        }

        [stream addStreamOutput:sink_
                           type:SCStreamOutputTypeScreen
             sampleContextQueue:dispatch_get_global_queue(
                 DISPATCH_QUEUE_PRIORITY_USER_INITIATED, 0)];

        if (![stream startCaptureWithError:&error_ns]) {
            error = UnixCaptureError::kCaptureFailed;
            return plugins::kErrHardwareError;
        }

        stream_ = stream;

        plugins::CaptureFormat negotiated;
        negotiated.width = width;
        negotiated.height = height;
        negotiated.fps = cfg_.format.fps != 0 ? cfg_.format.fps : 30u;
        negotiated.tag = kFormatTag;
        negotiated.zero_copy = opts_.allow_zero_copy;
        negotiated.has_dirty_rects = opts_.enable_pixel_diff;
        negotiated_format_ = negotiated;

        return plugins::kOk;
    }

    void close_device() noexcept {
        if (stream_ != nil) {
            [stream_ stopCapture];
            stream_ = nil;
        }
        if (sink_ != nil) {
            sink_.sink = nullptr;
            sink_ = nil;
        }
    }

    void start() noexcept {
        // The stream was already started by `open_device()`; there is
        // nothing further to do here.
    }

    void stop() noexcept {
        // The stream is stopped by `close_device()`.
    }

    // ---- The frame pump ---------------------------------------------------

    plugins::Status poll_frame() noexcept {
        if (sink_ != nil && sink_.failed) {
            // ScreenCaptureKit stopped the stream: the device is lost.
            return plugins::kErrHardwareError;
        }

        CVPixelBufferRef buffer = frame_sink_.take_latest();
        if (buffer == nullptr) {
            return plugins::kErrNotReady;
        }

        const std::int64_t ts = now_us();

        // ---- The zero-copy path ------------------------------------------
        //
        // The `CVPixelBuffer` is a native GPU surface, so it is handed
        // over as is: the pixels never enter host memory.
        if (opts_.allow_zero_copy) {
            plugins::CaptureDirtyRects dirty;
            dirty.full_frame = true;   // SCK reports no changed region
            auto owner = std::shared_ptr<void>(
                buffer, [](void* p) {
                    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(p));
                });
            return core()->submit_gpu_frame(
                nullptr, 0, 0, negotiated_format_.width,
                negotiated_format_.height, ts,
                plugins::GpuBackend::kAppleCFPixelBuffer, buffer, 0, &dirty,
                std::move(owner));
        }

        // ---- The CPU path -------------------------------------------------
        //
        // The buffer is locked and copied out.  The copy is counted, so
        // the statistics show that the fast path was not taken.
        CVPixelBufferRef buffer_ref = buffer;
        const std::uint32_t width = negotiated_format_.width;
        const std::uint32_t height = negotiated_format_.height;

        CVPixelBufferLockBaseAddress(buffer_ref,
                                     kCVPixelBufferLock_ReadOnly);
        void* base = CVPixelBufferGetBaseAddress(buffer_ref);
        const std::size_t stride = CVPixelBufferGetBytesPerRow(buffer_ref);

        plugins::Status status = plugins::kErrHardwareError;
        if (base != nullptr) {
            std::vector<std::uint8_t> pixels(stride * height);
            std::memcpy(pixels.data(), base, pixels.size());
            plugins::CaptureDirtyRects dirty;
            dirty.full_frame = true;
            status = core()->submit_frame(
                pixels.data(), pixels.size(),
                static_cast<std::int32_t>(stride), width, height, ts, &dirty);
        }
        CVPixelBufferUnlockBaseAddress(buffer_ref,
                                       kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(buffer_ref);
        return status;
    }

    plugins::CaptureCapabilities capabilities() const noexcept {
        plugins::CaptureCapabilities caps;
        caps.zero_copy = true;
        // ScreenCaptureKit reports no changed region; the portable
        // pixel-diff supplies the rectangles.
        caps.dirty_rects = opts_.enable_pixel_diff;
        // ScreenCaptureKit composites the pointer into the frame.
        caps.cursor_composited = true;
        caps.region_selection = true;
        caps.enumeration = true;
        caps.max_pixels = 0;   // bounded by the device itself
        caps.max_fps = 0;
        caps.backend = "macos-screencapturekit";
        return caps;
    }

    // ---- Accessors --------------------------------------------------------

    [[nodiscard]] const plugins::CaptureFormat&
    negotiated_format() const noexcept {
        return negotiated_format_;
    }

    void set_core(UnixCaptureCore* core) noexcept { core_ptr_ = core; }

private:
    UnixCaptureCore* core() noexcept { return core_ptr_; }

    plugins::CaptureConfig cfg_;
    UnixCaptureOptions opts_;
    plugins::CaptureFormat negotiated_format_{};

    sck_internal::FrameSink frame_sink_;
    NimSCKSink* sink_ = nil;
    SCStream* stream_ = nil;

    UnixCaptureCore* core_ptr_ = nullptr;
};

} // namespace nimrtc::video_capture

#else // !NIMRTC_SCK_AVAILABLE

namespace nimrtc::video_capture {

// The framework is macOS 12.3+; on an older SDK the backend reports
// "unsupported" rather than pretending to work.
//
// The `Impl` still has to exist (the base class holds a `unique_ptr` to
// it, and the destructor needs a complete type), so it is a placeholder
// that never does anything.

class ScreenCaptureKitCapture::Impl {
public:
    Impl(plugins::CaptureConfig, UnixCaptureOptions) {}
};

} // namespace nimrtc::video_capture

#endif // NIMRTC_SCK_AVAILABLE

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// ScreenCaptureKitCapture
// ---------------------------------------------------------------------------

ScreenCaptureKitCapture::ScreenCaptureKitCapture(plugins::CaptureConfig cfg,
                                                 UnixCaptureOptions opts) noexcept
    : UnixCaptureDevice(cfg, opts) {
#if NIMRTC_SCK_AVAILABLE
    impl_ = std::make_unique<Impl>(std::move(cfg), opts);
    impl_->set_core(&core());
#endif
}

ScreenCaptureKitCapture::~ScreenCaptureKitCapture() = default;

std::vector<plugins::CaptureDevice>
ScreenCaptureKitCapture::platform_enumerate_devices() const noexcept {
#if NIMRTC_SCK_AVAILABLE
    return impl_ ? impl_->enumerate_devices()
                 : std::vector<plugins::CaptureDevice>{};
#else
    return {};
#endif
}

std::vector<plugins::CaptureFormat>
ScreenCaptureKitCapture::platform_supported_formats(
    const std::string& device_id) const noexcept {
#if NIMRTC_SCK_AVAILABLE
    return impl_ ? impl_->supported_formats(device_id)
                 : std::vector<plugins::CaptureFormat>{};
#else
    return {};
#endif
}

plugins::Status ScreenCaptureKitCapture::platform_open_device(
    UnixCaptureError& error) noexcept {
#if NIMRTC_SCK_AVAILABLE
    if (!impl_) {
        error = UnixCaptureError::kCaptureFailed;
        return plugins::kErrInternal;
    }
    const plugins::Status status = impl_->open_device(error);
    if (status == plugins::kOk) {
        set_negotiated_format(impl_->negotiated_format());
    }
    return status;
#else
    (void)error;
    return plugins::kErrUnsupported;
#endif
}

void ScreenCaptureKitCapture::platform_close_device() noexcept {
#if NIMRTC_SCK_AVAILABLE
    if (impl_) impl_->close_device();
#endif
}

plugins::CaptureCapabilities
ScreenCaptureKitCapture::platform_capabilities() const noexcept {
#if NIMRTC_SCK_AVAILABLE
    if (impl_) return impl_->capabilities();
#endif
    plugins::CaptureCapabilities caps;
    caps.backend = "macos-screencapturekit";
    return caps;
}

void ScreenCaptureKitCapture::platform_start() noexcept {
#if NIMRTC_SCK_AVAILABLE
    if (impl_) impl_->start();
#endif
}

void ScreenCaptureKitCapture::platform_stop() noexcept {
#if NIMRTC_SCK_AVAILABLE
    if (impl_) impl_->stop();
#endif
}

plugins::Status ScreenCaptureKitCapture::platform_poll_frame() noexcept {
#if NIMRTC_SCK_AVAILABLE
    if (!impl_) return plugins::kErrNotReady;
    return impl_->poll_frame();
#else
    return plugins::kErrUnsupported;
#endif
}

} // namespace nimrtc::video_capture

#endif // defined(__APPLE__)

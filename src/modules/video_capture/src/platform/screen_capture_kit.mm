// modules/video_capture/src/platform/screen_capture_kit.mm
// ----------------------------------------------------------------------------
// The macOS ScreenCaptureKit capture device (remote-desktop plan A4).
//
// This is the only file in the capture module that is Objective-C++:
// ScreenCaptureKit is an Objective-C framework, and its `SCStream`
// delivers its samples as `CMSampleBuffer`s wrapping IOSurface-backed
// `CVPixelBuffer`s.
//
// The file is compiled *only* on macOS (the Apple frameworks do not
// exist elsewhere).  The backend's portable half — the class itself,
// its capability query, its lifecycle — lives in
// `screen_capture_kit_stub.cpp`, which every other platform compiles;
// the two halves are the *same* class, split so that a non-macOS host
// can still name the backend and get a clean "unsupported" answer.
//
// See include/nimrtc/video_capture/screen_capture_kit.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/screen_capture_kit.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>

// ---------------------------------------------------------------------------
// The sample output (the Objective-C half)
//
// The interface lives at file scope: an Objective-C `@interface` cannot
// be declared inside a C++ namespace.
// ---------------------------------------------------------------------------

/**
 * @brief The `SCStreamOutput` that receives the screen samples.
 *
 * ScreenCaptureKit's Objective-C API takes an *object* conforming to
 * the `SCStreamOutput` protocol (the block form is Swift sugar).  The
 * object holds a raw pointer to the C++ device and forwards the
 * samples to it; the device owns the output object, so the pointer is
 * valid for exactly as long as the output is registered.
 */
@interface NimRTCSCKStreamOutput : NSObject <SCStreamOutput>
@property(nonatomic, assign) void* device;
@property(nonatomic, assign) void (*handler)(void*, void*);
@end

@implementation NimRTCSCKStreamOutput

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type != SCStreamOutputTypeScreen) return;
    if (sampleBuffer == nullptr) return;
    if (self.handler == nullptr) return;
    self.handler(self.device, sampleBuffer);
}

@end

namespace nimrtc::video_capture {

namespace {
/** How long the capture thread waits between polls of the running flag. */
constexpr int kPollIntervalMs = 50;

/** The device-id prefix for a display. */
constexpr const char* kDisplayIdPrefix = "sck:display:";

/** The device-id prefix for a window. */
constexpr const char* kWindowIdPrefix = "sck:window:";

/**
 * @brief The screen's backing scale factor.
 *
 * macOS draws in *points*; a Retina screen has two pixels per point.
 * ScreenCaptureKit's `SCDisplay::width` / `SCWindow::frame` are in
 * points, but the captured buffer is in pixels, so the two must be
 * reconciled — otherwise a Retina capture comes back at half its real
 * resolution.
 */
CGFloat pixel_scale_factor() {
    static const CGFloat kScale = []() -> CGFloat {
        const CGRect bounds = CGDisplayBounds(CGMainDisplayID());
        const size_t pixels = CGDisplayPixelsWide(CGMainDisplayID());
        if (bounds.size.width <= 0 || pixels == 0) return 1.0;
        return static_cast<CGFloat>(pixels) / bounds.size.width;
    }();
    return kScale > 0 ? kScale : 1.0;
}

/**
 * @brief The *pixel* size of a display.
 *
 * `SCDisplay::width` / `SCDisplay::height` are measured in **points**,
 * not in pixels: a Retina display reports half the pixels it actually
 * has.  A capture that negotiated the point count would come back at
 * half resolution on every Retina machine, so the display's *mode* is
 * asked for instead — `CGDisplayCopyDisplayMode` reports the true
 * pixel width and height.
 *
 * @return The pixel size, or `CGSizeZero` if the mode is unavailable.
 */
CGSize display_pixel_size(SCDisplay* display) {
    if (display == nil) return CGSizeZero;

    // The mode is the authoritative source: it is the size the display
    // is actually rendering at (which is not the point size when the
    // display runs at a scaled resolution).
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display.displayID);
    if (mode != nullptr) {
        const size_t w = CGDisplayModeGetPixelWidth(mode);
        const size_t h = CGDisplayModeGetPixelHeight(mode);
        CGDisplayModeRelease(mode);
        if (w > 0 && h > 0) {
            return CGSizeMake(static_cast<CGFloat>(w),
                              static_cast<CGFloat>(h));
        }
    }

    // The fallback is the point size scaled by the backing scale
    // factor, which is what the mode would have reported anyway.
    const CGFloat scale = pixel_scale_factor();
    return CGSizeMake(display.width * scale, display.height * scale);
}

/**
 * @brief The screen-recording permission state.
 *
 * `CGPreflightScreenCaptureAccess()` reports whether the *host
 * application* already holds the Screen Recording permission.  It never
 * prompts.
 */
bool preflight_permission() {
    return CGPreflightScreenCaptureAccess() != 0;
}

/**
 * @brief Ask for the Screen Recording permission.
 *
 * This *does* prompt: macOS shows the system dialog.  It must only be
 * called from `open()`, where prompting is the user's explicit intent
 * to capture something 鈥?never from an enumeration.
 */
bool request_permission() {
    // The request itself does not grant the permission: on macOS 10.15
    // the user must also add the application in *System Settings 鈫?    // Privacy & Security 鈫?Screen Recording*, which the prompt tells
    // them about.  Re-preflighting tells us whether they did.
    if (CGRequestScreenCaptureAccess()) return true;
    return preflight_permission();
}

/** The UTF-8 text of an NSString (never null). */
std::string utf8(NSString* s) {
    if (s == nil) return std::string();
    const char* c = s.UTF8String;
    return c != nullptr ? std::string(c) : std::string();
}

/**
 * @brief The shareable content, and the displays and windows in it.
 *
 * ## Why the content is *copied*, not retained
 *
 * `SCShareableContent` is handed to the completion handler as an
 * autoreleased object, and this file is compiled *without* ARC (the
 * backend has no business imposing an ownership model on the host's
 * build).  Under manual retain/release an autoreleased object is only
 * guaranteed to live until the enclosing autorelease pool drains — and
 * the pool that drains is the one the completion handler's queue owns,
 * which is long gone by the time `dispatch_semaphore_wait` returns.
 *
 * Accessing `content.displays` after that point is a use-after-free:
 * the XPC reply that backs the array is gone, and the property access
 * dereferences freed memory.  On macOS 14 and later that shows up as
 * `EXC_BAD_ACCESS` inside `objc_msgSend` — a segfault in a library that
 * did nothing wrong.
 *
 * The fix is to take *our own* reference to the data while the
 * completion handler's scope is still alive: the arrays are copied out
 * of the content inside the handler, and the copies are what the
 * callers work with.  A copy of an `NSArray` is an immutable
 * snapshot, so it stays valid for as long as we hold it.
 */
struct ShareableContent {
    /** The displays, copied out of the content while it was alive. */
    NSArray<SCDisplay*>* displays = nil;

    /** The windows, copied out of the content while it was alive. */
    NSArray<SCWindow*>* windows = nil;
};

/**
 * @brief Fetch the shareable content, synchronously.
 *
 * `SCShareableContent` is fetched asynchronously, but both callers need
 * the answer *before* they can act — `open()` must resolve the device
 * before it negotiates the format, and the enumeration must have the
 * list before it can return it.  The completion handler therefore
 * signals a semaphore that the caller waits on.
 *
 * The API used here is
 *
 *     + (void)getShareableContentExcludingDesktopWindows:(BOOL)
 *                            onScreenWindowsOnly:(BOOL)
 *                              completionHandler:(void(^)(SCShareableContent*,
 *                                                         NSError*));
 *
 * which is available since macOS 12.3 — the same floor as
 * ScreenCaptureKit itself, so it needs no version branch.  (Its
 * *Swift* spelling, `getExcludingDesktopWindows`, is not the
 * Objective-C selector: writing that one is a compile error.)
 *
 * @param out     Receives the displays and the windows (empty on
 *                failure).
 * @param error_text Receives the failure description (empty on success).
 * @return true iff the content was fetched.
 */
bool fetch_shareable_content(ShareableContent& out,
                             std::string& error_text) {
    out.displays = nil;
    out.windows = nil;
    error_text.clear();

    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSError* error = nil;
    __block NSArray<SCDisplay*>* displays = nil;
    __block NSArray<SCWindow*>* windows = nil;

    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                                 onScreenWindowsOnly:YES
                                  completionHandler:^(SCShareableContent* c,
                                                      NSError* e) {
        error = e;
        if (c != nil) {
            // The copy happens *here*, while the content is still
            // alive: the arrays are the content's own data, and a copy
            // is an immutable snapshot that outlives the content.
            // Reading them here and copying them out is what keeps the
            // caller from touching freed memory once the handler has
            // returned.
            displays = [c.displays copy];
            windows = [c.windows copy];
        }
        dispatch_semaphore_signal(done);
    }];

    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);

    if (displays == nil && windows == nil) {
        error_text = error != nil ? utf8(error.localizedDescription)
                                  : std::string("no error reported");
        return false;
    }

    out.displays = displays;
    out.windows = windows;
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The platform state (PIMPL)
// ---------------------------------------------------------------------------

/**
 * @brief The ScreenCaptureKit objects the device owns.
 *
 * Kept out of the public header so that the header stays free of the
 * macOS SDK (and so that the class exists on every platform).
 */
class ScreenCaptureKitState {
public:
    /** The captured display (null when a window is captured). */
    SCDisplay* display = nil;

    /** The captured window (null when a display is captured). */
    SCWindow* window = nil;

    /** The content filter (the display or the window). */
    SCContentFilter* filter = nil;

    /** The stream configuration. */
    SCStreamConfiguration* config = nil;

    /** The capture stream. */
    SCStream* stream = nil;

    /** The stream output (an `NimRTCSCKStreamOutput`). */
    NimRTCSCKStreamOutput* output = nil;

    /** The captured surface's size, in pixels. */
    std::uint32_t source_width_  = 0;
    std::uint32_t source_height_ = 0;

    /** The bounded frame queue the sample handler fills. */
    CaptureQueue queue_{2};

    /** The capture thread. */
    std::thread thread_;

    /** Set to false to ask the capture thread to stop. */
    std::atomic<bool> running_{false};

    ~ScreenCaptureKitState() {
        stop_thread();
    }

    void stop_thread() noexcept {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }
};

// ---------------------------------------------------------------------------
// ScreenCaptureKitCapture
// ---------------------------------------------------------------------------

ScreenCaptureKitCapture::ScreenCaptureKitCapture(
        plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

ScreenCaptureKitCapture::~ScreenCaptureKitCapture() {
    close();
}

bool ScreenCaptureKitCapture::available() noexcept {
    return true;
}

bool ScreenCaptureKitCapture::has_permission() noexcept {
    return preflight_permission();
}

plugins::Status ScreenCaptureKitCapture::open() noexcept {
    if (open_) return plugins::kOk;

    // ---- The permission --------------------------------------------------
    //
    // This is where the prompt belongs: the user asked to capture
    // something, so asking them for the Screen Recording permission is
    // exactly what they meant.  A denial is a *distinct* outcome, not a
    // hardware error, because only the user can fix it.
    if (!preflight_permission()) {
        if (!request_permission()) {
            core::log::Logger::instance().error(
                "sck: the Screen Recording permission was not granted");
            return plugins::kErrPermissionDenied;
        }
    }

    // ---- Resolve the device ------------------------------------------------
    //
    // The shareable content is fetched asynchronously, but the call is
    // wrapped in a semaphore so that `open()` stays synchronous — the
    // seam's contract is that `open()` either succeeds or fails, and
    // the engine reads the negotiated format right after it returns.
    ShareableContent content;
    std::string content_error;
    if (!fetch_shareable_content(content, content_error)) {
        core::log::Logger::instance().error(
            "sck: the shareable content could not be fetched: " +
            content_error);
        return plugins::kErrHardwareError;
    }

    auto state = std::make_unique<ScreenCaptureKitState>();

    // The device id is either a display ("sck:display:<index>") or a
    // window ("sck:window:<window-id>").  An empty id means "the
    // primary display".
    bool want_window = false;
    std::uint64_t window_id = 0;

    if (!cfg_.device_id.empty()) {
        if (cfg_.device_id.rfind(kWindowIdPrefix, 0) == 0) {
            want_window = true;
            const std::string tail =
                cfg_.device_id.substr(std::strlen(kWindowIdPrefix));
            char* end = nullptr;
            window_id = static_cast<std::uint64_t>(
                std::strtoull(tail.c_str(), &end, 10));
            if (end == nullptr || *end != '\0' || window_id == 0) {
                core::log::Logger::instance().error(
                    "sck: '" + cfg_.device_id + "' is not a window id");
                return plugins::kErrInvalidParam;
            }
        } else if (cfg_.device_id.rfind(kDisplayIdPrefix, 0) == 0) {
            const std::string tail =
                cfg_.device_id.substr(std::strlen(kDisplayIdPrefix));
            char* end = nullptr;
            const long index = std::strtol(tail.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || index < 0) {
                core::log::Logger::instance().error(
                    "sck: '" + cfg_.device_id + "' is not a display id");
                return plugins::kErrInvalidParam;
            }
            const NSUInteger count = content.displays.count;
            // `index` is a signed `long` (that is what `strtol` returns)
            // and `content.displays` is indexed by an `NSUInteger`, so
            // the subscript is spelled in the array's own index type.
            // Casting at the subscript is what keeps the conversion
            // explicit -- and keeps it out of a -Wsign-conversion
            // warning, which the build treats as an error.
            const NSUInteger display_index =
                static_cast<NSUInteger>(index);
            if (display_index >= count) {
                core::log::Logger::instance().error(
                    "sck: display " + std::to_string(index) +
                    " does not exist (" + std::to_string(count) +
                    " displays)");
                return plugins::kErrInvalidParam;
            }
            state->display = content.displays[display_index];
        } else {
            core::log::Logger::instance().error(
                "sck: '" + cfg_.device_id +
                "' is not a ScreenCaptureKit device id");
            return plugins::kErrInvalidParam;
        }
    }

    if (want_window) {
        SCWindow* found = nil;
        for (SCWindow* w in content.windows) {
            if (static_cast<std::uint64_t>(w.windowID) == window_id) {
                found = w;
                break;
            }
        }
        if (found == nil) {
            core::log::Logger::instance().error(
                "sck: no window has the id " + std::to_string(window_id));
            return plugins::kErrInvalidParam;
        }
        state->window = found;
    } else if (state->display == nil) {
        // The default is the primary display.
        if (content.displays.count == 0) {
            core::log::Logger::instance().error("sck: no display to capture");
            return plugins::kErrHardwareError;
        }
        state->display = content.displays[0];
    }

    // ---- The source size ---------------------------------------------------
    //
    // The filter is built on the *display's* own pixel dimensions, so
    // that the pixels the consumer sees are the display's pixels and
    // not a resampling of them.  A requested region is cropped out of
    // the readback (see below), exactly like the Windows and X11
    // backends do.
    if (state->display != nil) {
        const CGSize px = display_pixel_size(state->display);
        state->source_width_  = static_cast<std::uint32_t>(px.width);
        state->source_height_ = static_cast<std::uint32_t>(px.height);
    } else {
        // A window's `frame` is in points; the captured buffer is in
        // pixels.
        const CGFloat scale = pixel_scale_factor();
        state->source_width_  = static_cast<std::uint32_t>(
            state->window.frame.size.width * scale);
        state->source_height_ = static_cast<std::uint32_t>(
            state->window.frame.size.height * scale);
    }
    if (state->source_width_ == 0 || state->source_height_ == 0) {
        return plugins::kErrInvalidParam;
    }

    // ---- Negotiate the format ----------------------------------------------
    cfg_.format.width  = state->source_width_;
    cfg_.format.height = state->source_height_;
    if (cfg_.format.fps == 0) cfg_.format.fps = 60;
    cfg_.format.tag = "bgra";
    // The pixels come back through a CPU copy of the IOSurface, so the
    // *seam's* path is not zero-copy.  The `CVPixelBufferRef` itself is
    // the zero-copy handle, and it is the hardware encoder's half (B10).
    cfg_.format.zero_copy = false;
    // ScreenCaptureKit hands over a whole surface every time; it has no
    // per-frame damage information a client can read.  The backend
    // therefore *computes* the changed region from the portable
    // pixel-diff, so the frames it delivers do carry the metadata.
    cfg_.format.has_dirty_rects = true;

    // The description is for diagnostics: a display has no title, so
    // its geometry stands in for one; a window has a title.
    if (state->display != nil) {
        description_ = "Display " +
            std::to_string(state->source_width_) + "x" +
            std::to_string(state->source_height_);
    } else {
        description_ = utf8(state->window.title);
        if (description_.empty()) description_ = "window";
    }

    // ---- The stream configuration ------------------------------------------
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width  = state->source_width_;
    config.height = state->source_height_;
    // BGRA is the format every desktop compositor produces natively,
    // and the portable capture layer speaks it.
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    // The colour space is deliberately left unset: ScreenCaptureKit
    // then uses the display's own colour space, which is the better
    // default — re-tagging a wide-gamut display as sRGB would lose
    // colour information for no benefit.
    // The pointer is composited by the OS into the captured surface:
    // this is why the backend reports `cursor_composited = true` and
    // the cursor layer (plan A6) is not needed on this path.
    config.showsCursor = cfg_.want_cursor ? YES : NO;
    config.queueDepth = 3;
    // The frame interval is the negotiated rate.
    config.minimumFrameInterval =
        CMTimeMake(1, static_cast<int32_t>(cfg_.format.fps));
    state->config = config;

    // ---- The content filter --------------------------------------------------
    if (state->display != nil) {
        state->filter = [[SCContentFilter alloc]
            initWithDisplay:state->display
         excludingWindows:@[]];
    } else {
        // A *desktop-independent* window filter captures the window
        // even when another window is on top of it — which is the
        // whole point of capturing a single window.
        state->filter = [[SCContentFilter alloc]
            initWithDesktopIndependentWindow:state->window];
    }

    // ---- The stream ------------------------------------------------------------
    SCStream* stream = [[SCStream alloc] initWithFilter:state->filter
                                           configuration:config
                                               delegate:nil];
    if (stream == nil) {
        core::log::Logger::instance().error(
            "sck: the capture stream could not be created");
        return plugins::kErrHardwareError;
    }
    state->stream = stream;

    // ---- The scratch buffer -----------------------------------------------------
    const std::size_t bytes =
        static_cast<std::size_t>(state->source_width_) *
        static_cast<std::size_t>(state->source_height_) * 4u;
    scratch_.assign(bytes, 0u);

    // ---- The queue -----------------------------------------------------------------
    state->queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0) state->queue_.set_depth(cfg_.queue_depth);

    // ---- The sample output ----------------------------------------------------------
    //
    // The handler is delivered on a dispatch queue the backend owns, so
    // the sample delivery never runs on the caller's thread.
    NimRTCSCKStreamOutput* output = [[NimRTCSCKStreamOutput alloc] init];
    output.device = this;
    output.handler = &ScreenCaptureKitCapture::dispatch_sample;
    state->output = output;

    dispatch_queue_t sample_queue =
        dispatch_queue_create("nimrtc.sck.samples", DISPATCH_QUEUE_SERIAL);
    NSError* add_error = nil;
    if (![stream addStreamOutput:output
                            type:SCStreamOutputTypeScreen
              sampleHandlerQueue:sample_queue
                           error:&add_error]) {
        core::log::Logger::instance().error(
            std::string{"sck: the stream output could not be added: "} +
            (add_error != nil
                 ? utf8(add_error.localizedDescription)
                 : std::string("no error reported")));
        return plugins::kErrHardwareError;
    }

    // ---- Start -----------------------------------------------------------------------
    //
    // `startCapture` is asynchronous, but `open()` must not return
    // before the capture is known to have started: the engine reads the
    // negotiated format right after `open()` returns, and a stream that
    // failed to start would otherwise surface as an empty queue instead
    // of as an error.  The completion handler therefore signals a
    // semaphore that `open()` waits on.
    // `startCaptureWithCompletionHandler:` returns *void*: it reports
    // the outcome through the handler's `NSError`, not through a
    // return value.  (There is no BOOL to test here -- asking for one
    // is a compile error, and the error is the only signal there is.)
    __block NSError* start_error = nil;
    dispatch_semaphore_t started = dispatch_semaphore_create(0);
    [stream startCaptureWithCompletionHandler:^(NSError* e) {
        start_error = e;
        dispatch_semaphore_signal(started);
    }];
    dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
    if (start_error != nil) {
        core::log::Logger::instance().error(
            std::string{"sck: the capture could not be started: "} +
            utf8(start_error.localizedDescription));
        return plugins::kErrHardwareError;
    }

    // ---- The capture thread ------------------------------------------------------------
    //
    // The stream delivers its samples on the dispatch queue above; the
    // capture thread's job is to keep the stream alive and to notice
    // when it stops.
    frame_index_ = 0;
    state_ = std::move(state);
    state_->running_.store(true, std::memory_order_release);
    state_->thread_ = std::thread([this]() { capture_loop(); });

    open_ = true;
    return plugins::kOk;
}

void ScreenCaptureKitCapture::close() noexcept {
    if (!open_) return;
    open_ = false;

    if (state_) {
        state_->stop_thread();

        // The stream is stopped from the thread that owns it.  The
        // dispatch queue the samples arrive on is released by the
        // stream's own teardown.
        if (state_->stream != nil) {
            [state_->stream stopCaptureWithCompletionHandler:^(NSError* e) {
                (void)e;
            }];
            state_->stream = nil;
        }
        state_->output = nil;
        state_->filter = nil;
        state_->config = nil;
        state_->display = nil;
        state_->window = nil;
    }
    state_.reset();
    scratch_.clear();
    scratch_.shrink_to_fit();
}

std::vector<plugins::CaptureDevice>
ScreenCaptureKitCapture::enumerate_devices() const noexcept {
    std::vector<plugins::CaptureDevice> devices;

    // The enumeration never prompts.  Without the Screen Recording
    // permission the shareable content cannot be fetched at all, and a
    // library that showed a modal dialog during an enumeration would be
    // unusable in a server process 鈥?so the honest answer is an empty
    // list, and `has_permission()` tells the host why.
    if (!preflight_permission()) return devices;

    ShareableContent content;
    std::string content_error;
    if (!fetch_shareable_content(content, content_error)) {
        return devices;
    }

    NSUInteger index = 0;
    for (SCDisplay* display in content.displays) {
        plugins::CaptureDevice d;
        d.id   = std::string(kDisplayIdPrefix) + std::to_string(index);
        // `SCDisplay` has no title — only an identifier and a geometry.
        // The device id therefore carries the *index*, which is what
        // `open()` resolves, and the name carries the geometry, which
        // is what a human recognises ("Display 1, 2560x1440").
        const CGSize px = display_pixel_size(display);
        d.name = "Display " + std::to_string(index) + " (" +
                 std::to_string(static_cast<std::uint32_t>(px.width)) + "x" +
                 std::to_string(static_cast<std::uint32_t>(px.height)) + ")";
        d.kind = plugins::CaptureDeviceKind::kScreen;
        d.max_width  = static_cast<std::uint32_t>(px.width);
        d.max_height = static_cast<std::uint32_t>(px.height);
        // ScreenCaptureKit does not publish a maximum frame rate: the
        // stream delivers frames as fast as the display refreshes, up
        // to the configured minimum frame interval.
        d.max_fps = 60;
        devices.push_back(std::move(d));
        ++index;
    }

    for (SCWindow* window in content.windows) {
        plugins::CaptureDevice d;
        // The window's *id* (a `CGWindowID`) is stable for the life of
        // the window, and — unlike the title — it is unique.  Two
        // windows can share a title; they cannot share an id.  The id
        // is therefore the device id, and the title is the name.
        d.id   = std::string(kWindowIdPrefix) +
                 std::to_string(static_cast<std::uint64_t>(window.windowID));
        d.name = utf8(window.title);
        if (d.name.empty()) d.name = "window";
        d.kind = plugins::CaptureDeviceKind::kWindow;
        const CGFloat scale = pixel_scale_factor();
        d.max_width  = static_cast<std::uint32_t>(
            window.frame.size.width * scale);
        d.max_height = static_cast<std::uint32_t>(
            window.frame.size.height * scale);
        d.max_fps = 60;
        devices.push_back(std::move(d));
    }

    return devices;
}

std::vector<plugins::CaptureFormat>
ScreenCaptureKitCapture::supported_formats(
        const std::string& device_id) const noexcept {
    std::vector<plugins::CaptureFormat> out;
    for (const auto& d : enumerate_devices()) {
        if (!device_id.empty() && d.id != device_id) continue;
        plugins::CaptureFormat f;
        f.width  = d.max_width;
        f.height = d.max_height;
        f.fps    = d.max_fps;
        f.tag    = "bgra";
        f.zero_copy = false;
        f.has_dirty_rects = true;
        out.push_back(std::move(f));
    }
    return out;
}

plugins::CaptureCapabilities
ScreenCaptureKitCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    // The frame arrives as an IOSurface-backed CVPixelBuffer.  The seam
    // delivers CPU bytes, so the *seam's* path is not zero-copy; the
    // CVPixelBufferRef is the handle the hardware path (B10) consumes.
    c.zero_copy = false;
    // The backend *delivers* per-frame dirty-rectangle metadata: the
    // changed region comes from the portable pixel-diff, because
    // ScreenCaptureKit hands over a whole surface every time.
    c.dirty_rects = true;
    // The OS composites the pointer into the frame.
    c.cursor_composited = true;
    // The backend crops a region out of the readback itself.
    c.region_selection = true;
    // The backend enumerates displays and windows.
    c.enumeration = true;
    c.max_pixels = 0;
    c.backend = "sck-capture";
    return c;
}

plugins::Status ScreenCaptureKitCapture::pop_frame(
        plugins::CaptureFrame& out) noexcept {
    if (!open_ || !state_) return plugins::kErrNotReady;
    // The capture thread filled the queue; the engine's tick thread
    // only takes what is already there.  It never waits.
    if (!state_->queue_.pop(out)) {
        return plugins::kErrNotReady;
    }
    return plugins::kOk;
}

plugins::CaptureStats ScreenCaptureKitCapture::stats() const noexcept {
    plugins::CaptureStats s;
    s.frames_captured = frames_captured_.load(std::memory_order_relaxed);
    s.frames_dropped  = frames_dropped_.load(std::memory_order_relaxed);
    s.device_errors   = device_errors_.load(std::memory_order_relaxed);
    s.bytes_copied    = bytes_copied_.load(std::memory_order_relaxed);
    return s;
}

void ScreenCaptureKitCapture::capture_loop() noexcept {
    if (!state_) return;

    // The stream delivers its samples on its own dispatch queue; the
    // capture thread's job is to keep the objects alive and to notice
    // when the stream has stopped.  It does *not* poll the queue: the
    // samples are pushed by the handler.
    while (state_->running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
}

// ---------------------------------------------------------------------------
// The sample handler
// ---------------------------------------------------------------------------

void ScreenCaptureKitCapture::dispatch_sample(
        void* ctx, void* sample_buffer) noexcept {
    auto* self = static_cast<ScreenCaptureKitCapture*>(ctx);
    if (self == nullptr || sample_buffer == nullptr) return;
    if (!self->state_) return;

    auto* cms = static_cast<CMSampleBufferRef>(sample_buffer);
    CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(cms);
    if (buffer == nullptr) {
        self->device_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ---- The pixel buffer's geometry ---------------------------------------
    const std::uint32_t width =
        static_cast<std::uint32_t>(CVPixelBufferGetWidth(buffer));
    const std::uint32_t height =
        static_cast<std::uint32_t>(CVPixelBufferGetHeight(buffer));
    if (width == 0 || height == 0) {
        self->device_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // The pixel buffer's bytes are locked while they are read.  The
    // lock is released before the frame is published, so the surface
    // is not held across the consumer's work.
    CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    const void* base = CVPixelBufferGetBaseAddress(buffer);
    const std::size_t stride = CVPixelBufferGetBytesPerRow(buffer);

    if (base == nullptr) {
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        self->device_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // ---- The region crop ----------------------------------------------------
    //
    // The requested region is cropped out of the captured surface: the
    // pixels the consumer sees are the display's pixels, not a
    // resampling of them.  This mirrors the Windows and X11 backends.
    const plugins::CaptureRegion& region = self->cfg_.region;
    const bool has_region = region.width != 0 && region.height != 0;
    const std::uint32_t rx = has_region ? std::min(region.x, width) : 0u;
    const std::uint32_t ry = has_region ? std::min(region.y, height) : 0u;
    const std::uint32_t rw = has_region
        ? std::min(region.width, width - rx) : width;
    const std::uint32_t rh = has_region
        ? std::min(region.height, height - ry) : height;

    if (has_region && (rw == 0 || rh == 0)) {
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        self->device_errors_.fetch_add(1, std::memory_order_relaxed);
        core::log::Logger::instance().error(
            "sck: the requested region lies outside the captured surface");
        return;
    }

    // ---- Copy ------------------------------------------------------------------
    //
    // The scratch buffer is written on the sample-delivery thread and
    // copied by the queue, so it is free to be reused for the next
    // frame as soon as `push()` returns.
    const std::size_t row_bytes = static_cast<std::size_t>(rw) * 4u;
    const std::size_t needed = row_bytes * static_cast<std::size_t>(rh);
    if (self->scratch_.size() < needed) {
        self->scratch_.resize(needed);
    }

    const auto* src = static_cast<const std::uint8_t*>(base);
    for (std::uint32_t row = 0; row < rh; ++row) {
        std::memcpy(
            self->scratch_.data() + static_cast<std::size_t>(row) * row_bytes,
            src + static_cast<std::size_t>(ry + row) * stride +
                static_cast<std::size_t>(rx) * 4u,
            row_bytes);
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);

    self->bytes_copied_.fetch_add(needed, std::memory_order_relaxed);

    // ---- The timestamp ----------------------------------------------------------
    const std::int64_t ts =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    // ---- The dirty rect ------------------------------------------------------------
    //
    // ScreenCaptureKit has no per-frame damage information a client can
    // read, so the changed region is computed by comparing the frame
    // with the previous one (remote-desktop plan A7).  An unchanged
    // frame is not worth queuing at all.
    plugins::CaptureDirtyRects dirty;
    {
        const bool identical = self->dirty_detector_.compare(
            self->scratch_.data(), needed, static_cast<std::int32_t>(row_bytes),
            rw, rh, 4, dirty);
        if (identical) {
            return;
        }
    }

    // ---- Push ------------------------------------------------------------------------
    const std::uint32_t seq = self->frame_index_++;
    const auto outcome = self->state_->queue_.push(
        self->scratch_.data(), needed, static_cast<std::int32_t>(row_bytes),
        rw, rh, ts, seq, dirty);
    switch (outcome) {
        case CaptureQueue::PushResult::kQueued:
            self->frames_captured_.fetch_add(1, std::memory_order_relaxed);
            break;
        case CaptureQueue::PushResult::kDroppedOldest:
            self->frames_captured_.fetch_add(1, std::memory_order_relaxed);
            self->frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            break;
        case CaptureQueue::PushResult::kRejected:
            self->frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            break;
    }

    // ---- The callback ------------------------------------------------------------------
    if (self->cb_) {
        plugins::CaptureFrame view;
        if (self->state_->queue_.peek(view)) {
            view.has_gpu_handle = false;
            self->cb_(view);
        }
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_screen_capture_kit() noexcept {
    static const ScreenCaptureKitFactory kFactory{};
    core::PluginRegistry::instance().register_video_capture(kFactory.id(),
                                                            &kFactory);
}

} // namespace nimrtc::video_capture

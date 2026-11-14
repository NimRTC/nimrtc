/**
 * @file src/modules/video_sink/src/metal_renderer.cpp
 * @brief The macOS Metal / AVSampleBufferDisplayLayer presentation sink.
 *
 * See include/nimrtc/video_sink/metal_renderer.hpp for the contract.
 *
 * ## The structure of this file
 *
 *   - `MetalRenderer::Impl` — the Metal device / command queue, the
 *     presentation surface (`CAMetalLayer` or
 *     `AVSampleBufferDisplayLayer`), the presenter core and the counters.
 *   - `MetalRenderer` — the `plugins::IVideoSink` face.
 *
 * ## Why the presenter core is shared
 *
 * The mailbox, the presentation clock and the drop accounting are *the
 * same* on macOS as on Windows.  They therefore live in the portable
 * `PresenterCore` (`presenter_core.hpp`) and are unit-tested on every
 * platform.  This file only supplies the `present()` callback — the half
 * that touches Metal / AVFoundation.
 *
 * ## Compilation
 *
 * This translation unit uses Objective-C++ (it needs `id<MTLDevice>`,
 * `CAMetalLayer`, `CMSampleBuffer`, ...).  CMake compiles it with the
 * `OBJCXX` language and `-fobjc-arc` on Apple platforms; every other
 * platform compiles the file as plain C++ and gets the "unsupported"
 * stub (the same shape the D3D11 renderer has on a non-Windows build).
 *
 * @note Remote-desktop plan E1 / E3 / E5 / E8.
 */

#include <nimrtc/video_sink/metal_renderer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_sink/presenter_core.hpp>
#include <nimrtc/video_sink/scale_to_fit.hpp>

#if defined(__APPLE__)
    #import <AVFoundation/AVFoundation.h>
    #import <AppKit/AppKit.h>
    #import <CoreMedia/CoreMedia.h>
    #import <CoreVideo/CoreVideo.h>
    #import <Foundation/Foundation.h>
    #import <Metal/Metal.h>
    #import <QuartzCore/CAMetalLayer.h>
    #import <QuartzCore/QuartzCore.h>
#endif

namespace nimrtc::video_sink {

namespace {

using Clock = std::chrono::steady_clock;

/** The default presentation rate when the configuration names none. */
constexpr std::uint32_t kDefaultPresentFps = 30;

#if defined(__APPLE__)

/**
 * The Metal shader used to blit the decoded picture into the layer.
 *
 * It is compiled from source at runtime (`newLibraryWithSource:`) rather
 * than from a `.metal` file so that the whole adapter stays in one
 * translation unit — no build-system shader compilation step, no
 * generated header.
 *
 * The vertex stage emits a full-screen triangle pair and the fragment
 * stage samples the source texture.  The destination rectangle is the
 * scale-to-fit rectangle (plan E7), so a 16:9 picture in a 4:3 window is
 * letterboxed rather than stretched.
 */
constexpr const char* kShaderSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 kDestOrigin = float2(0.0f, 0.0f);
constant float2 kDestSize   = float2(1.0f, 1.0f);

vertex VSOut nimrtc_present_vertex(uint vid [[vertex_id]],
                                   constant float4* dest [[buffer(0)]]) {
    // Two triangles covering the unit square.
    const float2 corners[6] = {
        float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(1.0f, 1.0f),
        float2(0.0f, 0.0f), float2(1.0f, 1.0f), float2(0.0f, 1.0f)
    };
    const float2 corner = corners[vid];
    // Map the unit square onto the destination rectangle.  `dest` is
    // (x, y, w, h) in normalised target coordinates with y measured
    // downwards.
    const float2 p = dest->xy + corner * dest->zw;
    VSOut out;
    out.position = float4(p.x * 2.0f - 1.0f, 1.0f - p.y * 2.0f, 0.0f, 1.0f);
    out.uv = corner;
    return out;
}

fragment float4 nimrtc_present_fragment(VSOut in [[stage_in]],
                                         texture2d<float> src [[texture(0)]],
                                         sampler smp [[sampler(0)]]) {
    return src.sample(smp, in.uv);
}
)METAL";

/** The BGRA pixel format the display surfaces use. */
constexpr OSType kBgraPixelFormat = kCVPixelFormatType_32BGRA;

#endif  // __APPLE__

} // namespace

// ===========================================================================
// MetalRenderer::Impl
// ===========================================================================

struct MetalRenderer::Impl {
    explicit Impl(plugins::VideoSinkConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    ~Impl() { stop_presenter(); }

    plugins::VideoSinkConfig cfg_;

    /// Guards `cfg_`.
    ///
    ///  The configuration is written by `set_config()` (the engine calls
    ///  it when the viewer resizes the window) and read by `config()`.
    mutable std::mutex cfg_mutex_;

    // ---- Counters (atomic — `stats()` may be read from any thread) ----
    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> frames_dropped_present_{0};
    std::atomic<std::uint64_t> frames_presented_{0};
    std::atomic<std::uint64_t> format_conversions_{0};
    std::atomic<std::uint64_t> render_errors_{0};
    std::atomic<std::int64_t>  last_render_age_us_{0};
    std::atomic<std::uint32_t> present_fps_{0};
    std::atomic<bool>          running_{false};

    /** The most recent frame's metadata (for `current_frame_info()`). */
    mutable std::mutex      info_mutex_;
    plugins::VideoFrameInfo last_info_{};

    /** The portable presenter (mailbox + clock + accounting). */
    std::unique_ptr<PresenterCore> presenter_;

#if defined(__APPLE__)
    // ---- The Metal objects ---------------------------------------------
    id<MTLDevice>       device_ = nil;
    id<MTLCommandQueue> queue_  = nil;

    /** The display surface.  Either a `CAMetalLayer` (the Metal path) or
     *  an `AVSampleBufferDisplayLayer` (the CPU path) — whichever the
     *  host's view already carries, or the one we install. */
    CAMetalLayer*                metal_layer_   = nil;
    AVSampleBufferDisplayLayer*  display_layer_ = nil;

    /** The render pipeline (built lazily on the first present). */
    id<MTLRenderPipelineState> pipeline_ = nil;
    id<MTLSamplerState>        sampler_  = nil;

    /** The offscreen target (used when there is no view). */
    id<MTLTexture> offscreen_ = nil;
    std::uint32_t  offscreen_w_ = 0;
    std::uint32_t  offscreen_h_ = 0;

    /** The staging texture the CPU path uploads through. */
    id<MTLTexture> upload_ = nil;
    std::uint32_t  upload_w_ = 0;
    std::uint32_t  upload_h_ = 0;

    /** The presentation target's size (the layer's drawable size). */
    std::uint32_t target_w_ = 0;
    std::uint32_t target_h_ = 0;
#endif

    /** The presentation cadence, in frames per second. */
    std::uint32_t target_fps() const noexcept {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        if (cfg_.target_fps > 0) return cfg_.target_fps;
        if (cfg_.max_fps > 0) return cfg_.max_fps;
        return kDefaultPresentFps;
    }

    // ---- The presenter ---------------------------------------------------

    void start_presenter();
    void stop_presenter();

    /** Present one frame from the mailbox. */
    bool present_one(const PresentedFrame& frame);

#if defined(__APPLE__)
    /** Create the Metal device, the queue and the display surface. */
    bool create_metal_objects();

    /** Release every Metal / AVFoundation object. */
    void destroy_metal_objects();

    /** Build the render pipeline (lazily — it needs the device). */
    bool ensure_pipeline();

    /** Present the BGRA picture through the AVSampleBufferDisplayLayer. */
    bool present_via_avsamplebuffer(const PresentedFrame& frame);

    /** Present the BGRA picture through Metal. */
    bool present_via_metal(const PresentedFrame& frame);

    /** Upload the BGRA picture into the staging texture. */
    bool upload_bgra(const PresentedFrame& frame);

    /** Resize the offscreen target to the given geometry. */
    bool resize_offscreen(std::uint32_t width, std::uint32_t height);
#endif
};

// ===========================================================================
// The presenter thread
// ===========================================================================

void MetalRenderer::Impl::start_presenter() {
    if (running_.exchange(true)) return;
    presenter_ = std::make_unique<PresenterCore>(
        target_fps(),
        [this](PresentedFrame& frame) { return present_one(frame); });
    presenter_->start();
}
void MetalRenderer::Impl::stop_presenter() {
    if (presenter_) {
        presenter_->stop();
        presenter_.reset();
    }
    running_.store(false);
}

bool MetalRenderer::Impl::present_one(const PresentedFrame& frame) {
    if (frame.pixels.empty() || frame.width == 0 || frame.height == 0) {
        return false;
    }
#if defined(__APPLE__)
    // The `AVSampleBufferDisplayLayer` is the CPU path: it takes a
    // `CMSampleBuffer`.  The Metal path is the GPU path.
    if (display_layer_ != nil) return present_via_avsamplebuffer(frame);
    if (device_ != nil) return present_via_metal(frame);
    return false;
#else
    (void)frame;
    return true;
#endif
}

// ===========================================================================
// The Apple implementation
// ===========================================================================

#if defined(__APPLE__)

bool MetalRenderer::Impl::create_metal_objects() {
    // ---- The device -------------------------------------------------------
    device_ = MTLCreateSystemDefaultDevice();
    if (device_ == nil) {
        NIMRTC_LOG_ERROR("metal renderer: no Metal device on this machine");
        return false;
    }
    queue_ = [device_ newCommandQueue];
    if (queue_ == nil) {
        NIMRTC_LOG_ERROR("metal renderer: newCommandQueue failed");
        destroy_metal_objects();
        return false;
    }

    // ---- The display surface ----------------------------------------------
    //
    // `native_window` is an `NSView*`.  If the view already carries a
    // `CAMetalLayer` we draw into it; if it carries an
    // `AVSampleBufferDisplayLayer` we use that (the CPU path); otherwise
    // we install a `CAMetalLayer` of our own.
    const bool want_surface = !cfg_.offscreen && cfg_.native_window != nullptr;

    if (want_surface) {
        NSView* view = static_cast<NSView*>(cfg_.native_window);
        CALayer* layer = [view layer];

        if ([layer isKindOfClass:[CAMetalLayer class]]) {
            metal_layer_ = static_cast<CAMetalLayer*>(layer);
        } else if ([layer isKindOfClass:[AVSampleBufferDisplayLayer class]]) {
            display_layer_ =
                static_cast<AVSampleBufferDisplayLayer*>(layer);
        } else {
            CAMetalLayer* created = [CAMetalLayer layer];
            if (created == nil) {
                NIMRTC_LOG_ERROR("metal renderer: CAMetalLayer creation failed");
                destroy_metal_objects();
                return false;
            }
            [view setWantsLayer:YES];
            [view setLayer:created];
            metal_layer_ = created;
        }

        const CGRect bounds = [view bounds];
        target_w_ = static_cast<std::uint32_t>(std::max<CGFloat>(bounds.size.width, 1.0));
        target_h_ = static_cast<std::uint32_t>(std::max<CGFloat>(bounds.size.height, 1.0));

        if (metal_layer_ != nil) {
            [metal_layer_ setDevice:device_];
            [metal_layer_ setPixelFormat:MTLPixelFormatBGRA8Unorm];
            [metal_layer_ setDrawableSize:CGSizeMake(target_w_, target_h_)];
        }
        if (display_layer_ != nil) {
            [display_layer_ setVideoGravity:AVLayerVideoGravityResizeAspect];
        }
        NIMRTC_LOG_INFO("metal renderer: display surface created ("
                        << target_w_ << "x" << target_h_ << ")");
    } else {
        // The offscreen mode.  Its size follows the configuration; the
        // renderer resizes it when a frame of a different size arrives.
        const std::uint32_t w = cfg_.width  > 0 ? cfg_.width  : 1280;
        const std::uint32_t h = cfg_.height > 0 ? cfg_.height : 720;
        if (!resize_offscreen(w, h)) {
            destroy_metal_objects();
            return false;
        }
        NIMRTC_LOG_INFO("metal renderer: offscreen target created ("
                        << w << "x" << h << ")");
    }
    return true;
}

void MetalRenderer::Impl::destroy_metal_objects() {
    pipeline_ = nil;
    sampler_  = nil;
    offscreen_ = nil;
    upload_   = nil;
    metal_layer_ = nil;
    display_layer_ = nil;
    queue_  = nil;
    device_ = nil;
}

bool MetalRenderer::Impl::ensure_pipeline() {
    if (pipeline_ != nil) return true;
    if (device_ == nil) return false;

    NSError* error = nil;
    id<MTLLibrary> library =
        [device_ newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                              options:nil
                                error:&error];
    if (library == nil) {
        NIMRTC_LOG_ERROR("metal renderer: shader compilation failed ("
                         << (error != nil ? [[error localizedDescription] UTF8String]
                                          : "unknown") << ")");
        return false;
    }

    id<MTLFunction> vertex_fn =
        [library newFunctionWithName:@"nimrtc_present_vertex"];
    id<MTLFunction> fragment_fn =
        [library newFunctionWithName:@"nimrtc_present_fragment"];
    if (vertex_fn == nil || fragment_fn == nil) {
        NIMRTC_LOG_ERROR("metal renderer: shader entry point missing");
        return false;
    }

    MTLRenderPipelineDescriptor* desc =
        [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction   = vertex_fn;
    desc.fragmentFunction = fragment_fn;
    [desc.colorAttachments[0] setPixelFormat:MTLPixelFormatBGRA8Unorm];

    pipeline_ = [device_ newRenderPipelineStateWithDescriptor:desc error:&error];
    if (pipeline_ == nil) {
        NIMRTC_LOG_ERROR("metal renderer: pipeline creation failed");
        return false;
    }

    MTLSamplerDescriptor* sampler_desc = [[MTLSamplerDescriptor alloc] init];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_ = [device_ newSamplerStateWithDescriptor:sampler_desc];

    return true;
}

bool MetalRenderer::Impl::resize_offscreen(std::uint32_t width,
                                            std::uint32_t height) {
    if (device_ == nil) return false;
    if (width == 0 || height == 0) return false;

    offscreen_ = nil;

    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:width
                                                            height:height
                                                         mipmapped:NO];
    offscreen_ = [device_ newTextureWithDescriptor:desc];
    if (offscreen_ == nil) return false;

    offscreen_w_ = width;
    offscreen_h_ = height;
    return true;
}

bool MetalRenderer::Impl::upload_bgra(const PresentedFrame& frame) {
    if (device_ == nil) return false;

    if (upload_ == nil || upload_w_ != frame.width ||
        upload_h_ != frame.height) {
        upload_ = nil;
        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                 width:frame.width
                                                                height:frame.height
                                                             mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        upload_ = [device_ newTextureWithDescriptor:desc];
        if (upload_ == nil) return false;
        upload_w_ = frame.width;
        upload_h_ = frame.height;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(frame.width) * 4u;
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        std::memcpy(static_cast<std::uint8_t*>([upload_ getBytes]) +
                        static_cast<std::ptrdiff_t>(row) * upload_w_ * 4u,
                    frame.pixels.data() +
                        static_cast<std::ptrdiff_t>(row) * row_bytes,
                    row_bytes);
    }
    return true;
}

bool MetalRenderer::Impl::present_via_avsamplebuffer(const PresentedFrame& frame) {
    if (display_layer_ == nil) return false;

    // ---- The pixel buffer -------------------------------------------------
    CVPixelBufferRef pixel_buffer = nullptr;
    const CVPixelBufferAttributes attrs[] = {
        {kCVPixelBufferIOSurfacePropertiesKey, nullptr},
        {kCVPixelBufferOpenGLESCompatibilityKey, kCFBooleanTrue},
        {kCVPixelBufferCGBitmapContextCompatibilityKey, kCFBooleanTrue},
    };
    const CVReturn cv_rc = CVPixelBufferCreate(
        kCFAllocatorDefault, frame.width, frame.height, kBgraPixelFormat,
        attrs, &pixel_buffer);
    if (cv_rc != kCVReturnSuccess || pixel_buffer == nullptr) {
        NIMRTC_LOG_ERROR("metal renderer: CVPixelBufferCreate failed ("
                         << cv_rc << ")");
        return false;
    }

    if (CVPixelBufferLockBaseAddress(pixel_buffer, 0) != kCVReturnSuccess) {
        CVPixelBufferRelease(pixel_buffer);
        return false;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(frame.width) * 4u;
    auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
    const std::size_t base_stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        std::memcpy(base + static_cast<std::size_t>(row) * base_stride,
                    frame.pixels.data() +
                        static_cast<std::ptrdiff_t>(row) * row_bytes,
                    row_bytes);
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

    // ---- The sample buffer ------------------------------------------------
    CMVideoFormatDescriptionRef format_desc = nullptr;
    if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault,
                                                      pixel_buffer,
                                                      &format_desc) !=
        noErr) {
        CVPixelBufferRelease(pixel_buffer);
        return false;
    }

    CMSampleBufferRef sample_buffer = nullptr;
    const CMSampleTiming timing = {
        .duration = CMTimeMake(1, 30),
        .presentationTimeStamp = CMTimeMake(
            static_cast<std::int64_t>(frame.capture_ts_us), 1000000),
        .decodeTimeStamp = kCMTimeInvalid,
    };
    const OSStatus rc = CMSampleBufferCreateReadyWithImageBuffer(
        kCFAllocatorDefault, pixel_buffer, format_desc, &timing,
        &sample_buffer);
    CFRelease(format_desc);
    CVPixelBufferRelease(pixel_buffer);
    if (rc != noErr || sample_buffer == nullptr) {
        NIMRTC_LOG_ERROR("metal renderer: CMSampleBufferCreate failed ("
                         << rc << ")");
        return false;
    }

    // The layer enqueues the sample; it is released by the layer.
    if ([display_layer_ status] == AVQueuedBufferStatusFull) {
        // The layer is full: drop the *oldest* sample so the picture
        // stays live (the same latest-wins policy as the mailbox).
        [display_layer_ flush];
    }
    [display_layer_ enqueueSampleBuffer:sample_buffer];
    CFRelease(sample_buffer);
    return true;
}

bool MetalRenderer::Impl::present_via_metal(const PresentedFrame& frame) {
    if (device_ == nil || queue_ == nil) return false;
    if (!ensure_pipeline()) return false;
    if (!upload_bgra(frame)) return false;

    // ---- The render target ------------------------------------------------
    id<MTLTexture> target = nil;
    if (metal_layer_ != nil) {
        // The layer's drawable: its size follows the window, and it is
        // only valid for the duration of this frame.
        id<CAMetalDrawable> drawable = [metal_layer_ nextDrawable];
        if (drawable == nil) {
            // The layer is out of drawables: the frame is dropped (the
            // next one will be shown instead — the latest-wins policy).
            return false;
        }
        target = [drawable texture];
    } else {
        // The offscreen mode.
        if (offscreen_ == nil ||
            offscreen_w_ != frame.width || offscreen_h_ != frame.height) {
            if (!resize_offscreen(frame.width, frame.height)) return false;
        }
        target = offscreen_;
    }

    const std::uint32_t target_w = target != nil ? [target width]  : 0u;
    const std::uint32_t target_h = target != nil ? [target height] : 0u;
    if (target_w == 0 || target_h == 0) return false;

    // ---- The scale-to-fit rectangle (plan E7) ------------------------------
    const ScaleRect dest = compute_scale_to_fit(
        static_cast<std::int32_t>(frame.width),
        static_cast<std::int32_t>(frame.height),
        static_cast<std::int32_t>(target_w),
        static_cast<std::int32_t>(target_h),
        ScaleMode::kLetterbox);
    if (dest.w <= 0 || dest.h <= 0) return false;

    // ---- The command buffer -----------------------------------------------
    id<MTLCommandBuffer> command_buffer = [queue_ commandBuffer];
    if (command_buffer == nil) return false;

    MTLRenderPassDescriptor* pass_desc =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass_desc.colorAttachments[0].texture     = target;
    pass_desc.colorAttachments[0].loadAction  = MTLLoadActionClear;
    pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass_desc.colorAttachments[0].clearColor =
        MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass_desc];
    if (encoder == nil) return false;

    [encoder setRenderPipelineState:pipeline_];
    if (sampler_ != nil) [encoder setFragmentSamplerState:sampler_ atIndex:0];
    [encoder setFragmentTexture:upload_ atIndex:0];

    // The destination rectangle, in normalised coordinates.
    const float dest_rect[4] = {
        static_cast<float>(dest.x) / static_cast<float>(target_w),
        static_cast<float>(dest.y) / static_cast<float>(target_h),
        static_cast<float>(dest.w) / static_cast<float>(target_w),
        static_cast<float>(dest.h) / static_cast<float>(target_h),
    };
    [encoder setVertexBytes:dest_rect
                      length:sizeof(dest_rect)
                    atIndex:0];

    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:6];
    [encoder endEncoding];

    // The command buffer is committed with the *non-blocking* policy:
    // the pacing is the presentation clock, not the drawable's
    // availability.
    [command_buffer commit];
    return true;
}

#endif  // __APPLE__

// ===========================================================================
// MetalRenderer
// ===========================================================================

MetalRenderer::MetalRenderer(plugins::VideoSinkConfig cfg) noexcept
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

MetalRenderer::~MetalRenderer() {
    if (impl_) impl_->stop_presenter();
}

const char* MetalRenderer::name() const noexcept {
    return "nimrtc::video_sink::MetalRenderer";
}

plugins::Status MetalRenderer::open() noexcept {
#if defined(__APPLE__)
    if (!impl_->create_metal_objects()) {
        impl_->destroy_metal_objects();
        return plugins::kErrHardwareError;
    }
    impl_->start_presenter();
    return plugins::kOk;
#else
    // The seam still exists on a non-Apple build so a configuration that
    // names it resolves — but the renderer reports honestly that it
    // cannot run here rather than pretending to present.
    NIMRTC_LOG_ERROR("metal renderer: this build has no Metal support");
    return plugins::kErrUnsupported;
#endif
}

void MetalRenderer::close() noexcept {
    impl_->stop_presenter();
#if defined(__APPLE__)
    impl_->destroy_metal_objects();
#endif
}

plugins::Status MetalRenderer::render(
    const plugins::VideoSourceFrame& frame) noexcept {
    if (!impl_ || !impl_->running_.load(std::memory_order_acquire)) {
        return plugins::kErrNotReady;
    }
    if (frame.width == 0 || frame.height == 0 || !frame.plane_y) {
        impl_->render_errors_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kErrInvalidParam;
    }

    // The source frame's format.  A packed BGRA frame is uploaded as-is;
    // an I420 / NV12 frame is converted to BGRA first (the display
    // surface's format).
    const bool source_is_bgra =
        frame.format == plugins::VideoPixelFormat::kBGRA ||
        frame.format == plugins::VideoPixelFormat::kUnknown;

    std::vector<std::uint8_t> bgra;
    const std::uint8_t* src = frame.plane_y;
    std::int32_t src_stride = frame.stride_y > 0
        ? frame.stride_y
        : static_cast<std::int32_t>(frame.width * 4u);

    if (!source_is_bgra) {
        const auto layout = video_frame::compute_layout(
            plugins::VideoPixelFormat::kBGRA, frame.width, frame.height);
        bgra.resize(layout.buffer_size);
        if (!frame.plane_u || !frame.plane_v) {
            impl_->render_errors_.fetch_add(1, std::memory_order_relaxed);
            return plugins::kErrInvalidParam;
        }
        const std::int32_t stride_c = frame.stride_u > 0
            ? frame.stride_u
            : static_cast<std::int32_t>((frame.width + 1u) / 2u);
        video_frame::convert_i420_to_bgra(
            frame.width, frame.height,
            frame.plane_y,
            frame.stride_y > 0 ? frame.stride_y
                              : static_cast<std::int32_t>(frame.width),
            frame.plane_u, stride_c,
            frame.plane_v, stride_c,
            bgra.data(), layout.strides[0]);
        impl_->format_conversions_.fetch_add(1, std::memory_order_relaxed);
        src = bgra.data();
        src_stride = layout.strides[0];
    }

    // ---- Post the frame to the mailbox (latest-wins) ----------------------
    PresentedFrame presented;
    presented.pixels.assign(
        src, src + static_cast<std::ptrdiff_t>(src_stride) * frame.height);
    presented.width         = frame.width;
    presented.height        = frame.height;
    presented.stride        = static_cast<std::uint32_t>(src_stride);
    presented.capture_ts_us = frame.capture_ts_us;
    presented.info.capture_ts_us = frame.capture_ts_us;
    presented.info.frame_seq     = frame.frame_seq;
    presented.info.rtp_timestamp = frame.rtp_timestamp;

    const plugins::Status rc =
        impl_->presenter_ ? impl_->presenter_->submit(std::move(presented))
                          : plugins::kErrNotReady;
    if (rc != plugins::kOk) {
        impl_->render_errors_.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }

    impl_->frames_rendered_.fetch_add(1, std::memory_order_relaxed);

    const auto now_us = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
    impl_->last_render_age_us_.store(now_us - frame.capture_ts_us,
                                      std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(impl_->info_mutex_);
        impl_->last_info_.capture_ts_us = frame.capture_ts_us;
        impl_->last_info_.frame_seq     = frame.frame_seq;
        impl_->last_info_.rtp_timestamp = frame.rtp_timestamp;
        impl_->last_info_.render_time_us = now_us;
    }
    return plugins::kOk;
}

plugins::Status MetalRenderer::render(
    const plugins::GpuBuffer& surface) noexcept {
#if defined(__APPLE__)
    if (!impl_ || !impl_->running_.load(std::memory_order_acquire)) {
        return plugins::kErrNotReady;
    }
    if (!surface.has_gpu_handle()) return plugins::kErrInvalidParam;

    const auto backend = surface.handle().backend;
    if (backend != plugins::GpuBackend::kAppleMTLTexture &&
        backend != plugins::GpuBackend::kAppleCFPixelBuffer &&
        backend != plugins::GpuBackend::kMetalIOSurface) {
        return plugins::kErrUnsupported;
    }
    if (surface.handle().handle == nullptr) return plugins::kErrInvalidParam;

    // The GPU path.  A `CVPixelBuffer` is handed to the
    // `AVSampleBufferDisplayLayer` (which accepts one directly); a Metal
    // texture is drawn into the layer's drawable.
    if (backend == plugins::GpuBackend::kAppleCFPixelBuffer) {
        auto* pixel_buffer =
            static_cast<CVPixelBuffer*>(surface.handle().handle);
        if (pixel_buffer == nullptr) return plugins::kErrInvalidParam;
        if (impl_->display_layer_ == nil) return plugins::kErrUnsupported;
        CMSampleBufferRef sample_buffer = nullptr;
        CMVideoFormatDescriptionRef format_desc = nullptr;
        if (CMVideoFormatDescriptionCreateForImageBuffer(
                kCFAllocatorDefault, pixel_buffer, &format_desc) != noErr) {
            return plugins::kErrHardwareError;
        }
        const CMSampleTiming timing = {
            .duration = CMTimeMake(1, 30),
            .presentationTimeStamp = CMTimeMake(
                static_cast<std::int64_t>(surface.handle().native_pitch_bytes),
                1000000),
            .decodeTimeStamp = kCMTimeInvalid,
        };
        const OSStatus rc = CMSampleBufferCreateReadyWithImageBuffer(
            kCFAllocatorDefault, pixel_buffer, format_desc, &timing,
            &sample_buffer);
        CFRelease(format_desc);
        if (rc != noErr || sample_buffer == nullptr) {
            return plugins::kErrHardwareError;
        }
        [impl_->display_layer_ enqueueSampleBuffer:sample_buffer];
        CFRelease(sample_buffer);

        impl_->frames_rendered_.fetch_add(1, std::memory_order_relaxed);
        impl_->frames_presented_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kOk;
    }

    // A Metal texture / IOSurface: draw it into the layer's drawable.
    if (impl_->metal_layer_ == nil || impl_->device_ == nil) {
        return plugins::kErrUnsupported;
    }
    if (!impl_->ensure_pipeline()) return plugins::kErrHardwareError;

    id<CAMetalDrawable> drawable = [impl_->metal_layer_ nextDrawable];
    if (drawable == nil) {
        impl_->frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kErrHardwareError;
    }

    id<MTLCommandBuffer> command_buffer = [impl_->queue_ commandBuffer];
    if (command_buffer == nil) return plugins::kErrHardwareError;

    MTLRenderPassDescriptor* pass_desc =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass_desc.colorAttachments[0].texture     = [drawable texture];
    pass_desc.colorAttachments[0].loadAction  = MTLLoadActionClear;
    pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass_desc.colorAttachments[0].clearColor =
        MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass_desc];
    if (encoder == nil) return plugins::kErrHardwareError;

    [encoder setRenderPipelineState:impl_->pipeline_];
    if (impl_->sampler_ != nil) {
        [encoder setFragmentSamplerState:impl_->sampler_ atIndex:0];
    }
    [encoder setFragmentTexture:
                 static_cast<id<MTLTexture>>(surface.handle().handle)
                            atIndex:0];
    const float dest_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    [encoder setVertexBytes:dest_rect length:sizeof(dest_rect) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [encoder endEncoding];
    [command_buffer commit];

    impl_->frames_rendered_.fetch_add(1, std::memory_order_relaxed);
    impl_->frames_presented_.fetch_add(1, std::memory_order_relaxed);
    return plugins::kOk;
#else
    (void)surface;
    return plugins::kErrUnsupported;
#endif
}

plugins::Status MetalRenderer::render(
    const plugins::GpuBuffer& surface,
    const plugins::VideoFrameInfo& info) noexcept {
    const plugins::Status rc = render(surface);
    if (rc != plugins::kOk) return rc;
    std::lock_guard<std::mutex> lock(impl_->info_mutex_);
    impl_->last_info_.capture_ts_us = info.capture_ts_us;
    impl_->last_info_.frame_seq     = info.frame_seq;
    impl_->last_info_.rtp_timestamp = info.rtp_timestamp;
    impl_->last_info_.render_time_us = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
    return plugins::kOk;
}

bool MetalRenderer::supports_gpu_render() const noexcept {
    return true;
}

bool MetalRenderer::supports_cpu_render() const noexcept {
    return true;
}

plugins::VideoSinkStats MetalRenderer::stats() const noexcept {
    plugins::VideoSinkStats s;
    s.frames_rendered = impl_->frames_rendered_.load(std::memory_order_relaxed);
    s.frames_dropped  = impl_->frames_dropped_.load(std::memory_order_relaxed);
    s.format_conversions =
        impl_->format_conversions_.load(std::memory_order_relaxed);
    s.render_errors   = impl_->render_errors_.load(std::memory_order_relaxed);
    s.last_render_age_us =
        static_cast<std::uint64_t>(impl_->last_render_age_us_.load(
            std::memory_order_relaxed));
    if (impl_->presenter_) {
        const auto core = impl_->presenter_->stats();
        s.frames_dropped_present = core.frames_dropped_present;
        s.frames_presented       = core.frames_presented;
        s.present_fps            = core.present_fps;
        s.presenter_queue_depth  = core.presenter_queue_depth;
    }
    return s;
}

plugins::VideoSinkConfig MetalRenderer::config() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->cfg_mutex_);
    return impl_->cfg_;
}

plugins::Status MetalRenderer::set_config(
        plugins::VideoSinkConfig cfg) noexcept {
    if (!impl_) return plugins::kErrNotReady;

    // The presentation geometry is a runtime property (plan E7): the
    // viewer resizes the window and the picture follows.  The drawable's
    // own size is the layer's, so the *draw* rectangle follows the new
    // configuration.
    std::lock_guard<std::mutex> lock(impl_->cfg_mutex_);
    impl_->cfg_ = std::move(cfg);

#if defined(__APPLE__)
    if (impl_->cfg_.width > 0) impl_->target_w_ = impl_->cfg_.width;
    if (impl_->cfg_.height > 0) impl_->target_h_ = impl_->cfg_.height;
#endif
    return plugins::kOk;
}

const plugins::VideoFrameInfo* MetalRenderer::current_frame_info() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->info_mutex_);
    return &impl_->last_info_;
}

bool MetalRenderer::is_hardware_accelerated() const noexcept {
    return true;
}

std::string_view MetalRenderer::hardware_backend() const noexcept {
    return "metal";
}

// ===========================================================================
// The factory
// ===========================================================================

std::string_view MetalRendererFactory::id() const noexcept {
    return "metal";
}

std::string_view MetalRendererFactory::display_name() const noexcept {
    return "macOS Metal / AVSampleBufferDisplayLayer renderer";
}

plugins::IVideoSink* MetalRendererFactory::create(
    plugins::VideoSinkConfig cfg) const {
    return new MetalRenderer(std::move(cfg));
}

} // namespace nimrtc::video_sink

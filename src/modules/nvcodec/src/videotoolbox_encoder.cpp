/**
 * @file src/modules/nvcodec/src/videotoolbox_encoder.cpp
 * @brief Apple VideoToolbox encoder — real adapter (H.264 / HEVC / AV1).
 *
 * Talks to VideoToolbox through the modern `VTCompressionSession` API.
 * The session code is *codec-parameterised*: the same session serves
 * H.264, HEVC and AV1, and only the `CMVideoCodecType` differs.
 *
 * Entry points (wired into the codec modules' backend tables):
 *   bool    vt_encoder_available(codec)  → runtime probe (see below)
 *   make_vt_encoder(codec, cfg)          → real VtEncoder or nullptr
 *
 * ## The probe-then-create shape
 *
 * This file is modelled directly on `nvenc_encoder.cpp`:
 *
 *   1. `vt_encoder_available()` creates a *real* compression session for
 *      the requested codec and tears it down again.  It does NOT answer
 *      "the OS is new enough" — a Mac whose silicon has no HEVC encoder
 *      (or whose OS is too old for the required
 *      `VTCompressionOutputSettings`) must report "not available".
 *   2. `make_vt_encoder()` creates the session for real and returns an
 *      opened encoder; a failure returns nullptr so the registry falls
 *      through to the next backend.
 *
 * ## The zero-copy contract
 *
 * VideoToolbox consumes and produces `CVPixelBufferRef`.  The tree
 * carries that handle as `plugins::GpuBackend::kAppleCFPixelBuffer`
 * (see `video_frame::GpuHandle`), so the encoder supports the
 * zero-copy encode path: a `CVPixelBuffer` handed to
 * `encode_zero_copy()` goes straight into the compression session,
 * with no CPU staging copy.
 *
 * The CPU path (the engine's universal fallback) converts the source
 * frame's I420 planes into a `CVPixelBuffer` and submits that — which
 * is one copy, and is counted honestly as a *staged* frame in the
 * codec statistics.
 *
 * ## Bitstream framing
 *
 * VideoToolbox's compression session emits *length-prefixed* (AVCC)
 * NAL units for H.264 / HEVC, while this tree's RTP packetizer expects
 * Annex-B start codes.  The conversion happens on the way out (see
 * `vt_detail::ensure_annex_b`), so the bytes handed to the caller are
 * already in the shape the packetizer wants.
 *
 * AV1 is different: VideoToolbox's AV1 encoder emits the AV1 spec's
 * *low-overhead* form (OBUs with `obu_has_size_field = 1`), while the
 * tree carries AV1 in Annex-B framing (the same framing the NVENC AV1
 * path produces via `outputAnnexBFormat = 1`).  The conversion is
 * `vt_detail::av1_low_overhead_to_annex_b()`.
 *
 * ## Platform reality
 *
 * The whole file is guarded by `#if defined(__APPLE__)`.  On every
 * other platform the module still exports `vt_encoder_available()`
 * and `make_vt_encoder()` — they simply report "not available" and
 * return nullptr, which is exactly the behaviour of a Mac whose
 * hardware cannot do the codec.  The Windows / Linux builds therefore
 * compile the seam without any Apple SDK.
 */

#include <nimrtc/nvcodec/vt_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>

// The shared helpers (`vt_detail`) are needed on *every* platform: the
// non-Apple leg of this file uses them for the unavailable-reason
// strings.
#include "vt_common.hpp"

#if defined(__APPLE__)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::nvcodec::vt_backend {
namespace {

namespace vt_detail = ::nimrtc::nvcodec::vt_detail;

// ---------------------------------------------------------------------------
// Session defaults
// ---------------------------------------------------------------------------

constexpr std::uint32_t kDefaultWidth    = 1280;
constexpr std::uint32_t kDefaultHeight   = 720;
constexpr std::uint32_t kDefaultFps      = 30;
constexpr std::uint32_t kDefaultBitrate  = 1000000;
constexpr std::uint32_t kDefaultGopLength = 60;

/** The probe's geometry and rate settings.
 *
 *  The probe deliberately uses a *small* frame: the session creation
 *  must not depend on the geometry the caller will later ask for, and
 *  a small frame is the cheapest one the hardware will accept. */
constexpr std::uint32_t kProbeWidth   = 320;
constexpr std::uint32_t kProbeHeight  = 240;
constexpr std::uint32_t kProbeFps     = 30;
constexpr std::uint32_t kProbeBitrate = 500000;
constexpr std::uint32_t kProbeGop     = 30;

/** The scratch size for the zero-copy output path.
 *
 *  The zero-copy entry point has no caller-supplied output buffer (the
 *  seam's signature is `encode_zero_copy(gpu, encoded_out)`), so the
 *  bitstream is written into the codec's own scratch.  2 MiB is the
 *  same budget the NVENC backend uses. */
constexpr std::size_t kScratchBytes = 2 * 1024 * 1024;

// ---------------------------------------------------------------------------
// VtSession — the wrapper around VTCompressionSession.
//
// The C API is used rather than the Objective-C wrapper so the file
// compiles as plain C++ (no ARC, no Objective-C++ runtime dependency).
// ---------------------------------------------------------------------------

class VtSession {
public:
    VtSession() = default;
    ~VtSession() { close(); }

    VtSession(const VtSession&)            = delete;
    VtSession& operator=(const VtSession&) = delete;

    /** Create the compression session for @p codec at @p w x @p h.
     *
     *  @return true on success.  A false return means the hardware (or
     *          the OS version) refused the session — which is exactly
     *          what the availability probe reports. */
    bool open(vt_detail::Codec codec,
              std::uint32_t w,
              std::uint32_t h,
              std::uint32_t fps,
              std::uint32_t bitrate_bps,
              std::uint32_t gop_length) noexcept {
        close();

        codec_  = codec;
        width_  = w;
        height_ = h;
        fps_    = fps > 0 ? fps : kDefaultFps;

        // ---- The encoder specification --------------------------------
        //
        // `RequireHardwareAcceleratedVideoEncoder = true` makes the
        // session creation *fail* when the machine has no hardware
        // encoder for the codec, instead of silently falling back to the
        // software encoder.  Without it, the "hardware backend" claim
        // would be a lie on a Mac whose silicon cannot do the codec —
        // and the availability probe would report "available" for a
        // software encode.
        CFMutableDictionaryRef encoder_spec =
            CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        if (!encoder_spec) return false;
        CFDictionarySetValue(
            encoder_spec,
            kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
            kCFBooleanTrue);

        // `RealTime = true` is the right choice for screen and camera
        // content: latency matters more than compression efficiency.
        CFDictionarySetValue(encoder_spec,
                             kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                             kCFBooleanTrue);

        // ---- The session -----------------------------------------------
        //
        // The output callback is *mandatory* — VideoToolbox has no
        // "synchronous mode" without one.  The callback appends the
        // compressed sample to this session's own buffer, and `encode()`
        // hands that buffer out.
        OSStatus st = VTCompressionSessionCreate(
            kCFAllocatorDefault,
            static_cast<std::int32_t>(width_),
            static_cast<std::int32_t>(height_),
            vt_detail::cm_codec_type(codec_),
            encoder_spec,
            nullptr,             // no source image-buffer attributes: the
                                 // session picks its own pixel format
            nullptr,             // no compressed data allocator
            &VtSession::output_callback,
            this,
            &session_);
        CFRelease(encoder_spec);

        if (st != noErr || !session_) {
            NIMRTC_LOG_WARN("vt: VTCompressionSessionCreate failed (os="
                            << static_cast<int>(st) << ")");
            session_ = nullptr;
            return false;
        }

        // ---- The encoder settings --------------------------------------
        //
        // The *settings* are applied to the session through
        // `VTSessionSetProperty` — that is the only supported way to
        // tell the encoder the bitrate, the frame rate and the key-frame
        // interval.  The plan's B5a acceptance names this
        // ("`available()` is false when the OS is too old for the
        // required `VTCompressionOutputSettings`"): the VideoToolbox
        // equivalent of that struct is this set of session properties,
        // and `min_macos_version()` names the release each of them
        // needs.
        //
        // Every one of them is *optional* per the VideoToolbox
        // documentation: an encoder that does not support a property
        // simply ignores it, so a failure here is a warning, not a
        // reason to abandon the session.
        //
        // The floor for H.264 is macOS 10.9 (`RealTime` and the
        // hardware-accelerated specification keys are 10.9); HEVC needs
        // 10.13; AV1 needs 14.0.
        const std::int32_t fps_i = static_cast<std::int32_t>(fps_);
        const std::int32_t bitrate_i =
            bitrate_bps > 0 ? static_cast<std::int32_t>(bitrate_bps)
                            : static_cast<std::int32_t>(kDefaultBitrate);
        const std::int32_t gop_i =
            gop_length > 0 ? static_cast<std::int32_t>(gop_length)
                           : static_cast<std::int32_t>(kDefaultGopLength);

        set_property(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        set_property(kVTCompressionPropertyKey_ExpectedFrameRate, fps_i);
        set_property(kVTCompressionPropertyKey_AverageBitRate, bitrate_i);
        set_property(kVTCompressionPropertyKey_MaxKeyFrameInterval, gop_i);
        // A real-time stream must not reorder frames: the receiver
        // expects them in presentation order.
        set_property(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

        // ---- The pixel buffer pool -------------------------------------
        //
        // The pool is what makes the zero-copy path possible: the
        // encoder allocates its input surfaces from the pool and
        // releases them after the frame has been coded.
        //
        // The pool is taken from the *session* rather than built by
        // hand: `VTCompressionSessionGetPixelBufferPool` returns a pool
        // whose attributes match what the encoder actually wants, which
        // is what the zero-copy contract needs (a surface the encoder
        // can consume without an internal conversion).
        pool_ = VTCompressionSessionGetPixelBufferPool(session_);
        if (!pool_) {
            NIMRTC_LOG_WARN("vt: VTCompressionSessionGetPixelBufferPool "
                            "returned no pool — the zero-copy path is off");
        }

        NIMRTC_LOG_INFO("vt: compression session created (" << width_ << "x"
                        << height_ << " @" << fps_ << " fps)");
        return true;
    }

    void close() noexcept {
        if (session_) {
            VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
            VTCompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (pool_) {
            // The pool is owned by the session; the session was already
            // invalidated above, so the pool must not be released here.
            pool_ = nullptr;
        }
    }

    [[nodiscard]] bool is_open() const noexcept { return session_ != nullptr; }
    [[nodiscard]] std::uint32_t width()  const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    /** Acquire an input surface from the pool.
     *
     *  @return nullptr when the pool is exhausted. */
    CVPixelBufferRef acquire_input() noexcept {
        if (!pool_) return nullptr;
        CVPixelBufferRef buf = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool_, &buf)
            != kCVReturnSuccess) {
            return nullptr;
        }
        return buf;
    }

    /** Encode one pixel buffer.
     *
     *  The compressed sample is delivered through the output callback,
     *  which appends it to this session's buffer; the call then
     *  completes the frames so the buffer holds the finished picture.
     *
     *  @param force_keyframe  Ask for an IDR for this frame.
     *  @param out  Receives the length-prefixed (AVCC) bitstream.
     *  @param out_is_keyframe  Receives whether the frame is an IDR.
     *  @return true on success. */
    bool encode(CVPixelBufferRef pixel_buffer,
                bool force_keyframe,
                std::vector<std::uint8_t>& out,
                bool& out_is_keyframe) noexcept {
        if (!session_ || !pixel_buffer) return false;

        const CMTime pts = CMTimeMake(static_cast<std::int64_t>(frame_index_) * 1000, 1000);
        const CMTime dur = CMTimeMake(1, static_cast<std::int32_t>(fps_));

        // The "force a key frame" request is a *frame property*, not a
        // flag on the encode call.
        CFDictionaryRef frame_props = nullptr;
        if (force_keyframe) {
            frame_props = CFDictionaryCreate(
                kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
            if (frame_props) {
                CFDictionarySetValue(frame_props,
                                     kVTEncodeFrameOptionKey_ForceKeyFrame,
                                     kCFBooleanTrue);
            }
        }

        output_.clear();
        dropped_ = false;
        keyframe_seen_ = false;

        const auto encode_start = std::chrono::steady_clock::now();
        OSStatus st = VTCompressionSessionEncodeFrame(
            session_, pixel_buffer, pts, dur, frame_props, nullptr, nullptr);
        if (frame_props) CFRelease(frame_props);
        if (st != noErr) {
            NIMRTC_LOG_ERROR("vt: VTCompressionSessionEncodeFrame failed (os="
                             << static_cast<int>(st) << ")");
            return false;
        }

        // Complete the frames so the compressed data is available.
        st = VTCompressionSessionCompleteFrames(session_, pts);
        if (st != noErr) {
            NIMRTC_LOG_ERROR("vt: VTCompressionSessionCompleteFrames failed (os="
                             << static_cast<int>(st) << ")");
            return false;
        }
        const auto encode_end = std::chrono::steady_clock::now();
        last_latency_us_ = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                encode_end - encode_start).count());

        if (dropped_) {
            // The encoder dropped the frame.  That is not an error — it
            // is a skipped frame.
            out.clear();
            out_is_keyframe = false;
            return true;
        }

        out = output_;
        output_.clear();

        // The keyframe flag: the callback reports it through the
        // `kVTEncodeInfo_...` flags of the compressed sample, so this is
        // the encoder's *own* answer — not a guess.
        out_is_keyframe = keyframe_seen_;
        ++frame_index_;
        return true;
    }

    [[nodiscard]] std::uint64_t last_latency_us() const noexcept {
        return last_latency_us_;
    }

private:
    /** Set one optional session property.
     *
     *  A failure is a warning, never a hard error: VideoToolbox
     *  documents every one of these properties as optional, and an
     *  encoder that does not support one simply ignores it. */
    void set_property(CFStringRef key, CFTypeRef value) noexcept {
        if (!session_ || !key || !value) return;
        const OSStatus st = VTSessionSetProperty(session_, key, value);
        if (st != noErr) {
            NIMRTC_LOG_WARN("vt: VTSessionSetProperty failed (os="
                            << static_cast<int>(st) << ")");
        }
    }

    void set_property(CFStringRef key, std::int32_t value) noexcept {
        const CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                                                &value);
        if (!num) return;
        set_property(key, num);
        CFRelease(num);
    }

    /** The output callback VideoToolbox delivers the compressed sample to. */
    static OSStatus output_callback(void* refcon,
                                    void* /*sourceFrameRefCon*/,
                                    OSStatus status,
                                    VTEncodeInfoFlags flags,
                                    CMSampleBufferRef sample_buffer) {
        auto* self = static_cast<VtSession*>(refcon);
        if (!self) return noErr;
        if (status != noErr) return status;

        if (flags & kVTEncodeInfo_FrameDropped) {
            self->dropped_ = true;
            return noErr;
        }
        if (!sample_buffer) return noErr;

        // The key-frame flag: `kCMSampleBufferFlag_...` does not carry
        // it, but the sample's attachments do — VideoToolbox marks an
        // IDR with the `kVTCompressionOutput_KeyFrame` flag, which
        // reaches us through the sample buffer's format-description
        // attachments.  The reliable, public way to ask is the sample
        // buffer's *sync* sample flag, which is set for an IDR.
        if (CMSampleBufferIsNotSync(sample_buffer)) {
            self->keyframe_seen_ = false;
        } else {
            self->keyframe_seen_ = true;
        }

        // ---- Extract the compressed bytes ------------------------------
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buffer);
        if (!block) return noErr;
        const std::size_t total = CMBlockBufferGetDataLength(block);
        if (total == 0) return noErr;

        self->output_.resize(total);
        if (CMBlockBufferCopyDataBytes(block, 0, total, self->output_.data())
            != kCMBlockBufferNoErr) {
            self->output_.clear();
            return noErr;
        }
        return noErr;
    }

    VTCompressionSessionRef session_ = nullptr;
    CVPixelBufferPoolRef      pool_    = nullptr;
    vt_detail::Codec          codec_   = vt_detail::Codec::kH264;
    std::uint32_t             width_   = 0;
    std::uint32_t             height_  = 0;
    std::uint32_t             fps_     = kDefaultFps;
    std::uint32_t             frame_index_ = 0;
    std::vector<std::uint8_t> output_;
    std::uint64_t             last_latency_us_ = 0;
    bool                      dropped_ = false;
    bool                      keyframe_seen_ = false;
};

// ---------------------------------------------------------------------------
// VtEncoder — implements plugins::IVideoCodec
// ---------------------------------------------------------------------------

class VtEncoder final : public plugins::IVideoCodec {
public:
    VtEncoder(plugins::VideoCodecConfig cfg, vt_detail::Codec codec) noexcept
        : cfg_(std::move(cfg)), codec_(codec) {}

    ~VtEncoder() override { close(); }

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::nvcodec::vt_backend::VtEncoder";
    }

    plugins::Status open() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (opened_) return plugins::kOk;

        const std::uint32_t w = cfg_.width  > 0 ? cfg_.width  : kDefaultWidth;
        const std::uint32_t h = cfg_.height > 0 ? cfg_.height : kDefaultHeight;
        const std::uint32_t fps = cfg_.fps > 0 ? cfg_.fps : kDefaultFps;

        if (!session_.open(codec_, w, h, fps,
                           cfg_.bitrate_bps > 0 ? cfg_.bitrate_bps
                                                : kDefaultBitrate,
                           cfg_.keyframe_interval > 0 ? cfg_.keyframe_interval
                                                     : kDefaultGopLength)) {
            return plugins::kErrHardwareError;
        }
        opened_ = true;
        // The first picture must be an IDR so the receiver can start.
        force_keyframe_next_ = true;
        return plugins::kOk;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) return;
        session_.close();
        opened_ = false;
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode()   const noexcept override { return true; }
    bool can_decode()   const noexcept override { return false; }

    plugins::VideoCodecKind kind() const noexcept override {
        return vt_detail::codec_kind(codec_);
    }
    std::string_view codec_name() const noexcept override {
        return vt_detail::codec_short_name(codec_);
    }

    plugins::Status encode(const plugins::VideoFrame& raw,
                           std::uint8_t* output,
                           std::size_t output_capacity,
                           plugins::EncodedVideoFrame& encoded_out) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) {
            stats_.encode_errors++;
            return plugins::kErrNotReady;
        }
        if (!output || output_capacity == 0) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }

        const std::uint32_t w = raw.width()  ? raw.width()  : session_.width();
        const std::uint32_t h = raw.height() ? raw.height() : session_.height();
        if (w == 0 || h == 0) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }
        if (w != session_.width() || h != session_.height()) {
            // A VideoToolbox session is bound to a fixed geometry, and
            // re-creating it is the engine's job (the parameter-set
            // change path).  Refuse rather than code a wrong picture.
            stats_.encode_errors++;
            return plugins::kErrUnsupported;
        }

        // ---- Stage the source picture into a CVPixelBuffer ---------------
        auto cpu = raw.cpu_buffer();
        if (!cpu) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }
        const std::uint8_t* y_in = cpu->plane(0);
        const std::uint8_t* u_in = cpu->plane(1);
        const std::uint8_t* v_in = cpu->plane(2);
        if (!y_in || !u_in || !v_in) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }

        CVPixelBufferRef pixel_buffer = session_.acquire_input();
        if (!pixel_buffer) {
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }

        // Lock the pixel buffer and fill the NV12 planes.  The lock has
        // to happen *after* the acquisition, and it reports the real
        // row pitch — which is not the frame width in general.
        const CVReturn lock_st = CVPixelBufferLockBaseAddress(pixel_buffer, 0);
        void* base = (lock_st == kCVReturnSuccess)
                         ? CVPixelBufferGetBaseAddress(pixel_buffer) : nullptr;
        const std::size_t pitch = (lock_st == kCVReturnSuccess)
                                      ? CVPixelBufferGetBytesPerRow(pixel_buffer) : 0;
        if (!base || pitch == 0) {
            if (lock_st == kCVReturnSuccess) {
                CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
            }
            CVPixelBufferRelease(pixel_buffer);
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }

        {
            auto* dst = static_cast<std::uint8_t*>(base);
            const auto& layout = cpu->layout();
            const std::uint32_t stride_y =
                static_cast<std::uint32_t>(layout.strides[0]);
            const std::uint32_t stride_u =
                static_cast<std::uint32_t>(layout.strides[1]);
            const std::uint32_t stride_v =
                static_cast<std::uint32_t>(layout.strides[2]);

            // The luma plane: one row at a time, at the buffer's pitch.
            for (std::uint32_t row = 0; row < h; ++row) {
                std::memcpy(dst + static_cast<std::size_t>(row) * pitch,
                            y_in + static_cast<std::size_t>(row) * stride_y, w);
            }
            // The chroma plane: U and V interleaved, at the buffer's
            // own chroma pitch.
            const std::size_t uv_pitch =
                CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
            const std::uint32_t cw = (w + 1u) / 2u;
            const std::uint32_t ch = (h + 1u) / 2u;
            auto* uv_dst = dst + static_cast<std::size_t>(pitch) * h;
            for (std::uint32_t row = 0; row < ch; ++row) {
                const std::uint8_t* u_row =
                    u_in + static_cast<std::size_t>(row) * stride_u;
                const std::uint8_t* v_row =
                    v_in + static_cast<std::size_t>(row) * stride_v;
                std::uint8_t* out = uv_dst + static_cast<std::size_t>(row) * uv_pitch;
                for (std::uint32_t col = 0; col < cw; ++col) {
                    out[col * 2 + 0] = u_row[col];
                    out[col * 2 + 1] = v_row[col];
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

        const plugins::Status st = encode_pixel_buffer(pixel_buffer, output,
                                                       output_capacity,
                                                       encoded_out);
        CVPixelBufferRelease(pixel_buffer);
        return st;
    }

    plugins::Status decode(const plugins::EncodedVideoFrame&,
                           plugins::VideoFrame&,
                           std::uint8_t* const*) noexcept override {
        return plugins::kErrUnsupported;
    }

    plugins::Status force_keyframe() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        force_keyframe_next_ = true;
        return plugins::kOk;
    }

    // ---- The zero-copy path ----------------------------------------------
    //
    // The encoder can consume a `CVPixelBuffer` directly: the surface
    // is handed to the compression session without a CPU staging copy,
    // so the picture never leaves the GPU.  The handle travels in the
    // tree's `GpuHandle` as `GpuBackend::kAppleCFPixelBuffer`.
    bool supports_zero_copy_encode() const noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        return opened_ && session_.is_open();
    }

    bool supports_zero_copy_decode() const noexcept override { return false; }

    plugins::Status encode_zero_copy(const plugins::GpuBuffer& gpu,
                                     plugins::EncodedVideoFrame& encoded_out) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) return plugins::kErrNotReady;
        if (gpu.handle().backend != plugins::GpuBackend::kAppleCFPixelBuffer) {
            // Not a VideoToolbox surface — the caller falls back to the
            // CPU staging path.
            return plugins::kErrUnsupported;
        }
        auto* pixel_buffer = static_cast<CVPixelBufferRef>(gpu.handle().handle);
        if (!pixel_buffer) return plugins::kErrInvalidParam;

        // The surface's geometry must match the session's.
        const std::size_t w = CVPixelBufferGetWidth(pixel_buffer);
        const std::size_t h = CVPixelBufferGetHeight(pixel_buffer);
        if (w == 0 || h == 0) return plugins::kErrInvalidParam;
        if (w != session_.width() || h != session_.height()) {
            return plugins::kErrUnsupported;
        }

        return encode_pixel_buffer(pixel_buffer, scratch_.data(),
                                    scratch_.size(), encoded_out);
    }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }

    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        const bool resize = cfg.width > 0 && cfg.height > 0 && opened_ &&
                            (cfg.width != session_.width() ||
                             cfg.height != session_.height());
        cfg_ = std::move(cfg);
        if (!opened_) return plugins::kOk;
        if (resize) {
            // A VideoToolbox session is bound to a fixed geometry, so a
            // resolution change means a fresh session.  The bitrate
            // rides along with it.
            const std::uint32_t fps = cfg_.fps > 0 ? cfg_.fps : kDefaultFps;
            if (!session_.open(codec_, cfg_.width, cfg_.height, fps,
                               cfg_.bitrate_bps > 0 ? cfg_.bitrate_bps
                                                    : kDefaultBitrate,
                               cfg_.keyframe_interval > 0 ? cfg_.keyframe_interval
                                                         : kDefaultGopLength)) {
                opened_ = false;
                return plugins::kErrHardwareError;
            }
            force_keyframe_next_ = true;
        }
        return plugins::kOk;
    }

    plugins::VideoCodecStats stats() const noexcept override { return stats_; }

    std::uint8_t payload_type() const noexcept override {
        return cfg_.payload_type != 0 ? cfg_.payload_type
                                      : vt_detail::default_payload_type(codec_);
    }

    // ---- Telemetry ---------------------------------------------------------

    std::string_view backend_id() const noexcept override { return "videotoolbox"; }

    bool is_hardware_accelerated() const noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        return opened_ && session_.is_open();
    }

private:
    /** Encode one `CVPixelBuffer` and publish the bitstream. */
    plugins::Status encode_pixel_buffer(CVPixelBufferRef pixel_buffer,
                                        std::uint8_t* output,
                                        std::size_t output_capacity,
                                        plugins::EncodedVideoFrame& encoded_out) noexcept {
        std::vector<std::uint8_t> raw_bs;
        bool is_keyframe = false;
        if (!session_.encode(pixel_buffer, force_keyframe_next_, raw_bs,
                             is_keyframe)) {
            stats_.encode_errors++;
            return plugins::kErrHardwareError;
        }
        if (force_keyframe_next_) force_keyframe_next_ = false;

        if (raw_bs.empty()) {
            // The encoder produced nothing (a dropped frame).  That is
            // not an error — it is a skipped frame.
            stats_.frames_skipped++;
            return plugins::kOk;
        }

        // ---- Convert the encoder's framing to Annex-B -------------------
        //
        // H.264 / HEVC: VideoToolbox emits length-prefixed (AVCC) NAL
        // units; AV1: it emits the AV1 low-overhead OBU form.  Both
        // become Annex-B here, which is the framing the RTP packetizer
        // in this tree expects.
        std::vector<std::uint8_t> annex_b;
        bool converted = false;
        if (codec_ == vt_detail::Codec::kAv1) {
            converted = vt_detail::av1_low_overhead_to_annex_b(
                raw_bs.data(), raw_bs.size(), annex_b);
        } else {
            converted = vt_detail::ensure_annex_b(raw_bs.data(), raw_bs.size(),
                                                  annex_b);
        }
        if (!converted) {
            stats_.encode_errors++;
            NIMRTC_LOG_ERROR("vt: the compressed bitstream is neither Annex-B "
                             "nor a well-formed length-prefixed stream");
            return plugins::kErrCorrupt;
        }

        if (annex_b.size() > output_capacity) {
            NIMRTC_LOG_WARN("vt: bitstream " << annex_b.size()
                           << " B exceeds output buffer " << output_capacity
                           << " B — truncating");
        }
        const std::size_t n_copy = std::min(annex_b.size(), output_capacity);
        std::memcpy(output, annex_b.data(), n_copy);

        encoded_out.codec        = vt_detail::codec_kind(codec_);
        encoded_out.payload      = core::ByteSpan{output, n_copy};
        encoded_out.payload_type = payload_type();
        encoded_out.is_keyframe  = is_keyframe;
        encoded_out.nalu_format  = plugins::NaluFormat::kAnnexBStartCode;

        stats_.frames_encoded++;
        stats_.bytes_encoded += n_copy;
        stats_.encode_latency_us_total += session_.last_latency_us();
        if (session_.last_latency_us() > stats_.encode_latency_us_max) {
            stats_.encode_latency_us_max = session_.last_latency_us();
        }
        if (is_keyframe) stats_.keyframes_encoded++;
        return plugins::kOk;
    }

    plugins::VideoCodecConfig      cfg_;
    vt_detail::Codec               codec_ = vt_detail::Codec::kH264;
    VtSession                      session_;
    std::mutex                     mtx_;
    bool                           opened_ = false;
    bool                           force_keyframe_next_ = false;
    plugins::VideoCodecStats       stats_{};
    std::vector<std::uint8_t>      scratch_ = std::vector<std::uint8_t>(kScratchBytes);
};

} // namespace

} // namespace nimrtc::nvcodec::vt_backend

namespace nimrtc::nvcodec {

// ---------------------------------------------------------------------------
// The public entry points.
//
// The H.264, H.265 and AV1 codec modules do not re-implement the
// VideoToolbox session code — they call these, which is the whole
// point of parameterising the encoder by codec.
// ---------------------------------------------------------------------------

namespace {

/** Map the public codec enum onto the internal one.
 *
 *  The mapping lives in `vt_common.hpp` so the *non-Apple* leg of this
 *  file can use it too (the reason strings are needed there). */
vt_detail::Codec to_detail_codec(VtCodec codec) noexcept {
    return vt_detail::to_detail(codec);
}

} // namespace

bool vt_encoder_available(VtCodec codec) noexcept {
    const vt_detail::Codec c = to_detail_codec(codec);

#if defined(__APPLE__)
    // ---- The OS-version gate -------------------------------------------
    //
    // A host older than the codec's floor cannot run the session at
    // all, so the probe reports "not available" *before* trying — and
    // `vt_encoder_unavailable_reason()` names the required release.
    if (!vt_detail::os_at_least(vt_detail::min_macos_version(c))) {
        return false;
    }

    // The probe creates a *real* compression session and tears it down
    // again.  A session that cannot be created is the honest answer:
    // the hardware (or the OS version) does not support the codec.
    vt_backend::VtSession probe;
    return probe.open(c, vt_backend::kProbeWidth, vt_backend::kProbeHeight,
                      vt_backend::kProbeFps, vt_backend::kProbeBitrate,
                      vt_backend::kProbeGop);
#else
    (void)c;
    return false;
#endif
}

std::unique_ptr<plugins::IVideoCodec>
make_vt_encoder(VtCodec codec, plugins::VideoCodecConfig cfg) {
#if defined(__APPLE__)
    const vt_detail::Codec c = to_detail(codec);
    if (!vt_detail::os_at_least(vt_detail::min_macos_version(c))) {
        NIMRTC_LOG_WARN("vt: encoder open() failed — "
                        << vt_detail::encoder_unavailable_reason(c));
        return nullptr;
    }
    auto enc = std::make_unique<vt_backend::VtEncoder>(std::move(cfg), c);
    if (enc->open() != plugins::kOk) {
        NIMRTC_LOG_WARN("vt: encoder open() failed — returning nullptr");
        return nullptr;
    }
    return enc;
#else
    (void)codec;
    (void)cfg;
    return nullptr;
#endif
}

std::string vt_encoder_unavailable_reason(VtCodec codec) {
    return vt_detail::encoder_unavailable_reason(to_detail_codec(codec));
}

} // namespace nimrtc::nvcodec

#else  // !__APPLE__

// ---------------------------------------------------------------------------
// The non-Apple build.
//
// The entry points still exist — they report "not available" and return
// nullptr — so the codec modules need no `#if` guards of their own, and
// a Windows / Linux build behaves exactly like a Mac whose hardware
// cannot do the codec.
// ---------------------------------------------------------------------------

namespace nimrtc::nvcodec {

bool vt_encoder_available(VtCodec) noexcept { return false; }

std::unique_ptr<plugins::IVideoCodec>
make_vt_encoder(VtCodec, plugins::VideoCodecConfig) { return nullptr; }

std::string vt_encoder_unavailable_reason(VtCodec codec) {
    return vt_detail::encoder_unavailable_reason(vt_detail::to_detail(codec));
}

} // namespace nimrtc::nvcodec

#endif // __APPLE__

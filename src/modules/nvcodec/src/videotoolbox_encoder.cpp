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
        //
        // The two keys are complementary, not contradictory: "Enable"
        // says "hardware acceleration is allowed", "Require" says "and
        // fail if it is not available".  Setting only "Require" would
        // be read by some OS versions as "hardware is disabled".
        CFMutableDictionaryRef encoder_spec =
            CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        if (!encoder_spec) return false;
        CFDictionarySetValue(
            encoder_spec,
            kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
            kCFBooleanTrue);
        CFDictionarySetValue(
            encoder_spec,
            kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
            kCFBooleanTrue);

        // ---- The session -----------------------------------------------
        //
        // The output callback is *mandatory* — VideoToolbox has no
        // "synchronous mode" without one.  The callback appends the
        // compressed sample to this session's own buffer, and `encode()`
        // hands that buffer out.
        //
        // The source's pixel format is left to the session: the session
        // picks the format its encoder actually accepts, and the CPU
        // staging path reads the buffer's *actual* format before it
        // fills it.  Forcing a format here would make the session
        // creation fail outright on an encoder that does not accept it,
        // and "the encoder refused" is a worse answer than "the staging
        // path adapts to the format the encoder wants".
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
        // The parameter sets belong to the session that produced them; a
        // fresh session may carry different ones (a new geometry), so
        // they must not outlive it.
        parameter_sets_.clear();
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
        //
        // The dictionary has to be *mutable*: `CFDictionarySetValue`
        // takes a `CFMutableDictionaryRef`, and `CFDictionaryCreate`
        // (the immutable factory) takes six arguments and hands back
        // an immutable dictionary, so it cannot be used here at all.
        CFMutableDictionaryRef frame_props = nullptr;
        if (force_keyframe) {
            frame_props = CFDictionaryCreateMutable(
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

    /** The parameter sets the session produced, as Annex-B NAL units.
     *
     *  Empty until the first compressed sample has arrived (the format
     *  description only exists then), and empty for AV1 (whose sequence
     *  header travels inside the OBU stream). */
    [[nodiscard]] const std::vector<std::uint8_t>& parameter_sets() const noexcept {
        return parameter_sets_;
    }

    /** The session's reusable buffer for the merged access unit.
     *
     *  The parameter sets are prepended to a key frame's slices, and the
     *  merge needs a buffer of its own.  This one belongs to the
     *  session, so the merge does not allocate on the encode path — the
     *  buffer grows to its steady-state size on the first key frame and
     *  is reused from then on. */
    [[nodiscard]] std::vector<std::uint8_t>& merged_bitstream() noexcept {
        return merged_bitstream_;
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

    /** The output callback VideoToolbox delivers the compressed sample to.
     *
     *  The signature is fixed by VideoToolbox:
     *  `VTCompressionOutputCallback` returns **void**, not `OSStatus`.
     *  The encoder's own status arrives as the `status` *argument*, so
     *  there is nothing to return — returning an `OSStatus` here does
     *  not compile ("no known conversion from 'OSStatus (*)(...)' to
     *  'VTCompressionOutputCallback'"). */
    static void output_callback(void* refcon,
                                void* /*sourceFrameRefCon*/,
                                OSStatus status,
                                VTEncodeInfoFlags flags,
                                CMSampleBufferRef sample_buffer) {
        auto* self = static_cast<VtSession*>(refcon);
        if (!self) return;
        if (status != noErr) return;

        if (flags & kVTEncodeInfo_FrameDropped) {
            self->dropped_ = true;
            return;
        }
        if (!sample_buffer) return;

        // The key-frame flag: `kCMSampleBufferFlag_...` does not carry
        // it, but the sample's attachments do — VideoToolbox marks an
        // IDR with the `kVTCompressionOutput_KeyFrame` flag, which
        // reaches us through the sample buffer's format-description
        // attachments.  The reliable, public way to ask is the sample
        // buffer's *sync* sample flag, which is set for an IDR.
        //
        // The flag lives in the sample's *attachments* array, under
        // `kCMSampleAttachmentKey_NotSync`: the entry is absent (or
        // `false`) for a sync sample, and `true` for a non-sync one.
        // There is no `CMSampleBufferIsNotSync()` function — the
        // attachment is the only public spelling.
        //
        // A sample with no attachments array at all is treated as a
        // sync sample: the absence of the "not sync" marker means
        // "not marked as non-sync", and a *key frame* is the safe
        // answer (a spurious key frame costs one extra IDR; a missed
        // one costs a broken decode).
        bool not_sync = false;
        CFArrayRef attachments =
            CMSampleBufferGetSampleAttachmentsArray(sample_buffer, true);
        if (attachments) {
            CFIndex count = CFArrayGetCount(attachments);
            for (CFIndex i = 0; i < count; ++i) {
                // `CFArrayGetValueAtIndex` hands back a `const void *`,
                // so the element is read through the *immutable*
                // `CFDictionaryRef` view.  Casting it to
                // `CFMutableDictionaryRef` would cast the const away,
                // which Clang rejects -- and nothing here needs to
                // mutate the attachment anyway.
                CFDictionaryRef att = static_cast<CFDictionaryRef>(
                    CFArrayGetValueAtIndex(attachments, i));
                if (!att) continue;
                CFTypeRef value =
                    CFDictionaryGetValue(att, kCMSampleAttachmentKey_NotSync);
                // The value is a CFBoolean by contract, but
                // `CFBooleanGetValue` on anything else is undefined --
                // so the type is checked before it is read.
                if (value && CFGetTypeID(value) == CFBooleanGetTypeID() &&
                    CFBooleanGetValue(static_cast<CFBooleanRef>(value))) {
                    not_sync = true;
                    break;
                }
            }
        }
        self->keyframe_seen_ = !not_sync;

        // ---- Extract the compressed bytes ------------------------------
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample_buffer);
        if (!block) return;
        const std::size_t total = CMBlockBufferGetDataLength(block);
        if (total == 0) return;

        self->output_.resize(total);
        if (CMBlockBufferCopyDataBytes(block, 0, total, self->output_.data())
            != kCMBlockBufferNoErr) {
            self->output_.clear();
            return;
        }

        // ---- Remember the parameter sets ------------------------------
        //
        // VideoToolbox keeps the SPS / PPS (and the VPS for HEVC) *out
        // of band*, in the sample buffer's format description.  The
        // slice data alone is therefore not decodable by a decoder that
        // expects in-band parameter sets — a software decoder answers
        // `kErrCorrupt` (4100).
        //
        // The sets are collected here (this is the only place the format
        // description is available) and are prepended to the key frame
        // later, once the bitstream has been converted to Annex-B —
        // prepending them to the *length-prefixed* bytes would produce
        // a mixed-framing stream that no parser can read.
        //
        // The sets belong to the *session*, not to the frame: they only
        // change when the session is re-created (a new geometry).  So
        // they are collected once — when the session has none yet — and
        // the steady state (every inter frame) touches no allocator at
        // all.  Collecting them per frame would put a heap allocation
        // on the encode hot path, which the zero-allocation benchmark
        // measures.
        if (self->parameter_sets_.empty()) {
            CMFormatDescriptionRef fd =
                CMSampleBufferGetFormatDescription(sample_buffer);
            vt_detail::collect_parameter_sets(fd, self->codec_,
                                              self->parameter_sets_);
        }
    }

    VTCompressionSessionRef session_ = nullptr;
    CVPixelBufferPoolRef      pool_    = nullptr;
    vt_detail::Codec          codec_   = vt_detail::Codec::kH264;
    std::uint32_t             width_   = 0;
    std::uint32_t             height_  = 0;
    std::uint32_t             fps_     = kDefaultFps;
    std::uint32_t             frame_index_ = 0;
    std::vector<std::uint8_t> output_;
    // The parameter sets the session produced, concatenated in the order
    // they belong in the stream (SPS, PPS for H.264; VPS, SPS, PPS for
    // HEVC).  They are prepended to a key frame's Annex-B bitstream.
    std::vector<std::uint8_t> parameter_sets_;
    // The session's reusable buffer for the merged (parameter sets +
    // slices) access unit — see `merged_bitstream()`.
    std::vector<std::uint8_t> merged_bitstream_;
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

    // ---- The decoder half (not supported by this encoder) ----------------
    //
    // `IVideoCodec` is the *combined* seam: one instance may serve
    // both directions, so the interface carries the decoder's pure
    // virtuals as well.  This encoder never decodes, so the decoder
    // half answers "unsupported" rather than being left abstract --
    // an abstract class cannot be marked `final`.  (The `decode()`
    // override lives further down, next to the zero-copy path.)
    // ---- The parameter set (remote-desktop plan B12 / B13) --------------
    //
    // A VideoToolbox session is bound to a fixed geometry, so a
    // geometry change means a *fresh session* -- which is exactly what
    // `update_config()` already does.  This override therefore
    // translates the parameter set into a configuration and hands it
    // over, instead of answering "unsupported".
    //
    // Answering "unsupported" was a real bug: the engine pushes the
    // parameter set before the first frame, so the encoder kept the
    // geometry it was opened with (640x480) and then refused every
    // 320x240 frame ("the encoder keeps its previous parameters"),
    // which made the whole send path fail on macOS.
    plugins::Status set_params(const plugins::VideoEncoderParams& params) noexcept override {
        // A parameter set may leave the geometry out entirely (the
        // caller only wants to change, say, the bitrate).  But it may
        // not state *half* a geometry: "480 rows, no width" is a
        // contradiction, and quietly ignoring it would leave the
        // encoder coding at a size the caller did not ask for.
        if ((params.width == 0) != (params.height == 0)) {
            return plugins::kErrInvalidParam;
        }

        plugins::VideoCodecConfig cfg = cfg_;
        if (params.width != 0 && params.height != 0) {
            // 4:2:0 chroma needs even dimensions.
            cfg.width  = params.width  & ~1u;
            cfg.height = params.height & ~1u;
            if (cfg.width == 0 || cfg.height == 0) {
                return plugins::kErrInvalidParam;
            }
        }
        if (params.fps != 0)            cfg.fps = params.fps;
        if (params.bitrate_bps != 0)    cfg.bitrate_bps = params.bitrate_bps;
        if (params.keyframe_interval != 0) {
            cfg.keyframe_interval = params.keyframe_interval;
        }
        cfg.pixel_format = params.pixel_format;
        return update_config(cfg);
    }

    plugins::VideoEncoderParams params() const noexcept override {
        plugins::VideoEncoderParams p;
        p.width             = cfg_.width;
        p.height            = cfg_.height;
        p.fps               = cfg_.fps;
        p.bitrate_bps       = cfg_.bitrate_bps;
        p.keyframe_interval = cfg_.keyframe_interval;
        p.pixel_format      = cfg_.pixel_format;
        return p;
    }

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

        // Lock the pixel buffer and fill it.  The lock has to happen
        // *after* the acquisition, and it reports the real row pitch —
        // which is not the frame width in general.
        //
        // The buffer's *pixel format* is read first, because the
        // staging code has to match it.  The pool is taken from the
        // encoder's own session, so the format is the one the encoder
        // negotiated — which is NOT necessarily bi-planar NV12.
        //
        // Writing NV12 into a buffer that is, say, 32BGRA does not
        // merely produce a wrong picture: the chroma plane lands
        // outside the luma plane's allocation and the encoder then
        // reads past the end of the surface.  That is a heap overrun,
        // and it is what used to crash the macOS CI leg.
        const CVReturn lock_st = CVPixelBufferLockBaseAddress(pixel_buffer, 0);
        if (lock_st != kCVReturnSuccess) {
            CVPixelBufferRelease(pixel_buffer);
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }

        {
            const auto& layout = cpu->layout();
            const std::uint32_t stride_y =
                static_cast<std::uint32_t>(layout.strides[0]);
            const std::uint32_t stride_u =
                static_cast<std::uint32_t>(layout.strides[1]);
            const std::uint32_t stride_v =
                static_cast<std::uint32_t>(layout.strides[2]);

            const OSType format = CVPixelBufferGetPixelFormatType(pixel_buffer);

            if (format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
                format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
                // ---- The bi-planar (NV12) layout -------------------------
                //
                // The luma plane: one row at a time, at the *plane's own*
                // stride.
                //
                // The stride is `CVPixelBufferGetBytesPerRowOfPlane(pb, 0)`,
                // NOT `CVPixelBufferGetBytesPerRow(pb)`: for a buffer with
                // more than one plane the latter returns a value whose
                // product with the height covers *the whole buffer, all
                // planes together* (for 1920x1080 NV12: 2904, which is
                // (1920*1080 + 1920*540) / 1080 rounded up).  Using it
                // as the luma row stride walks the luma plane diagonally:
                // row N is read from an offset of N*2904 instead of
                // N*luma_stride, so the encoder codes a picture whose
                // rows are shifted sideways — and whose bottom rows come
                // from the chroma plane or past the end of the surface.
                //
                // The same applies to the plane's *address*: the luma
                // plane's base address comes from the buffer, not from
                // `base + something`.
                const std::size_t y_pitch =
                    CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
                void* y_plane = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0);
                if (y_pitch == 0 || !y_plane) {
                    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
                    CVPixelBufferRelease(pixel_buffer);
                    stats_.encode_errors++;
                    return plugins::kErrInternal;
                }
                auto* y_dst = static_cast<std::uint8_t*>(y_plane);
                for (std::uint32_t row = 0; row < h; ++row) {
                    std::memcpy(y_dst + static_cast<std::size_t>(row) * y_pitch,
                                y_in + static_cast<std::size_t>(row) * stride_y, w);
                }
                // The chroma plane: U and V interleaved, at the buffer's
                // own chroma pitch.
                //
                // The chroma plane's *address* comes from the buffer, not
                // from `luma_base + pitch * height`.  On an
                // IOSurface-backed buffer the planes are separate
                // allocations, so the luma pointer plus a height-sized
                // offset lands in the *middle of the luma allocation* —
                // and writing the chroma there overruns it.  That is
                // what used to crash the macOS CI leg.
                const std::size_t uv_pitch =
                    CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
                void* uv_plane = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1);
                const std::uint32_t cw = (w + 1u) / 2u;
                const std::uint32_t ch = (h + 1u) / 2u;
                if (uv_pitch == 0 || !uv_plane) {
                    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
                    CVPixelBufferRelease(pixel_buffer);
                    stats_.encode_errors++;
                    return plugins::kErrInternal;
                }
                auto* uv_dst = static_cast<std::uint8_t*>(uv_plane);
                for (std::uint32_t row = 0; row < ch; ++row) {
                    const std::uint8_t* u_row =
                        u_in + static_cast<std::size_t>(row) * stride_u;
                    const std::uint8_t* v_row =
                        v_in + static_cast<std::size_t>(row) * stride_v;
                    std::uint8_t* out =
                        uv_dst + static_cast<std::size_t>(row) * uv_pitch;
                    for (std::uint32_t col = 0; col < cw; ++col) {
                        out[col * 2 + 0] = u_row[col];
                        out[col * 2 + 1] = v_row[col];
                    }
                }
            } else if (format == kCVPixelFormatType_420YpCbCr8PlanarFullRange ||
                       format == kCVPixelFormatType_420YpCbCr8Planar) {
                // ---- The planar (I420) layout ------------------------------
                //
                // Three separate planes, each with its own pitch.  The
                // plane pointers come from `CVPixelBufferGetBaseAddressOfPlane`
                // — computing them by hand from the luma pointer would
                // be wrong as soon as the buffer's rows are padded.
                const std::size_t y_pitch =
                    CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
                const std::size_t u_pitch =
                    CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
                const std::size_t v_pitch =
                    CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 2);
                void* y_plane = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0);
                void* u_plane = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1);
                void* v_plane = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 2);
                if (!y_plane || !u_plane || !v_plane ||
                    y_pitch == 0 || u_pitch == 0 || v_pitch == 0) {
                    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
                    CVPixelBufferRelease(pixel_buffer);
                    stats_.encode_errors++;
                    return plugins::kErrInternal;
                }
                auto* y_dst = static_cast<std::uint8_t*>(y_plane);
                auto* u_dst = static_cast<std::uint8_t*>(u_plane);
                auto* v_dst = static_cast<std::uint8_t*>(v_plane);
                const std::uint32_t cw = (w + 1u) / 2u;
                const std::uint32_t ch = (h + 1u) / 2u;
                for (std::uint32_t row = 0; row < h; ++row) {
                    std::memcpy(y_dst + static_cast<std::size_t>(row) * y_pitch,
                                y_in + static_cast<std::size_t>(row) * stride_y, w);
                }
                for (std::uint32_t row = 0; row < ch; ++row) {
                    std::memcpy(u_dst + static_cast<std::size_t>(row) * u_pitch,
                                u_in + static_cast<std::size_t>(row) * stride_u, cw);
                    std::memcpy(v_dst + static_cast<std::size_t>(row) * v_pitch,
                                v_in + static_cast<std::size_t>(row) * stride_v, cw);
                }
            } else {
                // ---- An unexpected format ---------------------------------
                //
                // The pool's format is whatever the encoder negotiated,
                // and this backend does not know every format
                // VideoToolbox can pick.  Writing NV12 into it would
                // overrun it, so the frame is refused instead — the
                // caller falls back to the software encoder.
                NIMRTC_LOG_WARN("vt: the encoder's pixel buffer has an "
                                "unsupported format (" << format
                                << ") — the frame is refused");
                CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
                CVPixelBufferRelease(pixel_buffer);
                stats_.encode_errors++;
                return plugins::kErrUnsupported;
            }
        }
        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

        const plugins::Status st = encode_pixel_buffer(pixel_buffer, output,
                                                       output_capacity,
                                                       encoded_out,
                                                       /*zero_copy=*/false);
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
                                    scratch_.size(), encoded_out,
                                    /*zero_copy=*/true);
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
    /** Encode one `CVPixelBuffer` and publish the bitstream.
     *
     *  `zero_copy` says how the picture reached the session: `true` when
     *  the caller handed over a `CVPixelBuffer` it already owned (the
     *  surface never crossed the CPU), `false` when this backend staged
     *  the planes into a fresh surface itself.  The distinction is what
     *  the `frames_encoded_staged` / `frames_encoded_zero_copy` counters
     *  report — a session that reports neither reads as "the encoder
     *  never ran". */
    plugins::Status encode_pixel_buffer(CVPixelBufferRef pixel_buffer,
                                        std::uint8_t* output,
                                        std::size_t output_capacity,
                                        plugins::EncodedVideoFrame& encoded_out,
                                        bool zero_copy) noexcept {
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

        // ---- The parameter sets ----------------------------------------
        //
        // VideoToolbox keeps the SPS / PPS (and the VPS for HEVC) out of
        // band, in the sample buffer's format description.  A decoder
        // that expects in-band parameter sets — which is what the rest
        // of this tree ships — has nothing to decode against without
        // them, and answers `kErrCorrupt` (4100).
        //
        // The parameter sets therefore go back in front of the key
        // frame's slices as Annex-B NAL units, which is where the
        // specification puts them.  Only a key frame carries them:
        // repeating them on every inter frame would grow the stream for
        // nothing.
        //
        // The merge goes into the session's *reusable* buffer, not into
        // a fresh one: the merge helper is the portable one the unit
        // tests cover, and a per-key-frame allocation on the encode
        // path is exactly what the zero-allocation benchmark measures.
        if (is_keyframe) {
            const std::vector<std::uint8_t>& ps = session_.parameter_sets();
            if (!ps.empty() &&
                vt_detail::prepend_parameter_sets(ps, annex_b,
                                                  session_.merged_bitstream())) {
                annex_b.swap(session_.merged_bitstream());
            }
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
        // The staged / zero-copy split is what tells a host *how* the
        // picture reached the encoder.  A session that reports neither
        // reads as "the encoder never ran" — which is exactly what the
        // engine's telemetry and the hardware-loopback test read.
        if (zero_copy) {
            // The surface was handed over as it was: the picture never
            // crossed the CPU, so `bytes_copied` stays at zero.
            stats_.frames_encoded_zero_copy++;
            stats_.zero_copy = true;
            stats_.zero_copy_frames++;
        } else {
            // The planes were copied into a fresh surface: that is a
            // CPU staging copy.
            stats_.frames_encoded_staged++;
            stats_.zero_copy = false;
            stats_.staged_frames++;
        }
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
    // `mutable`: the two `supports_zero_copy_*` accessors are `const`
    // members, but they still have to take the lock to read the
    // session's state.  A plain `std::mutex` member would make the
    // lock impossible to take from a const member function, which
    // Clang rejects outright ("1st argument ('const std::mutex')
    // would lose const qualifier").
    mutable std::mutex              mtx_;
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
    const vt_detail::Codec c = vt_detail::to_detail(codec);
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

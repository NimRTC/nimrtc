/**
 * @file src/modules/nvcodec/src/videotoolbox_decoder.cpp
 * @brief Apple VideoToolbox decoder — real adapter (H.264 / HEVC / AV1).
 *
 * The decode counterpart of `videotoolbox_encoder.cpp`, and modelled on
 * the same shape:
 *
 *   1. `vt_decoder_available()` asks the host whether it has a hardware
 *      decoder for the codec (`VTIsHardwareDecodeSupported`) — it reports
 *      the truth about the hardware, not about the OS version.
 *   2. `make_vt_decoder()` creates the session for real and returns an
 *      opened decoder; a failure returns nullptr so the registry falls
 *      through to the next backend.
 *
 * ## Why the session is *deferred*
 *
 * A hardware VideoToolbox decoder is created from a format description
 * that must carry the stream's **own** parameter sets — the codec
 * extension atom (`avcC` for H.264, `hvcC` for HEVC) is what tells the
 * hardware which stream it is about to decode.  A description built from
 * a bare codec type carries none, and the session creation then fails
 * with `-8971` (`codecExtensionNotFoundErr`).
 *
 * The parameter sets are not known before the first access unit arrives,
 * so the session is opened in one of two ways:
 *
 *   - the caller supplies the parameter sets → the session is created
 *     immediately, from them;
 *   - the caller has none yet → the open is **deferred**: it succeeds
 *     (the host *does* have a hardware decoder for the codec — that is
 *     what `VTIsHardwareDecodeSupported()` says), and `decode()` creates
 *     the session from the first access unit that carries the parameter
 *     sets.
 *
 * AV1 is the exception: it carries its sequence header inside the OBU
 * stream, has no codec extension atom, and is therefore created eagerly
 * from the codec-type description.
 *
 * ## The zero-copy contract
 *
 * VideoToolbox's decompression session produces `CVPixelBufferRef`
 * surfaces.  The tree carries those as
 * `plugins::GpuBackend::kAppleCFPixelBuffer`, so the decoder supports
 * the zero-copy decode path: the picture the session produces is handed
 * to the caller as a `GpuBuffer` without a CPU round-trip.
 *
 * The CPU path (the engine's universal fallback) copies the decoded
 * surface into the caller's I420 planes — which is one copy, and is
 * counted honestly as a *staged* frame in the codec statistics.
 *
 * ## Bitstream framing
 *
 * VideoToolbox's decompression session takes a `CMSampleBufferRef`, not
 * a raw byte buffer.  This tree's RTP depacketizer produces Annex-B
 * start codes (for H.264 / HEVC) or Annex-B OBUs (for AV1), so the
 * bytes are converted on the way in (see `vt_detail::ensure_annex_b` /
 * `vt_detail::av1_annex_b_to_low_overhead`) and wrapped in a
 * `CMSampleBufferRef` before the decode call.
 *
 * ## Platform reality
 *
 * The whole file is guarded by `#if defined(__APPLE__)`.  On every
 * other platform the module still exports `vt_decoder_available()`
 * and `make_vt_decoder()` — they report "not available" and return
 * nullptr.
 */

#include <nimrtc/nvcodec/vt_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>

// The shared helpers (`vt_detail`) are needed on *every* platform: the
// non-Apple leg of this file uses them for the unavailable-reason
// strings.
#include "vt_common.hpp"

#if defined(__APPLE__)

#include <algorithm>
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

/** The probe's geometry.
 *
 *  A decompression session is created from a *format description*, not
 *  from a geometry — but the session still needs a picture size, so the
 *  probe uses a small one. */
constexpr std::uint32_t kProbeWidth  = 320;
constexpr std::uint32_t kProbeHeight = 240;

/** The `NALUnitHeaderLength` the format descriptions are built with.
 *
 *  The tree's own RTP packetizer carries AVCC (4-byte length-prefixed)
 *  NAL units, so the parameter sets are described with a 4-byte length
 *  field — which is what `CMVideoFormatDescriptionCreateFrom*ParameterSets`
 *  expects.
 *
 *  The type is `std::uint8_t` because that is what the CoreMedia
 *  functions declare the parameter as; a wider type would be an
 *  implicit narrowing conversion at the call site, and this tree builds
 *  with warnings-as-errors. */
constexpr std::uint8_t kAvccNalUnitHeaderLength = 4;

// ---------------------------------------------------------------------------
// VtDecoderSession — the wrapper around VTDecompressionSession.
// ---------------------------------------------------------------------------

class VtDecoderSession {
public:
    VtDecoderSession() = default;
    ~VtDecoderSession() { close(); }

    VtDecoderSession(const VtDecoderSession&)            = delete;
    VtDecoderSession& operator=(const VtDecoderSession&) = delete;

    /** Create the decompression session for @p codec.
     *
     *  The session is created from a *format description*.  For H.264 /
     *  HEVC the description must carry the stream's own parameter sets
     *  — a description built from a bare codec type carries none, and a
     *  **hardware** decoder rejects it with `-8971`
     *  (`codecExtensionNotFoundErr`), because the codec extension atom
     *  (`avcC` / `hvcC`) is what tells the hardware which stream it is
     *  about to decode.
     *
     *  So there are two outcomes when the caller has no parameter sets
     *  yet:
     *
     *  - the host has **no** hardware decoder for the codec → the open
     *    fails, and the caller must not fall back to software (the
     *    "hardware" claim would be false);
     *  - the host **does** have one → the open *defers*: the session is
     *    created later, from the parameter sets the first access unit
     *    carries.  `decode()` performs that step.
     *
     *  @param sets  The parameter sets, or nullptr when the caller has
     *               none yet.
     *  @return true on success (including the deferred case). */
    bool open(vt_detail::Codec codec,
              const vt_detail::ParameterSets* sets = nullptr) noexcept {
        codec_ = codec;

        // The session itself is **not** torn down here.  The new one is
        // built first and only replaces the old one once it is known
        // good; otherwise a failed mid-stream re-creation would destroy
        // a session that was still working.
        //
        // Whether the stream's *sequence* changed is not this function's
        // business — the caller owns that, because the caller is the only
        // party that knows whether the sets it passed in are the ones the
        // stream is actually sending.
        deferred_ = false;

        // ---- The format description ------------------------------------
        //
        // `CMVideoFormatDescriptionCreateFromH264ParameterSets` takes a
        // *flat* array of parameter-set pointers (SPS first, then PPS);
        // `...FromHEVCParameterSets` takes the same flat array (VPS,
        // SPS, PPS).  The payloads are the raw NAL units — neither the
        // start code nor the length prefix.
        CMFormatDescriptionRef desc = nullptr;
        OSStatus st = noErr;

        const bool have_sets = sets && vt_detail::parameter_sets_complete(codec, *sets);
        if (have_sets) {
            if (codec == vt_detail::Codec::kH265) {
                // HEVC wants the VPS as well; without it the format
                // description is incomplete.
                const std::uint8_t* ptrs[3] = {
                    sets->vps.data(), sets->sps.data(), sets->pps.data()};
                const std::size_t sizes[3] = {
                    sets->vps.size(), sets->sps.size(), sets->pps.size()};
                st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                    kCFAllocatorDefault, 3, ptrs, sizes,
                    kAvccNalUnitHeaderLength, nullptr, &desc);
            } else {
                const std::uint8_t* ptrs[2] = {sets->sps.data(),
                                               sets->pps.data()};
                const std::size_t sizes[2] = {sets->sps.size(),
                                              sets->pps.size()};
                st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    kCFAllocatorDefault, 2, ptrs, sizes,
                    kAvccNalUnitHeaderLength, &desc);
            }
        }
        if (!desc && codec == vt_detail::Codec::kAv1) {
            // AV1 has no codec extension atom to carry, so the
            // codec-type form is the whole description.
            st = CMVideoFormatDescriptionCreate(
                kCFAllocatorDefault,
                vt_detail::cm_codec_type(codec),
                static_cast<std::int32_t>(kProbeWidth),
                static_cast<std::int32_t>(kProbeHeight),
                nullptr,
                &desc);
        }
        if (!desc && !have_sets && codec != vt_detail::Codec::kAv1) {
            // ---- The deferred case ---------------------------------------
            //
            // No parameter sets, so no codec extension atom, so a
            // hardware decoder would refuse the session.  The host's
            // capability is asked instead: if it has a hardware decoder
            // for the codec, the session is created from the stream's
            // own parameter sets when the first access unit arrives.
            if (!vt_detail::hardware_decode_supported(codec)) {
                NIMRTC_LOG_WARN("vt: no hardware decoder for "
                                << vt_detail::codec_short_name(codec)
                                << " on this host, and the stream carried no "
                                   "parameter sets to build one from");
                return false;
            }
            // There is no session to speak the stream yet, so any session
            // left over from a previous sequence is released here.
            release_session();
            deferred_ = true;
            NIMRTC_LOG_INFO("vt: the decompression session is deferred until "
                            "the stream's parameter sets arrive");
            return true;
        }
        if (st != noErr || !desc) {
            NIMRTC_LOG_WARN("vt: the format description could not be built "
                            "(os=" << static_cast<int>(st) << ")");
            return false;
        }

        // ---- The decoder specification ---------------------------------
        //
        // `RequireHardwareAcceleratedVideoDecoder = true` makes the
        // session creation *fail* when the machine has no hardware
        // decoder for the codec, instead of silently falling back to the
        // software decoder — which would make the "hardware backend"
        // claim false.
        //
        // The two keys are complementary, not contradictory: "Enable"
        // says "hardware acceleration is allowed", "Require" says "and
        // fail if it is not available".  Setting only "Require" would
        // be read by some OS versions as "hardware is disabled".
        CFMutableDictionaryRef decoder_spec =
            CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        if (!decoder_spec) {
            CFRelease(desc);
            return false;
        }
        CFDictionarySetValue(
            decoder_spec,
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
            kCFBooleanTrue);
        CFDictionarySetValue(
            decoder_spec,
            kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
            kCFBooleanTrue);

        // ---- The output callback record -------------------------------
        //
        // `VTDecompressionSessionCreate` takes the callback as a
        // *record* (callback + refcon), not as two arguments.
        VTDecompressionOutputCallbackRecord record = {};
        record.decompressionOutputCallback = &VtDecoderSession::output_callback;
        record.decompressionOutputRefCon  = this;

        // The new session is built into a *local* first: when the
        // parameter sets changed mid-stream, the session that is already
        // working must survive a failed re-creation.  Destroying it
        // first would turn a recoverable hiccup into a dead decoder.
        VTDecompressionSessionRef new_session = nullptr;
        st = VTDecompressionSessionCreate(kCFAllocatorDefault, desc,
                                          decoder_spec, nullptr, &record,
                                          &new_session);
        CFRelease(decoder_spec);

        if (st != noErr || !new_session) {
            NIMRTC_LOG_WARN("vt: VTDecompressionSessionCreate failed (os="
                            << static_cast<int>(st) << ")");
            CFRelease(desc);
            return false;
        }

        // The new session is good — the old one (if any) can go.
        release_session();
        session_     = new_session;
        format_desc_ = desc;
        session_failed_ = false;

        NIMRTC_LOG_INFO("vt: decompression session created");
        return true;
    }

    void close() noexcept {
        release_session();
        if (decoded_) {
            CVPixelBufferRelease(decoded_);
            decoded_ = nullptr;
        }
        desc_from_parameter_sets_ = false;
        session_sets_              = vt_detail::ParameterSets{};
        deferred_                  = false;
        session_failed_            = false;
    }

    [[nodiscard]] bool is_open() const noexcept { return session_ != nullptr; }

    /** True iff the host has a hardware decoder for this codec.
     *
     *  Before the session exists (the deferred case) this is the
     *  capability probe's answer; afterwards it is "a hardware session
     *  exists", because the session was created with
     *  `RequireHardwareAcceleratedVideoDecoder = true` — the software
     *  decoder would have been refused. */
    [[nodiscard]] bool hardware_capable() const noexcept {
        return session_ != nullptr || deferred_;
    }

    /** True iff the session is still waiting for the stream's parameter
     *  sets. */
    [[nodiscard]] bool is_deferred() const noexcept { return deferred_; }

    /** True iff the last attempt to create the session from the
     *  stream's own parameter sets failed.
     *
     *  A frame that arrived *before* the parameter sets is "not ready"
     *  — nothing was rejected, the engine just has to wait for a key
     *  frame.  A frame that arrived *with* them and still could not be
     *  decoded is a real decode error, and is counted as one. */
    [[nodiscard]] bool session_creation_failed() const noexcept {
        return session_failed_;
    }

    /** Decode one compressed frame.
     *
     *  @param data  The Annex-B (or length-prefixed) bitstream.
     *  @param size  The bitstream's size in bytes.
     *  @param out   Receives the decoded `CVPixelBufferRef` (retained).
     *  @return true on success. */
    bool decode(const std::uint8_t* data, std::size_t size,
                CVPixelBufferRef& out) noexcept {
        if (!data || size == 0) return false;

        // ---- The parameter sets ----------------------------------------
        //
        // The format description a VideoToolbox decoder needs carries
        // the stream's own parameter sets.  The session is opened
        // *before* the first frame arrives (the factory contract is
        // "created, then opened"), so the first frame may carry the
        // parameter sets that the session does not have yet.
        //
        // Two cases:
        //
        //  - the session does not exist yet because the open was
        //    **deferred** (the host has a hardware decoder, but there
        //    were no parameter sets to build the codec extension atom
        //    from) — the session is created now, from this frame;
        //  - the session exists but was built without the stream's
        //    parameter sets — it is re-created with them.
        vt_detail::ParameterSets sets;
        if (codec_ != vt_detail::Codec::kAv1) {
            vt_detail::extract_parameter_sets(codec_, data, size, sets);
            const bool have_sets = vt_detail::parameter_sets_complete(codec_, sets);

            if (!session_) {
                // The session does not exist yet: the open was
                // **deferred** (the host has a hardware decoder, but there
                // were no parameter sets to build the codec extension
                // atom from).  It is created now, from this access unit.
                if (!deferred_) return false;
                if (!have_sets) {
                    // The session is waiting for the stream's parameter
                    // sets; this access unit does not carry them.  The
                    // frame is dropped — the engine will ask for a key
                    // frame, and the next key frame carries the sets.
                    NIMRTC_LOG_WARN("vt: the access unit carries no "
                                    "parameter sets yet; waiting for a key "
                                    "frame");
                    return false;
                }
                // `this->open(...)`, not `session_.open(...)`: the class
                // has a *data* member called `session_` (the
                // `VTDecompressionSessionRef`), so `session_.open` would
                // resolve to the pointer member and Clang rejects it
                // ("member reference type 'VTDecompressionSessionRef'
                // is a pointer; did you mean to use '->'?").
                if (!this->open(codec_, &sets)) {
                    // The session could not be created from the stream's
                    // *own* parameter sets.  The failure is remembered so
                    // the decoder reports it as a decode error (rather
                    // than as "not ready") — but the deferred flag is
                    // kept, so the next access unit that carries the
                    // sets gets another chance: the failure may have
                    // been transient, and a permanently broken stream
                    // fails the same way on every access unit anyway.
                    deferred_      = true;
                    session_failed_ = true;
                    NIMRTC_LOG_ERROR("vt: the decoder session could not be "
                                     "created from the stream's parameter sets");
                    return false;
                }
                desc_from_parameter_sets_ = true;
                session_sets_              = sets;
            } else if (have_sets) {
                // The stream may switch sequence mid-flight (a new
                // geometry, a new profile).  The session's format
                // description describes the *old* sequence, so the
                // session has to be re-created when the sets change.
                const bool changed = !desc_from_parameter_sets_ ||
                                     !vt_detail::parameter_sets_equal(sets, session_sets_);
                if (changed) {
                    if (!this->open(codec_, &sets)) {
                        // The old session survives — `open()` only
                        // replaces it once the new one is known good —
                        // but it speaks the *old* sequence, so it cannot
                        // decode this access unit.
                        NIMRTC_LOG_ERROR("vt: the decoder session could not be "
                                         "re-created with the updated parameter "
                                         "sets");
                        return false;
                    }
                    desc_from_parameter_sets_ = true;
                    session_sets_              = sets;
                }
            }
        }
        if (!session_) return false;

        // ---- The framing conversion -----------------------------------
        //
        // VideoToolbox wants a `CMSampleBufferRef`.  For H.264 / HEVC
        // the sample buffer's block buffer carries **length-prefixed**
        // (AVCC) NAL units; for AV1 it carries the AV1 low-overhead OBU
        // form.  This tree carries both in Annex-B, so the conversion
        // happens here.
        //
        // (The *encoder* direction is the mirror image: VideoToolbox's
        // compression session emits length-prefixed NAL units for
        // H.264 / HEVC, which `ensure_annex_b()` converts back.)
        std::vector<std::uint8_t> converted;
        bool ok = false;
        if (codec_ == vt_detail::Codec::kAv1) {
            ok = vt_detail::av1_annex_b_to_low_overhead(data, size, converted);
        } else {
            ok = vt_detail::annex_b_to_avcc(data, size, converted);
            if (!ok) {
                // The bitstream may already be length-prefixed; in that
                // case it is handed to the session as-is.
                converted.assign(data, data + size);
                ok = !converted.empty();
            }
        }
        if (!ok) {
            NIMRTC_LOG_ERROR("vt: the bitstream is neither Annex-B nor a "
                             "well-formed length-prefixed stream");
            return false;
        }

        // ---- The sample buffer -----------------------------------------
        CMBlockBufferRef block = nullptr;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, nullptr, converted.size(), kCFAllocatorDefault,
            nullptr, 0, converted.size(), 0, &block);
        if (st != noErr || !block) {
            NIMRTC_LOG_ERROR("vt: CMBlockBufferCreateWithMemoryBlock failed "
                             "(os=" << static_cast<int>(st) << ")");
            return false;
        }
        st = CMBlockBufferAssureBlockMemory(block);
        if (st != noErr) {
            CFRelease(block);
            return false;
        }
        st = CMBlockBufferReplaceDataBytes(converted.data(), block, 0,
                                            converted.size());
        if (st != noErr) {
            CFRelease(block);
            return false;
        }

        // The description: the session's own one when it has parameter
        // sets, otherwise a codec-type description built for the sample
        // buffer.
        CMFormatDescriptionRef desc = format_desc_;
        bool owns_desc = false;
        if (!desc) {
            st = CMVideoFormatDescriptionCreate(
                kCFAllocatorDefault, vt_detail::cm_codec_type(codec_),
                static_cast<std::int32_t>(kProbeWidth),
                static_cast<std::int32_t>(kProbeHeight), nullptr, &desc);
            if (st != noErr || !desc) {
                CFRelease(block);
                return false;
            }
            owns_desc = true;
        }

        CMSampleBufferRef sample = nullptr;
        // The sample buffer's timing is described by an *array* of
        // `CMSampleTimingInfo`, so the count and the array are passed
        // separately.  The size has to live in a named variable: the
        // address of the temporary `converted.size()` cannot be taken
        // ("cannot take the address of an rvalue"), and the array must
        // outlive the call.
        const std::size_t sample_size = converted.size();
        const CMSampleTimingInfo timing = {
            .duration = CMTimeMake(1, 1000),
            .presentationTimeStamp = CMTimeMake(0, 1),
            .decodeTimeStamp = kCMTimeInvalid,
        };
        st = CMSampleBufferCreate(kCFAllocatorDefault, block, true, nullptr,
                                  nullptr, desc, 1, 0, &timing, 1,
                                  &sample_size, &sample);
        if (owns_desc) CFRelease(desc);
        CFRelease(block);
        if (st != noErr || !sample) {
            NIMRTC_LOG_ERROR("vt: CMSampleBufferCreate failed (os="
                             << static_cast<int>(st) << ")");
            return false;
        }

        if (decoded_) {
            CVPixelBufferRelease(decoded_);
            decoded_ = nullptr;
        }

        VTDecodeFrameFlags flags = 0;
        st = VTDecompressionSessionDecodeFrame(session_, sample, flags,
                                                nullptr, nullptr);
        CFRelease(sample);
        if (st != noErr) {
            NIMRTC_LOG_ERROR("vt: VTDecompressionSessionDecodeFrame failed (os="
                             << static_cast<int>(st) << ")");
            return false;
        }

        if (!decoded_) {
            // The session produced nothing (a dropped frame).
            return false;
        }
        out = decoded_;
        return true;
    }

private:
    /** Release the decompression session and its format description.
     *
     *  The two are released together because the sample buffers handed to
     *  the decoder must carry the *same* format description the session
     *  was built from — so a description without a session is useless,
     *  and a session without its description is a dangling reference. */
    void release_session() noexcept {
        if (session_) {
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (format_desc_) {
            CFRelease(format_desc_);
            format_desc_ = nullptr;
        }
    }

    /** The output callback VideoToolbox delivers the decoded picture to. */
    static void output_callback(void* refcon,
                                void* /*sourceFrameRefCon*/,
                                OSStatus status,
                                VTDecodeInfoFlags info_flags,
                                CVImageBufferRef image_buffer,
                                CMTime /*presentation_timestamp*/,
                                CMTime /*presentation_duration*/) {
        auto* self = static_cast<VtDecoderSession*>(refcon);
        if (!self) return;
        if (status != noErr || !image_buffer) return;
        if (info_flags & kVTDecodeInfo_FrameDropped) return;

        if (self->decoded_) {
            CVPixelBufferRelease(self->decoded_);
        }
        // The session hands over a +1 reference; the decoder keeps it
        // until the next frame replaces it.
        self->decoded_ = static_cast<CVPixelBufferRef>(image_buffer);
    }

    VTDecompressionSessionRef session_ = nullptr;
    CMFormatDescriptionRef   format_desc_ = nullptr;
    CVPixelBufferRef          decoded_  = nullptr;
    vt_detail::Codec          codec_    = vt_detail::Codec::kH264;
    /// True once the session has been re-created with the stream's own
    /// parameter sets (the format description the frames are decoded
    /// with).
    bool                      desc_from_parameter_sets_ = false;
    /// The parameter sets the current session was built from — the
    /// reference the next access unit's sets are compared against, to
    /// detect a mid-stream sequence switch.
    vt_detail::ParameterSets session_sets_{};
    /// True while the session is still waiting for the stream's
    /// parameter sets: the host has a hardware decoder for the codec,
    /// but no format description can be built without the codec
    /// extension atom the parameter sets carry.
    bool                      deferred_ = false;
    /// True when the session creation from the stream's own parameter
    /// sets has failed.  The failure is a *decode error* (the stream is
    /// not decodable by this hardware), but it does not stop the retry:
    /// a transient failure must not kill the decoder for good.
    bool                      session_failed_ = false;
};

// ---------------------------------------------------------------------------
// VtDecoder — implements plugins::IVideoCodec
// ---------------------------------------------------------------------------

class VtDecoder final : public plugins::IVideoCodec {
public:
    VtDecoder(plugins::VideoCodecConfig cfg, vt_detail::Codec codec) noexcept
        : cfg_(std::move(cfg)), codec_(codec) {}

    ~VtDecoder() override { close(); }

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::nvcodec::vt_backend::VtDecoder";
    }

    plugins::Status open() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (opened_) return plugins::kOk;
        if (!session_.open(codec_)) return plugins::kErrHardwareError;
        opened_ = true;
        return plugins::kOk;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) return;
        session_.close();
        opened_ = false;
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode()   const noexcept override { return false; }
    bool can_decode()   const noexcept override { return true; }

    // ---- The encoder half (not supported by this decoder) ----------------
    //
    // `IVideoCodec` is the *combined* seam: one instance may serve
    // both directions, so the interface carries the encoder's pure
    // virtuals as well.  This decoder never encodes, so the encoder
    // half answers "unsupported" rather than being left abstract --
    // an abstract class cannot be marked `final`.
    plugins::Status encode(const plugins::VideoFrame& raw,
                           std::uint8_t* output,
                           std::size_t output_capacity,
                           plugins::EncodedVideoFrame& encoded_out) noexcept override {
        (void)raw;
        (void)output;
        (void)output_capacity;
        (void)encoded_out;
        return plugins::kErrUnsupported;
    }

    plugins::Status force_keyframe() noexcept override {
        // A decoder never produces a key frame, so there is nothing to
        // force.  "Unsupported" is the honest answer; the engine only
        // asks an *encoder* for one.
        return plugins::kErrUnsupported;
    }

    plugins::Status set_params(const plugins::VideoEncoderParams& params) noexcept override {
        (void)params;
        return plugins::kErrUnsupported;
    }

    plugins::VideoEncoderParams params() const noexcept override {
        return {};
    }

    plugins::VideoCodecKind kind() const noexcept override {
        return vt_detail::codec_kind(codec_);
    }
    std::string_view codec_name() const noexcept override {
        return vt_detail::codec_short_name(codec_);
    }

    plugins::Status decode(const plugins::EncodedVideoFrame& encoded,
                           plugins::VideoFrame& raw_out,
                           std::uint8_t* const* output_buffers) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) {
            stats_.decode_errors++;
            return plugins::kErrNotReady;
        }
        if (encoded.payload.empty()) {
            stats_.decode_errors++;
            return plugins::kErrInvalidParam;
        }

        CVPixelBufferRef pixel_buffer = nullptr;
        if (!session_.decode(encoded.payload.data(), encoded.payload.size(),
                             pixel_buffer) || !pixel_buffer) {
            // A frame that arrived *before* the stream's parameter sets
            // is not a decoding failure: the session is still waiting
            // for them, and the engine's keyframe request is what
            // brings them.  Counting it as an error would make a
            // perfectly healthy stream look broken.
            if (session_.is_deferred() &&
                !session_.session_creation_failed()) {
                return plugins::kErrNotReady;
            }
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        const std::uint32_t w =
            static_cast<std::uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
        const std::uint32_t h =
            static_cast<std::uint32_t>(CVPixelBufferGetHeight(pixel_buffer));
        if (w == 0 || h == 0) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        // ---- Copy the NV12 surface into the caller's I420 planes ---------
        //
        // The caller may supply the planes; when it does not, the
        // decoder allocates its own buffer and hands the ownership to
        // `raw_out`.
        std::shared_ptr<video_frame::VideoFrameBuffer> cpu_buf;
        std::uint8_t* y_plane = nullptr;
        std::uint8_t* u_plane = nullptr;
        std::uint8_t* v_plane = nullptr;

        const bool caller_supplied = output_buffers && output_buffers[0];
        if (caller_supplied) {
            y_plane = output_buffers[0];
            u_plane = output_buffers[1];
            v_plane = output_buffers[2];
        }
        if (!y_plane || !u_plane || !v_plane) {
            const video_frame::VideoFrameLayout layout =
                video_frame::compute_layout(video_frame::PixelFormat::kI420, w, h);
            cpu_buf = std::make_shared<video_frame::VideoFrameBuffer>(layout);
            y_plane = cpu_buf->plane(0);
            u_plane = cpu_buf->plane(1);
            v_plane = cpu_buf->plane(2);
        }

        if (!copy_surface_to_i420(pixel_buffer, y_plane, u_plane, v_plane, w, h)) {
            stats_.decode_errors++;
            return plugins::kErrInternal;
        }

        if (cpu_buf) {
            raw_out.buffer = std::move(cpu_buf);
        }
        raw_out.info.capture_ts_us = encoded.info.capture_ts_us;
        raw_out.info.frame_seq     = encoded.info.frame_seq;
        raw_out.info.rtp_timestamp = encoded.info.rtp_timestamp;

        stats_.frames_decoded++;
        stats_.bytes_decoded += encoded.payload.size();
        return plugins::kOk;
    }

    // ---- The zero-copy decode path ----------------------------------------
    //
    // The decoder can hand the `CVPixelBufferRef` the session produced
    // straight to the caller, so the picture never crosses the PCIe
    // bus into host memory.
    //
    // The answer is the *capability*, not "a session happens to exist
    // right now": the session is created from the stream's first
    // access unit, which may be later than the engine's `open()`.  A
    // decoder that will produce a native surface says so even before
    // that surface exists — otherwise the engine would pick the CPU
    // staging path for a decoder that never stages anything.
    bool supports_zero_copy_decode() const noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        return opened_ && session_.hardware_capable();
    }

    plugins::Status decode_zero_copy(const plugins::EncodedVideoFrame& encoded,
                                     plugins::GpuBuffer& out_surface) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) return plugins::kErrNotReady;

        CVPixelBufferRef pixel_buffer = nullptr;
        if (!session_.decode(encoded.payload.data(), encoded.payload.size(),
                             pixel_buffer) || !pixel_buffer) {
            // The same rule as the CPU path: a frame that arrived before
            // the stream's parameter sets is "not ready", not a decode
            // error.
            if (session_.is_deferred() &&
                !session_.session_creation_failed()) {
                return plugins::kErrNotReady;
            }
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        const std::uint32_t w =
            static_cast<std::uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
        const std::uint32_t h =
            static_cast<std::uint32_t>(CVPixelBufferGetHeight(pixel_buffer));
        if (w == 0 || h == 0) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        // The caller owns the surface from here on: the handle is handed
        // over with a +1 reference (the session's own reference is
        // released when the next frame replaces it).
        out_surface.set_handle(plugins::GpuHandle{
            plugins::GpuBackend::kAppleCFPixelBuffer,
            static_cast<void*>(pixel_buffer), 0, -1});
        out_surface.set_dimensions(w, h);
        out_surface.set_format(video_frame::PixelFormat::kNV12);

        stats_.frames_decoded++;
        stats_.bytes_decoded += encoded.payload.size();
        return plugins::kOk;
    }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }

    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        cfg_ = std::move(cfg);
        return plugins::kOk;
    }

    plugins::VideoCodecStats stats() const noexcept override { return stats_; }

    std::uint8_t payload_type() const noexcept override {
        return cfg_.payload_type != 0 ? cfg_.payload_type
                                      : vt_detail::default_payload_type(codec_);
    }

    std::string_view backend_id() const noexcept override { return "videotoolbox"; }

    bool is_hardware_accelerated() const noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        // The *capability*, not "a session happens to exist right now":
        // the session is created from the stream's first access unit,
        // which may be later than the engine's `open()`.  A deferred
        // session is still a hardware one — it is created with
        // `RequireHardwareAcceleratedVideoDecoder = true`, so the
        // software decoder would have been refused.
        return opened_ && session_.hardware_capable();
    }

private:
    /** Copy a decoded surface into three I420 planes.
     *
     *  The surface's *format* is read first: VideoToolbox's decoder
     *  produces whatever the session negotiated, which may be bi-planar
     *  (NV12) or planar (I420).  Copying an NV12 surface as if it were
     *  planar reads the chroma planes out of the wrong place, and
     *  copying a planar surface as if it were NV12 reads *past* the
     *  luma plane's allocation — a heap overrun, not a wrong picture.
     *
     *  @param stride_y  The destination luma stride (bytes).
     *  @param stride_u  The destination U stride (bytes).
     *  @param stride_v  The destination V stride (bytes). */
    static bool copy_surface_to_i420(CVPixelBufferRef pixel_buffer,
                                     std::uint8_t* y_plane,
                                     std::uint8_t* u_plane,
                                     std::uint8_t* v_plane,
                                     std::uint32_t w,
                                     std::uint32_t h,
                                     std::int32_t stride_y = 0,
                                     std::int32_t stride_u = 0,
                                     std::int32_t stride_v = 0) noexcept {
        if (!pixel_buffer || !y_plane || !u_plane || !v_plane) return false;

        const CVReturn lock_st = CVPixelBufferLockBaseAddress(pixel_buffer, 0);
        if (lock_st != kCVReturnSuccess) return false;

        const std::size_t sy = stride_y > 0 ? static_cast<std::size_t>(stride_y) : w;
        const std::size_t su = stride_u > 0 ? static_cast<std::size_t>(stride_u) : (w + 1u) / 2u;
        const std::size_t sv = stride_v > 0 ? static_cast<std::size_t>(stride_v) : (w + 1u) / 2u;
        const std::uint32_t cw = (w + 1u) / 2u;
        const std::uint32_t ch = (h + 1u) / 2u;
        const OSType format = CVPixelBufferGetPixelFormatType(pixel_buffer);

        if (format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
            format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
            // ---- The bi-planar (NV12) layout ------------------------------
            const auto* base = static_cast<const std::uint8_t*>(
                CVPixelBufferGetBaseAddress(pixel_buffer));
            const std::size_t pitch = CVPixelBufferGetBytesPerRow(pixel_buffer);
            const std::size_t uv_pitch =
                CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
            // The chroma plane's *address* comes from the buffer, not
            // from `luma_base + pitch * height`.  On an
            // IOSurface-backed buffer the planes are separate
            // allocations, so the luma pointer plus a height-sized
            // offset lands in the *middle of the luma allocation* — and
            // reading the chroma from there is a wild read.
            const auto* uv_plane = static_cast<const std::uint8_t*>(
                CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
            if (!base || pitch == 0 || uv_pitch == 0 || !uv_plane) {
                CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
                return false;
            }

            // Luma.
            for (std::uint32_t row = 0; row < h; ++row) {
                std::memcpy(y_plane + static_cast<std::size_t>(row) * sy,
                            base + static_cast<std::size_t>(row) * pitch, w);
            }

            // Chroma: the NV12 surface interleaves U and V.
            for (std::uint32_t row = 0; row < ch; ++row) {
                const std::uint8_t* src =
                    uv_plane + static_cast<std::size_t>(row) * uv_pitch;
                std::uint8_t* u_row = u_plane + static_cast<std::size_t>(row) * su;
                std::uint8_t* v_row = v_plane + static_cast<std::size_t>(row) * sv;
                for (std::uint32_t col = 0; col < cw; ++col) {
                    u_row[col] = src[col * 2 + 0];
                    v_row[col] = src[col * 2 + 1];
                }
            }
        } else if (format == kCVPixelFormatType_420YpCbCr8PlanarFullRange ||
                   format == kCVPixelFormatType_420YpCbCr8Planar) {
            // ---- The planar (I420) layout ---------------------------------
            //
            // The plane pointers come from the buffer itself: computing
            // them by hand from the luma pointer would be wrong as soon
            // as the buffer's rows are padded.
            const std::size_t y_pitch =
                CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
            const std::size_t u_pitch =
                CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
            const std::size_t v_pitch =
                CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 2);
            const auto* y_src = static_cast<const std::uint8_t*>(
                CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
            const auto* u_src = static_cast<const std::uint8_t*>(
                CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
            const auto* v_src = static_cast<const std::uint8_t*>(
                CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 2));
            if (!y_src || !u_src || !v_src ||
                y_pitch == 0 || u_pitch == 0 || v_pitch == 0) {
                CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
                return false;
            }
            for (std::uint32_t row = 0; row < h; ++row) {
                std::memcpy(y_plane + static_cast<std::size_t>(row) * sy,
                            y_src + static_cast<std::size_t>(row) * y_pitch, w);
            }
            for (std::uint32_t row = 0; row < ch; ++row) {
                std::memcpy(u_plane + static_cast<std::size_t>(row) * su,
                            u_src + static_cast<std::size_t>(row) * u_pitch, cw);
                std::memcpy(v_plane + static_cast<std::size_t>(row) * sv,
                            v_src + static_cast<std::size_t>(row) * v_pitch, cw);
            }
        } else {
            // An unexpected format: refuse rather than guess.  A wrong
            // guess reads outside the surface's allocation.
            CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
            return false;
        }

        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
        return true;
    }

    plugins::VideoCodecConfig      cfg_;
    vt_detail::Codec               codec_ = vt_detail::Codec::kH264;
    VtDecoderSession               session_;
    // `mutable`: the `supports_zero_copy_decode()` accessor is a
    // `const` member but still has to take the lock to read the
    // session's state.  A plain `std::mutex` member would make the
    // lock impossible to take from a const member function, which
    // Clang rejects outright ("1st argument ('const std::mutex')
    // would lose const qualifier").
    mutable std::mutex              mtx_;
    bool                           opened_ = false;
    plugins::VideoCodecStats       stats_{};
};

} // namespace

} // namespace nimrtc::nvcodec::vt_backend

namespace nimrtc::nvcodec {

bool vt_decoder_available(VtCodec codec) noexcept {
    const vt_detail::Codec c = vt_detail::to_detail(codec);

#if defined(__APPLE__)
    // The OS-version gate — the same floor as the encoder's.
    if (!vt_detail::os_at_least(vt_detail::min_macos_version(c))) {
        return false;
    }

    // The host's *capability* is the answer, not a probe session: a
    // hardware VideoToolbox decoder refuses a format description that
    // carries no codec extension atom, and only the stream's own
    // parameter sets can produce one.  A probe that created a session
    // would therefore report "no hardware decoder" on every Mac that
    // has one — and the hardware path would be dead everywhere.
    //
    // AV1 is the exception, and it has to be.  It carries no codec
    // extension atom at all — the description is built from the codec
    // type alone — so AV1 *can* be probed with a real session, and
    // probing it is the only honest answer.  `VTIsHardwareDecodeSupported`
    // reports what the framework believes about the codec, but the
    // session is created with
    // `kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder`,
    // so the two can disagree: on a host that reports AV1 support but
    // cannot back it with a hardware decoder, the session creation
    // fails with -12906.  A capability answer of "yes" there makes the
    // VideoToolbox backend *win* the selection and then fail to open,
    // which leaves the codec with no decoder at all — on a host that
    // does have a software AV1 decoder to fall back to.  The probe
    // asks the question the session will actually be asked.
    if (c == vt_detail::Codec::kAv1) {
        vt_backend::VtDecoderSession probe;
        return probe.open(c);
    }
    return vt_detail::hardware_decode_supported(c);
#else
    (void)c;
    return false;
#endif
}

std::unique_ptr<plugins::IVideoCodec>
make_vt_decoder(VtCodec codec, plugins::VideoCodecConfig cfg) {
#if defined(__APPLE__)
    const vt_detail::Codec c = vt_detail::to_detail(codec);
    if (!vt_detail::os_at_least(vt_detail::min_macos_version(c))) {
        NIMRTC_LOG_WARN("vt: decoder open() failed — "
                        << vt_detail::decoder_unavailable_reason(c));
        return nullptr;
    }
    auto dec = std::make_unique<vt_backend::VtDecoder>(std::move(cfg), c);
    if (dec->open() != plugins::kOk) {
        NIMRTC_LOG_WARN("vt: decoder open() failed — returning nullptr");
        return nullptr;
    }
    return dec;
#else
    (void)codec;
    (void)cfg;
    return nullptr;
#endif
}

std::string vt_decoder_unavailable_reason(VtCodec codec) {
    return vt_detail::decoder_unavailable_reason(vt_detail::to_detail(codec));
}

} // namespace nimrtc::nvcodec

#else  // !__APPLE__

namespace nimrtc::nvcodec {

bool vt_decoder_available(VtCodec) noexcept { return false; }

std::unique_ptr<plugins::IVideoCodec>
make_vt_decoder(VtCodec, plugins::VideoCodecConfig) { return nullptr; }

std::string vt_decoder_unavailable_reason(VtCodec codec) {
    return vt_detail::decoder_unavailable_reason(vt_detail::to_detail(codec));
}

} // namespace nimrtc::nvcodec

#endif // __APPLE__

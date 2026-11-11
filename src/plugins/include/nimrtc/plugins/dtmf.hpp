/**
 * @file nimrtc/plugins/dtmf.hpp
 * @brief IDtmfCodec — the RFC 4733 (telephone-event) seam.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` D1 / D2.
 *
 * ## Why this seam exists
 *
 * RFC 4733 carries DTMF digits as *in-band* audio events: the RTP
 * payload type carries a four-byte header (event code, end bit, volume,
 * duration) instead of audio samples.  A peer that negotiates
 * `telephone-event` sends those payloads on the audio m-line, and the
 * receiver has to turn them back into key events.
 *
 * NimRTC v1.1.0 had no telephone-event support at all: the payload type
 * was recognised only in the sense that it was *rejected* — the audio
 * routing table has no route for it, so the packet was counted as
 * `kAudioPayloadTypeUnknown` and dropped.
 *
 * ## Why it is not an audio codec plugin
 *
 * A telephone-event payload is **not** audio: handing it to an
 * `IAudioDecoder` would push a four-byte event header into an Opus
 * decoder.  The plan is explicit about this ("not a codec NimRTC can
 * decode"): the DTMF path is a *separate exit* next to the audio
 * routing table, not a member of it.  That is why this is its own
 * seam and not another `IAudioDecoder` implementation.
 *
 * ## Relationship to the engine
 *
 * The engine resolves the codec through the registry's DTMF slot:
 *
 *     core::PluginRegistry::instance().get_dtmf(
 *         EngineConfig::dtmf_codec_name)
 *
 * and drives it through the interface declared here.  The built-in
 * implementation lives in `src/modules/dtmf/` and registers under the
 * id "dtmf"; the engine never includes that module's headers
 * (Layout Invariant 4).
 *
 * ## The default is off
 *
 * Decision OD-B of the plan: DTMF is **off by default**.  With
 * `EngineConfig::dtmf_enabled == false` the offer carries no
 * telephone-event declaration, and a received telephone-event payload
 * keeps taking the pre-existing drop path (it counts as an unknown
 * payload type).  Only a host that explicitly enables DTMF gets the
 * declaration, the decoder and the event callback.
 */

// base.hpp must be before the include guard -- see transport.hpp for the
// rationale behind that ordering.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_DTMF_HPP
#define NIMRTC_PLUGINS_DTMF_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstdint>
#include <string>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

/** One DTMF event, as RFC 4733 describes it.
 *
 *  RFC 4733 §2.1 gives every event an *event code* (0 = DTMF digit 0,
 *  1 = DTMF digit 1, ... 9 = DTMF digit 9, 10 = `*`, 11 = `#`,
 *  12 = A, 13 = B, 14 = C, 15 = D, 16 = the "flash" event) and a
 *  *duration* in milliseconds.
 *
 *  The duration a receiver sees grows packet by packet: RFC 4733 §2.5
 *  repeats the event while the key is held, and the last packet of the
 *  sequence carries the end bit.  A receiver therefore reports the
 *  accumulated duration, not a single packet's worth of it. */
struct NIMRTC_API_STABLE DtmfEvent {
    /** The RFC 4733 event code (0..16). */
    std::uint8_t code = 0;

    /** The accumulated duration of the event, in milliseconds. */
    std::uint16_t duration_ms = 0;

    /** True when this is the end-of-event packet (RFC 4733 §2.5). */
    bool end = false;
};

// ---------------------------------------------------------------------------
// IDtmfCodec
// ---------------------------------------------------------------------------

/** The RFC 4733 telephone-event codec.
 *
 *  Thread-safety: the engine drives this from its own thread only (the
 *  thread that calls `tick()`); the interface does not promise internal
 *  locking. */
class NIMRTC_API_STABLE IDtmfCodec : public IPlugin {
public:
    /** Decode one telephone-event RTP payload.
     *
     *  RFC 4733 §2.3 (Figure 1): the payload is four bytes -- event
     *  code, the end-of-event bit (the most significant bit of byte 1,
     *  mask 0x80), the reserved bit (mask 0x40, ignored on receive),
     *  the volume (the six low bits of byte 1) and the duration (the
     *  16-bit big-endian pair of bytes 2 and 3).
     *
     *  The duration is carried in **timestamp units**, not
     *  milliseconds: the telephone-event clock rate is 8000 Hz
     *  (RFC 4733 §2.4), so one unit is 0.125 ms.  This interface
     *  reports the duration in milliseconds, and the implementation
     *  converts at the boundary.
     *
     *  @param payload The RTP payload (exactly the bytes after the RTP
     *                 header).
     *  @param out     Filled with the decoded event.
     *  @return        true when the payload was a well-formed
     *                 telephone-event payload.  A malformed payload
     *                 (wrong length, unknown event code) is rejected
     *                 rather than guessed at. */
    virtual bool decode(std::string_view payload, DtmfEvent& out) const = 0;

    /** Encode one event into a telephone-event RTP payload.
     *
     *  @param event The event to encode.
     *  @return     The four-byte payload, or an empty string when the
     *              event cannot be encoded (event code out of range). */
    virtual std::string encode(const DtmfEvent& event) const = 0;

    /** The dynamic payload type the codec uses for telephone-event.
     *
     *  RFC 4733 §2.3.2 negotiates it in the SDP; the default is 126,
     *  the value Chrome uses. */
    virtual std::uint8_t payload_type() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class NIMRTC_API_STABLE IDtmfCodecFactory {
public:
    virtual ~IDtmfCodecFactory() = default;
    virtual std::string_view id()           const noexcept = 0;
    virtual std::string_view display_name() const noexcept = 0;
    virtual IDtmfCodec* create()            const = 0;
};

template<class T>
class NIMRTC_API_STABLE SimpleDtmfCodecFactory : public IDtmfCodecFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleDtmfCodecFactory(std::string_view id,
                                    std::string_view name) noexcept
        : id_(id), name_(name) {}
    virtual std::string_view id()           const noexcept override { return id_; }
    virtual std::string_view display_name() const noexcept override { return name_; }
    virtual IDtmfCodec* create()            const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_DTMF_HPP

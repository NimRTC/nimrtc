/**
 * @file nimrtc/dtmf/dtmf.hpp
 * @brief The built-in RFC 4733 (telephone-event) codec.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` D1 / D2.
 *
 * ## What this module does
 *
 * RFC 4733 carries DTMF digits *in band*: the RTP payload type carries
 * a four-byte header (event code, end-of-event bit, volume, duration)
 * instead of audio samples.  This module implements the wire format on
 * both sides:
 *
 *   - `decode()` turns a telephone-event payload into a
 *     `plugins::DtmfEvent`;
 *   - `encode()` turns an event back into a payload.
 *
 * It is deliberately *not* an audio codec.  A telephone-event payload
 * is not audio, and handing one to an `IAudioDecoder` would push four
 * bytes of event header into an Opus decoder.  That is why the seam
 * (`plugins::IDtmfCodec`) has a slot of its own in the registry and the
 * engine drives it next to the audio routing table rather than inside
 * it.
 *
 * ## The wire format (RFC 4733 §2.3.1)
 *
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |   event code  |E|   volume   |          duration           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *   - **event code** — 0..9 are the DTMF digits, 10 is `*`, 11 is `#`,
 *     12..15 are A..D, 16 is the "flash" event (RFC 4733 §2.1).
 *   - **E** — the end-of-event bit, in the least significant bit of
 *     byte 1 (RFC 4733 §2.5).
 *   - **volume** — the volume of the digit, 0..63 (RFC 4733 §2.3.1).
 *   - **duration** — how long the event has been active, in
 *     milliseconds, big endian.
 *
 * ## The engine's side
 *
 * The engine resolves this codec through the registry's DTMF slot:
 *
 *     core::PluginRegistry::instance().get_dtmf(
 *         EngineConfig::dtmf_codec_name)
 *
 * The engine never includes this header (Layout Invariant 4).
 */

#ifndef NIMRTC_DTMF_DTMF_HPP
#define NIMRTC_DTMF_DTMF_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <nimrtc/core/api_version.hpp>

#include <cstdint>

#include <nimrtc/plugins/dtmf.hpp>

namespace nimrtc::dtmf {

/** The built-in RFC 4733 codec.
 *
 *  The codec is stateless: `decode()` and `encode()` are pure
 *  functions of their arguments.  The *repetition* and *end-of-event*
 *  timing of RFC 4733 §2.5 is the engine's job, because the engine
 *  owns the clock and the send path. */
class NIMRTC_API_STABLE TelephoneEventCodec final : public plugins::IDtmfCodec {
public:
    // IPlugin -----------------------------------------------------------
    const char* name() const noexcept override;

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // IDtmfCodec --------------------------------------------------------
    bool decode(std::string_view payload, plugins::DtmfEvent& out) const override;
    std::string encode(const plugins::DtmfEvent& event) const override;
    std::uint8_t payload_type() const noexcept override;

    /** The largest event code RFC 4733 defines (16, the "flash"
     *  event).  A payload carrying a larger code is malformed and is
     *  rejected by `decode()`. */
    static constexpr std::uint8_t kMaxEventCode = 16;

    /** The default dynamic payload type for telephone-event.
     *
     *  RFC 4733 §2.3.2 negotiates it in the SDP; 126 is the value
     *  Chrome uses and the engine's default. */
    static constexpr std::uint8_t kDefaultPayloadType = 126;

    /** The default volume of a generated event (RFC 4733 §2.3.1: 10 is
     *  the "comfortable" default). */
    static constexpr std::uint8_t kDefaultVolume = 10;
};

/**
 * @brief Register the module's default plugins.
 *
 * Called by `core::register_all_default_plugins()` (through the
 * registrar table in `src/core/src/pal_default_registrars.cpp`).  It
 * registers the codec under the id "dtmf" in the registry's DTMF
 * category.
 *
 * Idempotent: the registration happens once, on the first call.
 */
void NIMRTC_API_STABLE register_default_plugins() noexcept;

} // namespace nimrtc::dtmf
#endif // NIMRTC_DTMF_DTMF_HPP

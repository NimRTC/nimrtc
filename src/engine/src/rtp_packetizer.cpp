/**
 * @file src/engine/src/rtp_packetizer.cpp
 * @brief The engine's RTP packet builder — the ONE place the concrete
 *        `rtp::` module is touched for *outbound* packets.
 *
 * ADR-025 Decision 4: the `rtp::` usage is concentrated here, so the
 * engine's exemption from Layering Invariant 4 has a visible boundary.
 * A reader who wants to know how the engine puts bytes on the wire
 * reads this file and `rtp_packetizer.hpp` — nothing else.
 *
 * ## What is *not* here
 *
 * The *receive* side (RTCP demultiplexing, SR/RR parsing, the XR
 * blocks, the timeline reconstruction) is a different concern and stays
 * where it is: `rtcp::` is a separate module surface from `rtp::`'s
 * packet format, and the engine's RTCP path has its own collaborator
 * (`rtcp_processor` in the plan's §4.3 vocabulary) to carry it.  This
 * file concentrates the *packet building* half, which is the half that
 * ADR-025 Decision 4 names.
 */
#include "rtp_packetizer.hpp"

#include <nimrtc/rtp/packet.hpp>

namespace nimrtc::engine::detail {

namespace {

/** Apply the spec to a concrete builder.
 *
 *  The builder is stateful (it remembers the CSRC list and the
 *  extension), so it is reset first — the state must not leak from one
 *  packet to the next. */
void apply(const RtpPacketSpec& spec, rtp::PacketBuilder& pb) noexcept {
    pb.reset();
    pb.set_ssrc(spec.ssrc)
      .set_seq(spec.seq)
      .set_timestamp(spec.timestamp)
      .set_payload_type(spec.payload_type)
      .set_marker(spec.marker);
    if (spec.abs_send_time_ext_id != 0) {
        // The concrete builder treats id 0 as the historical default of
        // 1, so the "no extension" case is signalled by *not* calling
        // this at all — which is why the check is on the id.
        pb.set_abs_send_time(rtp::AbsSendTime{spec.abs_send_time_24mhz},
                              spec.abs_send_time_ext_id);
    }
    pb.set_payload(spec.payload);
}

}  // namespace

std::size_t RtpPacketizer::compute_size(const RtpPacketSpec& spec) noexcept {
    rtp::PacketBuilder pb;
    apply(spec, pb);
    return pb.compute_size();
}

std::size_t RtpPacketizer::build_into(const RtpPacketSpec& spec,
                                      std::uint8_t* out,
                                      std::size_t capacity) noexcept {
    rtp::PacketBuilder pb;
    apply(spec, pb);

    const std::size_t needed = pb.compute_size();
    if (out == nullptr || capacity < needed) return 0;

    return pb.build_into(out, capacity);
}

// ---------------------------------------------------------------------------
// The abs-send-time encoding
// ---------------------------------------------------------------------------

std::uint32_t RtpPacketizer::encode_abs_send_time_24(
    std::chrono::microseconds monotonic_us,
    std::int64_t origin_unix_us) noexcept {
    // The arithmetic is the RTP module's own (`rtp::encode_abs_send_time_24`,
    // RFC 8285 §4.2): the send instant on the Unix timeline is the origin
    // plus the monotonic reading, and the wire value is that instant at
    // 65536 Hz, truncated to 24 bits.
    //
    // The engine's timeline origin is *already* expressed in Unix
    // microseconds (`TimelineState::boot_epoch_offset_us`), so the
    // conversion from the NTP64 origin that the RTP module's overload
    // needs would be a round trip through a lossy fraction.  Doing the
    // arithmetic here keeps the two representations from drifting apart:
    // the engine stamps the extension with the same origin it uses for
    // every other instant it publishes.
    const std::int64_t unix_us = origin_unix_us + monotonic_us.count();

    // Negative means "before the origin"; the 24-bit field cannot express
    // that, so clamp to the wrap epoch (ticks 0).
    const std::int64_t us_int = unix_us < 0 ? 0 : unix_us / 1'000'000LL;
    const std::int64_t us_rem = unix_us < 0 ? 0 : unix_us % 1'000'000LL;

    // The largest term is `unix_us / 1e6 * 65536` — about 1.1e14 for a
    // current epoch (int64-safe for any realistic instant); the wrap to
    // 24 bits is applied last.
    constexpr std::uint64_t kClockRateHz = 65536;
    const std::uint64_t ticks =
        static_cast<std::uint64_t>(us_int) * kClockRateHz +
        (static_cast<std::uint64_t>(us_rem) * kClockRateHz) / 1'000'000ULL;
    return static_cast<std::uint32_t>(ticks & 0xFFFFFFULL);
}

}  // namespace nimrtc::engine::detail

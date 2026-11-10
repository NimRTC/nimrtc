/**
 * @file fuzz_rtp.cpp
 * @brief Fuzz target for the RTP and RTCP parsers.
 *
 * Every RTP packet a peer sends us lands in `rtp::Parser::parse()`, and
 * every RTCP compound datagram lands in `rtp::demux_rtcp()` plus one of
 * the per-message parsers (SR / RR / NACK / transport-cc).  Both read
 * lengths out of the packet itself, so both are places where a hostile
 * length field turns into an out-of-bounds read.
 *
 * The target therefore offers the same bytes to every one of them:
 *
 *   - the RTP parser (RFC 3550 + RFC 5285 header extensions);
 *   - the RTCP compound demultiplexer (RFC 3550 §6.1);
 *   - the per-message RTCP parsers, both on the whole buffer and on
 *     every sub-packet the demultiplexer found — the engine parses the
 *     sub-packets, not the compound, so that is the path that matters.
 *
 * The plugin adapter (`plugins::IRTP`) is driven too: it is the seam the
 * engine uses, and it is where a parsed view's lifetime is decided.
 */
#include "fuzz_targets.hpp"

#include <vector>

#include <nimrtc/plugins/rtp.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtp/rtcp.hpp>
#include <nimrtc/rtp/rtp_plugin.hpp>

namespace nimrtc::fuzz {
namespace {

/** Drive the per-message RTCP parsers over one buffer. */
void parse_rtcp_messages(const nimrtc::rtp::Parser& parser,
                         nimrtc::core::ByteSpan raw) {
    (void)parser.parse_sr(raw);
    (void)parser.parse_rr(raw);
    (void)parser.parse_nack(raw);
    (void)parser.parse_transport_cc(raw);
}

} // namespace

void run_rtp(const std::uint8_t* data, std::size_t size) {
    const nimrtc::core::ByteSpan raw{data, size};

    nimrtc::rtp::Parser parser;

    // The RTP path.
    auto packet = parser.parse(raw);
    if (packet.ok()) {
        // Touch the parsed view: a parser that returns a view pointing
        // outside the caller's buffer is a bug the fuzzer can only see
        // if somebody reads the view.
        (void)packet.value().payload_type;
        (void)packet.value().seq;
        (void)packet.value().timestamp;
        (void)packet.value().payload.size();
    }

    // The RTCP path.
    const auto blocks = nimrtc::rtp::demux_rtcp(raw);
    for (const auto& block : blocks) {
        // Each sub-packet is parsed on its own — that is what the engine
        // does with the demultiplexer's output.
        parse_rtcp_messages(parser, block.packet);
    }

    // And the whole buffer is offered to the per-message parsers as
    // well: a caller may hand a single (non-compound) RTCP packet
    // straight to them.
    parse_rtcp_messages(parser, raw);

    // The plugin seam.
    nimrtc::rtp::PluginAdapter adapter;
    const nimrtc::plugins::BufferView view{data, size};
    (void)adapter.parse_packet(view);
    (void)adapter.parse_rtcp(view);
}

} // namespace nimrtc::fuzz

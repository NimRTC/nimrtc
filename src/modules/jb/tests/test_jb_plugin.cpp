// modules/jb/tests/test_jb_plugin.cpp
// =============================================================================
// The jitter buffer's *plugin seam* tests.
//
// The adapter (`jb::PluginAdapter`) is what the rest of the engine actually
// talks to: it translates `plugins::JBConfig` into the concrete
// `jb::Config` and the concrete `jb::Stats` back into `plugins::IJB::Stats`.
//
// The memory bound (v1.3 plan Q2 / video plan R4) has to survive that
// translation on both sides:
//
//   - the *configuration* has to carry the cap across the seam, and
//   - the *drop counter* has to be reported back across the seam.
//
// A bound that only exists in the concrete module would be a bound the
// engine cannot reach.
// =============================================================================

#include <gtest/gtest.h>

#include <nimrtc/jb/jb_plugin.hpp>
#include <nimrtc/plugins/jb.hpp>
#include <nimrtc/plugins/rtp.hpp>

#include <cstdint>
#include <vector>

namespace {

using nimrtc::plugins::JBConfig;
using nimrtc::plugins::RtpPacket;
using nimrtc::plugins::TimestampUs;

// ---------------------------------------------------------------------------
// Helper — build a minimal RTP packet for the plugin seam.
//
// The plugin's push() takes a `plugins::RtpPacket`, whose `raw` view must
// be non-empty (the adapter rejects an empty one), so the helper builds a
// real 12-byte RTP header plus the payload.
// ---------------------------------------------------------------------------
RtpPacket make_packet(std::uint16_t seq, std::uint32_t ts,
                      std::size_t payload_bytes = 2,
                      std::vector<std::uint8_t>* storage = nullptr) {
    static std::vector<std::uint8_t> s_scratch;

    s_scratch.assign(12 + payload_bytes, 0x00);
    s_scratch[0] = 0x80;                     // version 2, no padding/ext
    s_scratch[1] = 96;                       // payload type
    s_scratch[2] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    s_scratch[3] = static_cast<std::uint8_t>(seq & 0xFF);
    s_scratch[4] = static_cast<std::uint8_t>((ts >> 24) & 0xFF);
    s_scratch[5] = static_cast<std::uint8_t>((ts >> 16) & 0xFF);
    s_scratch[6] = static_cast<std::uint8_t>((ts >> 8) & 0xFF);
    s_scratch[7] = static_cast<std::uint8_t>(ts & 0xFF);

    RtpPacket pkt;
    pkt.header.version       = 2;
    pkt.header.payload_type = 96;
    pkt.header.seq           = seq;
    pkt.header.ts            = ts;
    pkt.header.ssrc          = 0xCAFEBABEu;
    pkt.payload = nimrtc::core::ByteSpan{s_scratch.data() + 12, payload_bytes};
    pkt.raw     = nimrtc::core::ByteSpan{s_scratch.data(), s_scratch.size()};

    if (storage != nullptr) *storage = s_scratch;
    return pkt;
}

// =============================================================================
// The configuration crosses the seam
// =============================================================================

TEST(JbPluginConfig, TheDefaultConfigIsTheConcreteDefault) {
    // 0 means "use the implementation's default" — the adapter has to
    // substitute a real number, otherwise the bound would be off.
    JBConfig cfg;
    cfg.ssrc = 0x11223344u;

    nimrtc::jb::PluginAdapter adapter{cfg};
    ASSERT_EQ(adapter.open(), nimrtc::plugins::kOk);

    // The concrete default is 512 packets; a host that never touches the
    // cap still gets the bound.
    for (std::uint16_t i = 0; i < 600; ++i) {
        auto pkt = make_packet(i, 1000u + i);
        ASSERT_EQ(adapter.push(pkt, static_cast<TimestampUs>(i * 1000)),
                  nimrtc::plugins::kOk);
    }

    const auto s = adapter.stats();
    EXPECT_EQ(s.packets_received, 600u);
    EXPECT_EQ(s.packets_dropped, 88u) << "600 pushed, 512 kept → 88 dropped";
}

TEST(JbPluginConfig, TheCapIsCarriedAcrossTheSeam) {
    // A host that tightens the cap through the plugin seam has to see the
    // bound actually bite.  Without the translation the adapter's hard-coded
    // 512 would apply and the host's cap would be ignored.
    JBConfig cfg;
    cfg.ssrc         = 0x11223344u;
    cfg.max_packets  = 4;

    nimrtc::jb::PluginAdapter adapter{cfg};
    ASSERT_EQ(adapter.open(), nimrtc::plugins::kOk);

    for (std::uint16_t i = 0; i < 40; ++i) {
        auto pkt = make_packet(i, 1000u + i);
        ASSERT_EQ(adapter.push(pkt, static_cast<TimestampUs>(i * 1000)),
                  nimrtc::plugins::kOk);
    }

    const auto s = adapter.stats();
    EXPECT_EQ(s.packets_dropped, 36u)
        << "40 packets pushed, 4 kept → 36 dropped by the bound";
}

// =============================================================================
// The drop counter crosses the seam
// =============================================================================

TEST(JbPluginStats, TheDropCounterIsReportedBack) {
    // The bound's *report* is as important as the bound itself: a host
    // that cannot see the counter cannot tell "the receiver kept up" from
    // "the receiver silently threw audio away".
    JBConfig cfg;
    cfg.ssrc        = 0x11223344u;
    cfg.max_packets = 2;

    nimrtc::jb::PluginAdapter adapter{cfg};
    ASSERT_EQ(adapter.open(), nimrtc::plugins::kOk);

    auto s0 = adapter.stats();
    EXPECT_EQ(s0.packets_dropped, 0u);

    for (std::uint16_t i = 0; i < 10; ++i) {
        auto pkt = make_packet(i, 2000u + i);
        ASSERT_EQ(adapter.push(pkt, static_cast<TimestampUs>(i * 1000)),
                  nimrtc::plugins::kOk);
    }

    const auto s = adapter.stats();
    EXPECT_EQ(s.packets_received, 10u);
    EXPECT_EQ(s.packets_dropped, 8u);
}

TEST(JbPluginStats, AnUnboundedConfigurationKeepsEverything) {
    // A negative cap means "unbounded" — the historical behaviour stays
    // available for a caller that asks for it explicitly.
    JBConfig cfg;
    cfg.ssrc        = 0x11223344u;
    cfg.max_packets = -1;

    nimrtc::jb::PluginAdapter adapter{cfg};
    ASSERT_EQ(adapter.open(), nimrtc::plugins::kOk);

    for (std::uint16_t i = 0; i < 20; ++i) {
        auto pkt = make_packet(i, 3000u + i);
        ASSERT_EQ(adapter.push(pkt, static_cast<TimestampUs>(i * 1000)),
                  nimrtc::plugins::kOk);
    }

    const auto s = adapter.stats();
    EXPECT_EQ(s.packets_dropped, 0u);
}

TEST(JbPluginStats, ReconfiguringCarriesTheNewCap) {
    // The same seam, the other way round: a live reconfigure with a
    // tighter cap has to take effect (the concrete buffer trims to fit).
    JBConfig cfg;
    cfg.ssrc        = 0x11223344u;
    cfg.max_packets = 32;

    nimrtc::jb::PluginAdapter adapter{cfg};
    ASSERT_EQ(adapter.open(), nimrtc::plugins::kOk);

    for (std::uint16_t i = 0; i < 20; ++i) {
        auto pkt = make_packet(i, 4000u + i);
        ASSERT_EQ(adapter.push(pkt, static_cast<TimestampUs>(i * 1000)),
                  nimrtc::plugins::kOk);
    }

    JBConfig tighter = cfg;
    tighter.max_packets = 4;
    ASSERT_EQ(adapter.configure(tighter), nimrtc::plugins::kOk);

    const auto s = adapter.stats();
    EXPECT_EQ(s.packets_dropped, 16u)
        << "the tighter cap trims the buffer to fit right away";
}

}  // namespace

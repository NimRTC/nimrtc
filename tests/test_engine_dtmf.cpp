/**
 * @file tests/test_engine_dtmf.cpp
 * @brief v1.2.0 (session capability plan) — D3 and D4.
 *
 * D1 (the RFC 4733 codec) and D2 (the send path) are covered by
 * `src/modules/dtmf/tests/test_dtmf_codec.cpp`; this file covers the
 * two items that live in the *engine*:
 *
 *   1. **D3 — the SDP declaration.**  RFC 4733 §2.3.2 negotiates the
 *      telephone-event payload type in the description:
 *
 *          a=rtpmap:<pt> telephone-event/8000
 *          a=fmtp:<pt> 0-16
 *
 *      Decision OD-B made DTMF **off by default**, so the declaration
 *      is conditional: a default engine offers no telephone-event line
 *      at all, and an engine that was asked for DTMF offers one.  The
 *      payload type must also land in the m-line's format list —
 *      RFC 3550 §5.14 scopes a payload type to the m-section that
 *      declared it, so a payload type that is not in the `m=` line is
 *      not part of the session.
 *
 *   2. **D4 — the counting semantics.**  A telephone-event payload
 *      counts as an *unknown* audio payload type when DTMF was **not**
 *      negotiated, and does **not** count as one when it was.  Both
 *      directions are pinned here, because the difference is exactly
 *      what a regression would erase: the demux branch and the
 *      unknown-payload-type check sit next to each other, and moving
 *      the branch one line changes what the counters say.
 *
 * The suite uses the "shell" transport strategy (the same sentinel
 * `test_engine_timeline` uses) so `open()` reaches the open state
 * without binding ICE UDP sockets: these cases are about the SDP and
 * the inbound demux, not about the transport.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

/** The payload type RFC 4733 is normally negotiated on (the value
 *  Chrome uses, and the engine's default). */
constexpr std::uint8_t kDtmfPt = 126;

/** A peer offer that declares a telephone-event payload type.
 *
 *  The peer is a bare SDP string rather than a second engine, so the
 *  test controls exactly what the peer declared — including the
 *  event-code list, which is the input the negotiation intersects.
 *
 *  @param pt      The telephone-event payload type the peer declares.
 *  @param events  The peer's `a=fmtp:` event-code list. */
std::string peer_offer_with_telephone_event(
    std::uint8_t pt = kDtmfPt,
    std::string_view events = "0-16") {
    std::string sdp;
    sdp += "v=0\r\n";
    sdp += "o=- 123456 2 IN IP4 127.0.0.1\r\n";
    sdp += "s=-\r\n";
    sdp += "t=0 0\r\n";
    sdp += "a=group:BUNDLE 0\r\n";
    sdp += "m=audio 9 UDP/TLS/RTP/SAVPF 111 ";
    sdp += std::to_string(pt);
    sdp += "\r\n";
    sdp += "c=IN IP4 0.0.0.0\r\n";
    sdp += "a=mid:0\r\n";
    sdp += "a=rtcp-mux\r\n";
    sdp += "a=rtpmap:111 opus/48000/2\r\n";
    sdp += "a=rtpmap:" + std::to_string(pt) +
           " telephone-event/8000\r\n";
    sdp += "a=fmtp:" + std::to_string(pt) + " " + std::string(events) +
           "\r\n";
    sdp += "a=ssrc:1234 cname:peer\r\n";
    return sdp;
}

class EngineDtmf : public ::testing::Test {
protected:
    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52600;
        cfg.local_port_range_end   = 52700;
        // "shell" is the Slice 8 sentinel strategy: open() succeeds
        // without touching the network.  See test_engine_timeline.cpp
        // for why the real stack cannot be used on CI runners.
        cfg.transport_strategy_name = "shell";
        return cfg;
    }

    /** One media section of an SDP: the `m=` line and the media-level
     *  attributes that follow it, up to the next `m=` line. */
    struct MediaSection {
        std::string m_line;
        std::string attributes;
    };

    /** Every media section of @p sdp, in order.
     *
     *  The sections are cut at the `m=` lines themselves, never by
     *  searching for the *text* of an m-line: several m-lines can
     *  carry identical text, and a text search would always hit the
     *  first one. */
    static std::vector<MediaSection> media_sections(const std::string& sdp) {
        std::vector<std::size_t> starts;
        std::size_t pos = 0;
        while (true) {
            std::size_t at = std::string::npos;
            if (sdp.rfind("m=", 0) == 0) {
                at = 0;
            } else {
                const std::size_t p = sdp.find("\nm=", pos);
                if (p == std::string::npos) break;
                at = p + 1;
            }
            starts.push_back(at);
            pos = at + 2;
            if (at == 0) continue;
        }

        std::vector<MediaSection> out;
        for (std::size_t i = 0; i < starts.size(); ++i) {
            const std::size_t s = starts[i];
            std::size_t e = sdp.find("\r\n", s);
            if (e == std::string::npos) e = sdp.size();

            MediaSection section;
            section.m_line = sdp.substr(s, e - s);

            std::size_t a = sdp.find("\r\n", e == sdp.size() ? e : e + 2);
            while (a != std::string::npos &&
                   sdp.compare(a + 2, 2, "m=") != 0) {
                std::size_t b = sdp.find("\r\n", a + 2);
                if (b == std::string::npos) {
                    section.attributes += sdp.substr(a + 2);
                    a = std::string::npos;
                    break;
                }
                section.attributes += sdp.substr(a + 2, b - a - 2);
                section.attributes += "\n";
                a = b;
            }
            out.push_back(std::move(section));
        }
        return out;
    }

    /** The audio media section of @p sdp (the first one). */
    static MediaSection audio_section(const std::string& sdp) {
        for (const auto& s : media_sections(sdp)) {
            if (s.m_line.rfind("m=audio", 0) == 0) return s;
        }
        return MediaSection{};
    }

    /** The payload types the audio m-line's format list names.
     *
     *  RFC 3550 §5.14 scopes a payload type to its m-section, so this
     *  list is the set of payload types the session actually carries
     *  on the audio section. */
    static std::vector<std::string> audio_formats(const std::string& sdp) {
        std::vector<std::string> out;
        const MediaSection a = audio_section(sdp);
        const std::size_t sp = a.m_line.find(' ');
        if (sp == std::string::npos) return out;
        const std::size_t sp2 = a.m_line.find(' ', sp + 1);
        if (sp2 == std::string::npos) return out;

        std::size_t pos = sp2 + 1;
        while (pos < a.m_line.size()) {
            std::size_t end = a.m_line.find(' ', pos);
            if (end == std::string::npos) end = a.m_line.size();
            out.push_back(a.m_line.substr(pos, end - pos));
            pos = end + 1;
        }
        return out;
    }

    /** How many times @p needle appears in @p haystack. */
    static std::size_t count_of(const std::string& haystack,
                                std::string_view needle) {
        std::size_t n = 0;
        std::size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    }

    /** One telephone-event RTP packet.
     *
     *  RFC 4733 §2.3 (Figure 1): event code, the E bit (bit 7 of byte
     *  1) with the volume in the low six bits, and the duration as a
     *  16-bit big-endian count of 8000 Hz timestamp units. */
    static nimrtc::core::ByteBuffer make_telephone_event(
        std::uint32_t ssrc,
        std::uint8_t code,
        std::uint16_t duration_units,
        bool end) {
        std::uint8_t payload[4];
        payload[0] = code;
        payload[1] = static_cast<std::uint8_t>(
            (end ? 0x80 : 0x00) | 0x0A);      // E bit + the default volume
        payload[2] = static_cast<std::uint8_t>((duration_units >> 8) & 0xFF);
        payload[3] = static_cast<std::uint8_t>(duration_units & 0xFF);

        nimrtc::rtp::PacketBuilder pb;
        pb.set_ssrc(ssrc).set_seq(0).set_timestamp(0).set_payload_type(kDtmfPt);
        pb.set_payload(nimrtc::core::ByteSpan(payload, sizeof(payload)));
        return pb.build();
    }
};

// =====================================================================
// D3 — the SDP declaration
// =====================================================================

TEST_F(EngineDtmf, TheDefaultEngineOffersNoTelephoneEventDeclaration) {
    EngineConfig cfg = make_test_config();
    ASSERT_FALSE(cfg.dtmf_enabled)
        << "OD-B: DTMF is off by default, so a default offer must carry "
           "no telephone-event line";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    EXPECT_EQ(count_of(offer, "telephone-event"), 0u)
        << "a default offer must not advertise a capability the host "
           "did not ask for";
    engine.close();
}

TEST_F(EngineDtmf, AnEnabledEngineDeclaresTheTelephoneEventPayloadType) {
    EngineConfig cfg = make_test_config();
    cfg.dtmf_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    // RFC 4733 §2.3.2 / §2.4: the encoding name and the clock rate.
    EXPECT_NE(offer.find("a=rtpmap:" + std::to_string(kDtmfPt) +
                         " telephone-event/8000"),
              std::string::npos)
        << "the offer must declare the telephone-event payload type";

    // The event-code list (RFC 4733 §2.4).  The engine implements the
    // whole range the RFC defines (0..16).
    EXPECT_NE(offer.find("a=fmtp:" + std::to_string(kDtmfPt) + " 0-16"),
              std::string::npos)
        << "the offer must declare the supported event codes";

    // RFC 3550 §5.14: the payload type has to be in the m-line's format
    // list, otherwise it is not part of the session at all.
    const auto formats = audio_formats(offer);
    ASSERT_FALSE(formats.empty());
    bool found = false;
    for (const auto& f : formats) {
        if (f == std::to_string(kDtmfPt)) found = true;
    }
    EXPECT_TRUE(found)
        << "the telephone-event payload type must be in the audio m-line's "
           "format list";

    engine.close();
}

TEST_F(EngineDtmf, TheTelephoneEventPayloadTypeIsDeclaredExactlyOnce) {
    EngineConfig cfg = make_test_config();
    cfg.dtmf_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    // A payload type listed twice in the format list is a malformed
    // description, and some peers reject it outright.
    const auto formats = audio_formats(offer);
    std::size_t occurrences = 0;
    for (const auto& f : formats) {
        if (f == std::to_string(kDtmfPt)) ++occurrences;
    }
    EXPECT_EQ(occurrences, 1u)
        << "the telephone-event payload type must appear exactly once in "
           "the format list";

    // ... and the declaration itself is not emitted twice either.
    EXPECT_EQ(count_of(offer, "telephone-event/8000"), 1u);

    engine.close();
}

TEST_F(EngineDtmf, TheAnswerEchoesTheTelephoneEventDeclaration) {
    EngineConfig cfg = make_test_config();
    cfg.dtmf_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto answer = engine.process_remote_sdp(
        peer_offer_with_telephone_event());
    ASSERT_TRUE(answer.has_value());

    // RFC 3264 §6.1: the answer's format list is a subset of the
    // offer's, and the answerer uses the *offerer's* payload type.
    EXPECT_NE(answer->find("a=rtpmap:" + std::to_string(kDtmfPt) +
                            " telephone-event/8000"),
              std::string::npos)
        << "the answer must echo the telephone-event declaration";

    const auto formats = audio_formats(*answer);
    bool found = false;
    for (const auto& f : formats) {
        if (f == std::to_string(kDtmfPt)) found = true;
    }
    EXPECT_TRUE(found)
        << "the answer's format list must carry the telephone-event "
           "payload type";

    engine.close();
}

TEST_F(EngineDtmf, TheAnswerCarriesTheNegotiatedEventCodes) {
    EngineConfig cfg = make_test_config();
    cfg.dtmf_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The peer declared a *narrower* range than the engine supports
    // (0..15 instead of 0..16).  RFC 3264 §6.1: the answer carries the
    // negotiated subset, not a blind echo of the offer and not the
    // engine's own wider list.
    const auto answer = engine.process_remote_sdp(
        peer_offer_with_telephone_event(kDtmfPt, "0-15"));
    ASSERT_TRUE(answer.has_value());

    EXPECT_NE(answer->find("a=fmtp:" + std::to_string(kDtmfPt) + " 0-15"),
              std::string::npos)
        << "the answer must carry the intersection of the peer's event "
           "codes and ours";

    EXPECT_EQ(answer->find("a=fmtp:" + std::to_string(kDtmfPt) + " 0-16"),
              std::string::npos)
        << "the answer must not claim support for an event code the peer "
           "never offered";

    engine.close();
}

TEST_F(EngineDtmf, TheNegotiatedPayloadTypeIsReportedToTheHost) {
    EngineConfig cfg = make_test_config();
    cfg.dtmf_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_EQ(engine.negotiated_dtmf_payload_type(), 0u)
        << "nothing is negotiated before a description exchange";

    ASSERT_TRUE(engine.process_remote_sdp(
                    peer_offer_with_telephone_event())
                    .has_value());

    EXPECT_EQ(engine.negotiated_dtmf_payload_type(), kDtmfPt)
        << "the host must be able to ask whether a key press can reach "
           "the peer";

    engine.close();
}

// =====================================================================
// D4 — the counting semantics
// =====================================================================

TEST_F(EngineDtmf, ANegotiatedTelephoneEventIsNotCountedAsUnknownAudio) {
    EngineConfig cfg = make_test_config();
    cfg.dtmf_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The peer declared the payload type and we agreed to it.
    ASSERT_TRUE(engine.process_remote_sdp(
                    peer_offer_with_telephone_event())
                    .has_value());
    ASSERT_EQ(engine.negotiated_dtmf_payload_type(), kDtmfPt);

    // The error channel is the observable for the unknown-payload-type
    // path: the demux reports the *first* such packet through
    // `kAudioPayloadTypeUnknown`.
    std::uint32_t unknown_errors = 0;
    engine.set_on_error([&](std::uint32_t err, std::string_view) {
        if (err == nimrtc::core::kAudioPayloadTypeUnknown) ++unknown_errors;
    });

    // A well-formed telephone-event payload: event code 5, the E bit
    // set, 160 units = 20 ms at 8000 Hz.
    const auto pkt = make_telephone_event(0x11112222u, 5, 160, /*end=*/true);
    ASSERT_FALSE(pkt.empty());
    engine.feed_media_inbound(pkt.data(), pkt.size());

    EXPECT_EQ(unknown_errors, 0u)
        << "a negotiated telephone-event payload must not be reported as "
           "an unknown audio payload type (D4)";

    const auto st = engine.stats();
    EXPECT_EQ(st.dtmf_packets_received, 1u)
        << "the packet must have been decoded as a telephone event";
    EXPECT_EQ(st.dtmf_events_delivered, 1u)
        << "the completed event must have been reported to the host";

    engine.close();
}

TEST_F(EngineDtmf, AnUnnegotiatedTelephoneEventIsCountedAsUnknownAudio) {
    EngineConfig cfg = make_test_config();
    // DTMF stays off: the peer sends a telephone-event payload we
    // never agreed to, so it is an anomaly like any other unknown
    // payload type.
    ASSERT_FALSE(cfg.dtmf_enabled);

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The peer *does* declare the payload type, but DTMF is off here,
    // so nothing is negotiated (decision D4).
    ASSERT_TRUE(engine.process_remote_sdp(
                    peer_offer_with_telephone_event())
                    .has_value());
    EXPECT_EQ(engine.negotiated_dtmf_payload_type(), 0u)
        << "a payload type we never asked for must not be negotiated when "
           "DTMF is off";

    std::uint32_t unknown_errors = 0;
    engine.set_on_error([&](std::uint32_t err, std::string_view) {
        if (err == nimrtc::core::kAudioPayloadTypeUnknown) ++unknown_errors;
    });

    const auto pkt = make_telephone_event(0x11112222u, 5, 160, /*end=*/true);
    ASSERT_FALSE(pkt.empty());
    engine.feed_media_inbound(pkt.data(), pkt.size());

    EXPECT_EQ(unknown_errors, 1u)
        << "an unnegotiated telephone-event payload must still count as "
           "an unknown audio payload type (D4)";

    const auto st = engine.stats();
    EXPECT_EQ(st.dtmf_packets_received, 0u)
        << "the packet must not have been decoded as a telephone event";

    engine.close();
}

TEST_F(EngineDtmf, AMalformedTelephoneEventIsCountedSeparately) {
    EngineConfig cfg = make_test_config();
    cfg.dtmf_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.process_remote_sdp(
                    peer_offer_with_telephone_event())
                    .has_value());

    // A telephone-event payload of the wrong length: the codec rejects
    // it rather than guessing at it.
    const std::uint8_t bad[3] = {0x05, 0x80, 0x00};
    nimrtc::rtp::PacketBuilder pb;
    pb.set_ssrc(0x33334444u).set_seq(0).set_timestamp(0)
      .set_payload_type(kDtmfPt);
    pb.set_payload(nimrtc::core::ByteSpan(bad, sizeof(bad)));
    const auto pkt = pb.build();
    ASSERT_FALSE(pkt.empty());
    engine.feed_media_inbound(pkt.data(), pkt.size());

    const auto st = engine.stats();
    EXPECT_EQ(st.dtmf_packets_malformed, 1u)
        << "a malformed payload must be counted as malformed";
    EXPECT_EQ(st.dtmf_packets_received, 0u)
        << "a malformed payload must not be counted as a decoded event";

    engine.close();
}

TEST_F(EngineDtmf, SendDtmfFailsWhenNoPayloadTypeWasNegotiated) {
    EngineConfig cfg = make_test_config();
    cfg.dtmf_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Nothing was negotiated, so a key press cannot reach the peer and
    // the host must be told so (rather than a packet going out on a
    // payload type the peer never agreed to — RFC 3264 §6.1).
    EXPECT_FALSE(engine.send_dtmf(5, 100))
        << "send_dtmf() must fail when no telephone-event payload type "
           "was negotiated";

    engine.close();
}

TEST_F(EngineDtmf, SendDtmfSucceedsOnceThePayloadTypeIsNegotiated) {
    EngineConfig cfg = make_test_config();
    cfg.dtmf_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.process_remote_sdp(
                    peer_offer_with_telephone_event())
                    .has_value());
    ASSERT_EQ(engine.negotiated_dtmf_payload_type(), kDtmfPt);

    EXPECT_TRUE(engine.send_dtmf(5, 100))
        << "a key press must be accepted once the payload type is "
           "negotiated";

    // A zero duration is rejected: RFC 4733 §2.3.5 makes it a *state*,
    // not a key press.
    EXPECT_FALSE(engine.send_dtmf(5, 0))
        << "a zero duration is not a key press";

    // An event code outside the RFC's range is rejected.
    EXPECT_FALSE(engine.send_dtmf(17, 100))
        << "event code 17 is outside the range RFC 4733 §2.1 defines";

    engine.close();
}

// ============================================================================
// D2 — the send path on the wire (RFC 4733 §2.5)
// ============================================================================
//
// The suite above runs on the "shell" transport, which never puts a
// packet on a wire.  The D2 acceptance criterion is about the wire —
// "a key press must reach the peer with its event code and its
// duration, and the end packet must appear with the RFC 4733 §2.5
// timing" — so this fixture drives the engine through the same two
// fakes `test_engine_audio_send.cpp` uses: a fake ICE transport that
// captures every outbound datagram and a fake DTLS session that
// reports the handshake as complete but exports no SRTP keying
// material.  The second one is what makes the send path reachable, and
// it is also why the captured datagrams are plain RTP: with no
// keying material the engine never installs an SRTP context, so the
// test can read the telephone-event payload directly.

/** A fake ICE transport that reports `Connected` and captures every
 *  outbound datagram. */
class FakeIceTransport final : public np::IICETransport {
public:
    // ---- IPlugin ---------------------------------------------------------
    const char* name() const noexcept override { return "fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    // ---- ITransport ------------------------------------------------------
    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_  = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        sent_.emplace_back(view.data(), view.data() + view.size());
        return np::kOk;
    }

    int recv() noexcept override {
        int n = 0;
        while (!inbound_.empty()) {
            std::vector<std::uint8_t> pkt = std::move(inbound_.front());
            inbound_.erase(inbound_.begin());
            if (on_recv_) on_recv_(np::BufferView(pkt.data(), pkt.size()));
            ++n;
        }
        return n;
    }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    // ---- IICETransport ---------------------------------------------------
    np::IceState state() const noexcept override { return np::IceState::Connected; }
    std::string local_ufrag() const noexcept override { return "fakeufrag"; }
    std::string local_password() const noexcept override { return "fakepassword"; }
    bool wait_for_gathering(int) noexcept override { return true; }
    std::vector<std::string> gathered_local_candidates() const noexcept override {
        return {};
    }
    np::Status set_remote_description(std::string_view) noexcept override {
        return np::kOk;
    }
    np::Status add_remote_candidate(std::string_view) noexcept override {
        return np::kOk;
    }
    void set_bind_address(std::string_view) noexcept override {}
    void set_stun_server(std::string_view, std::uint16_t) noexcept override {}
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view, std::uint16_t, std::string_view,
                         std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

    // ---- Test helpers ----------------------------------------------------
    const std::vector<std::vector<std::uint8_t>>& sent() const noexcept {
        return sent_;
    }

private:
    np::RecvCallback   on_recv_;
    np::ErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> inbound_;
    std::vector<std::vector<std::uint8_t>> sent_;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE transport (test)";
    }
    np::IICETransport* create_ice() const override {
        auto* t = new FakeIceTransport();
        last_created() = t;
        return t;
    }
    static FakeIceTransport*& last_created() {
        static FakeIceTransport* instance = nullptr;
        return instance;
    }
};

/** A DTLS session that reports the handshake as complete and exports no
 *  SRTP keying material -- the same fake `test_engine_audio_send.cpp`
 *  uses, for the same reason: no keys means no SRTP, so the captured
 *  datagrams are plain RTP. */
class FakeDtlsSession final : public nd::IDtlsSession {
public:
    // ---- The seam surface ------------------------------------------------
    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }

    // ---- The legacy engine-facing surface --------------------------------
    nimrtc::core::Result<void> open() noexcept override {
        return nimrtc::core::Result<void>::make_ok();
    }
    void set_role(nd::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
    std::size_t feed_inbound(std::span<const std::uint8_t>,
                             const nd::DtlsAddr&) noexcept override {
        return 0;
    }
    std::vector<nd::DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override {}
    nd::DtlsState state() const noexcept override { return nd::DtlsState::Connected; }
    bool is_connected() const noexcept override { return true; }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

private:
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

class FakeDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

class EngineDtmfTx : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        static FakeDtlsFactory dtls_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);
        PluginRegistry::instance().register_dtls_session("fake-dtls", &dtls_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name          = "fake-ice";
        cfg.dtls_name               = "fake-dtls";
        cfg.datachannel_backend     = "";
        cfg.bwe_name                = "";
        cfg.scheduler_config.enabled = false;
        cfg.dtmf_enabled            = true;
        return cfg;
    }

    /** A decoded telephone-event payload (RFC 4733 §2.3, Figure 1). */
    struct EventView {
        std::uint8_t  code = 0;
        bool          end = false;
        std::uint16_t duration_ms = 0;
    };

    /** Decode a four-byte telephone-event payload.
     *
     *  The duration field counts 8000 Hz timestamp units, not
     *  milliseconds (RFC 4733 §2.3.5) -- the conversion is the detail
     *  a peer gets wrong, so the test does it explicitly rather than
     *  trusting the engine's own codec. */
    static EventView decode_event(const nimrtc::rtp::PacketView& pv) {
        EventView ev;
        if (pv.payload.size() < 4) return ev;
        const std::uint8_t* p = pv.payload.data();
        ev.code = p[0];
        ev.end  = (p[1] & 0x80) != 0;               // the E bit
        const std::uint16_t units =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[2]) << 8) | p[3]);
        ev.duration_ms = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(units) * 125u) / 1000u);
        return ev;
    }

    /** The telephone-event packets the engine put on the wire. */
    static std::vector<nimrtc::rtp::PacketView> dtmf_packets(
        const std::vector<std::vector<std::uint8_t>>& datagrams) {
        std::vector<nimrtc::rtp::PacketView> out;
        nimrtc::rtp::Parser parser;
        for (const auto& d : datagrams) {
            auto parsed = parser.parse(nimrtc::core::ByteSpan(d.data(), d.size()));
            if (!parsed.ok()) continue;
            const nimrtc::rtp::PacketView& view = parsed.value();
            if (view.payload_type != kDtmfPt) continue;
            out.push_back(view);
        }
        return out;
    }
};

TEST_F(EngineDtmfTx, AKeyPressGoesOutAsAnRfc4733EventSequence) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.process_remote_sdp(
                    peer_offer_with_telephone_event()).has_value());
    ASSERT_EQ(engine.negotiated_dtmf_payload_type(), kDtmfPt);

    ASSERT_TRUE(engine.send_dtmf(5, 100));

    // The first packet leaves right away: the peer learns about the
    // key press without waiting for the next tick.
    auto packets = dtmf_packets(FakeIceFactory::last_created()->sent());
    ASSERT_EQ(packets.size(), 1u);

    // RFC 4733 §2.5.1.2: the event is a *sequence*.  The packet is
    // repeated while the key is held -- each repeat carrying a larger
    // duration -- and the packet that closes the event carries the
    // end-of-event bit.  The repeats are paced by the engine's tick.
    for (int i = 0; i < 20; ++i) {
        bool ended = false;
        for (const auto& p : packets) {
            if (decode_event(p).end) ended = true;
        }
        if (ended) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        engine.tick();
        packets = dtmf_packets(FakeIceFactory::last_created()->sent());
    }

    ASSERT_GE(packets.size(), 2u)
        << "RFC 4733 §2.5.1.2: the event must be repeated while the key is "
           "held, not sent as a single packet";

    // Every packet of the event carries the event code the host asked
    // for -- that is what the peer decodes the key press from.
    for (const auto& p : packets) {
        EXPECT_EQ(decode_event(p).code, 5u);
    }

    // RFC 4733 §2.2.2: only the *first* packet of an event carries the
    // marker bit -- that is what tells the peer "a new event starts
    // here".
    EXPECT_TRUE(packets.front().marker)
        << "the first packet must mark the beginning of the event";
    for (std::size_t i = 1; i < packets.size(); ++i) {
        EXPECT_FALSE(packets[i].marker)
            << "only the first packet of an event carries the marker bit";
    }

    // The event closes with the end-of-event bit, and the closing
    // packet reports the duration the host asked for.
    const EventView last = decode_event(packets.back());
    EXPECT_TRUE(last.end)
        << "the last packet of the event must carry the end-of-event bit";
    EXPECT_EQ(last.duration_ms, 100u)
        << "the peer must learn how long the key was held";

    // The duration grows from packet to packet (RFC 4733 §2.5.1.2): the
    // peer reconstructs the key press from the largest one it sees.
    for (std::size_t i = 1; i < packets.size(); ++i) {
        EXPECT_GE(decode_event(packets[i]).duration_ms,
                  decode_event(packets[i - 1]).duration_ms);
    }

    // RFC 4733 §2.1: the event packets share the audio stream's
    // sequence number space and timestamp base.
    for (std::size_t i = 1; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].seq,
                  static_cast<std::uint16_t>(packets[i - 1].seq + 1))
            << "the event packets share the audio sequence space";
        EXPECT_EQ(packets[i].timestamp, packets[0].timestamp);
        EXPECT_EQ(packets[i].ssrc, packets[0].ssrc);
    }

    EXPECT_EQ(engine.stats().dtmf_packets_sent, packets.size());

    engine.close();
}

TEST_F(EngineDtmfTx, AKeyPressIsNotPutOnTheWireWithoutANegotiatedPayloadType) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // No description exchange: nothing was negotiated, so the key press
    // cannot reach the peer (RFC 3264 §6.1).
    EXPECT_FALSE(engine.send_dtmf(5, 100));

    auto packets = dtmf_packets(FakeIceFactory::last_created()->sent());
    EXPECT_TRUE(packets.empty())
        << "no telephone-event packet may go out on a payload type the "
           "peer never agreed to";

    engine.close();
}

} // namespace

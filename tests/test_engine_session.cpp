/**
 * @file tests/test_engine_session.cpp
 * @brief v1.2.0 (session capability plan) — N1 … N5 and T1 … T4.
 *
 * The engine's renegotiation surface is pinned down here.  Six
 * properties matter, and each of them is one of the plan's acceptance
 * criteria:
 *
 *   1. **N1 — the engine drives a pluggable session model.**  The
 *      model is resolved through the registry's session slot
 *      (`EngineConfig::session_name`), and the engine reaches it only
 *      through `plugins::ISession` — `src/engine/` contains no
 *      include of `nimrtc/session/**`.
 *
 *   2. **N2 — the negotiation trigger.**  A host that adds or removes
 *      a track can ask the engine whether a renegotiation is due
 *      (`needs_negotiation()`), and the flag is cleared once the new
 *      offer is produced.
 *
 *   3. **N3 — the m-line ↔ track mapping.**  A track the host added
 *      shows up as a new m-line, and the mid of an existing m-line
 *      never moves.  The BUNDLE group lists exactly the mids that are
 *      in the description.
 *
 *   4. **N4 — the direction mapping.**  A track's direction lands on
 *      the m-line as `a=sendonly` / `a=recvonly` / `a=sendrecv` /
 *      `a=inactive`.
 *
 *   5. **N5 / OD-A — a removed track does not remove the m-line.**
 *      The m-line stays, marked `inactive`, and its mid is not
 *      recycled.
 *
 *   6. **T2 / T3 — a multi-m-line offer is answered one-to-one.**  The
 *      answer mirrors the offer's m-sections, each with its own mid.
 *
 * The suite uses the "shell" transport strategy (the same sentinel
 * `test_engine_audio_state` uses) so `open()` reaches the open state
 * without binding ICE UDP sockets: these cases are about the session
 * and the SDP, not about the transport.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/session.hpp>

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using nimrtc::plugins::Direction;
using nimrtc::plugins::MediaKind;

/** One media section of an SDP: the `m=` line and the media-level
 *  attributes that follow it, up to the next `m=` line. */
struct MediaSection {
    std::string m_line;
    std::string attributes;
};

/** Every media section of @p sdp, in order.
 *
 *  The sections are cut at the `m=` lines themselves, never by
 *  searching for the *text* of an m-line: several m-lines can carry
 *  identical text (`m=audio 9 UDP/TLS/RTP/SAVPF 111`), and a text
 *  search would always hit the first one. */
std::vector<MediaSection> media_sections(const std::string& sdp) {
    // The offsets of the `m=` lines, in order.
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

        const std::size_t body_end = (i + 1 < starts.size()) ? starts[i + 1]
                                                            : sdp.size();
        if (body_end > e) section.attributes = sdp.substr(e, body_end - e);

        out.push_back(std::move(section));
    }
    return out;
}

/** Every `m=` line of @p sdp, in order. */
std::vector<std::string> media_lines(const std::string& sdp) {
    std::vector<std::string> out;
    for (const auto& section : media_sections(sdp)) out.push_back(section.m_line);
    return out;
}

/** The `a=mid:` value of the m-line at @p index, or "" when it has
 *  none. */
std::string mid_of(const std::string& sdp, std::size_t index) {
    const auto sections = media_sections(sdp);
    if (index >= sections.size()) return {};

    const std::string& attrs = sections[index].attributes;
    const std::string key = "a=mid:";
    const std::size_t p = attrs.find(key);
    if (p == std::string::npos) return {};
    const std::size_t v = p + key.size();
    std::size_t e = v;
    while (e < attrs.size() && attrs[e] != '\r' && attrs[e] != '\n') ++e;
    return attrs.substr(v, e - v);
}

/** The media-level direction attribute of the m-line at @p index
 *  ("sendonly", "recvonly", "sendrecv", "inactive"), or "" when the
 *  section carries none. */
std::string direction_of(const std::string& sdp, std::size_t index) {
    const auto sections = media_sections(sdp);
    if (index >= sections.size()) return {};

    const std::string& attrs = sections[index].attributes;
    for (const char* dir : {"sendonly", "recvonly", "sendrecv", "inactive"}) {
        const std::string attr = std::string("a=") + dir;
        std::size_t p = attrs.find(attr);
        while (p != std::string::npos) {
            const std::size_t after = p + attr.size();
            const bool boundary = after >= attrs.size() || attrs[after] == '\r' ||
                                  attrs[after] == '\n';
            if (boundary) return dir;
            p = attrs.find(attr, p + 1);
        }
    }
    return {};
}

/** The mids listed in the `a=group:BUNDLE` line, in order. */
std::vector<std::string> bundle_mids(const std::string& sdp) {
    std::vector<std::string> out;
    const std::size_t p = sdp.find("a=group:BUNDLE");
    if (p == std::string::npos) return out;
    std::size_t e = sdp.find("\r\n", p);
    if (e == std::string::npos) e = sdp.size();
    const std::string line = sdp.substr(p, e - p);
    // The line reads "a=group:BUNDLE 0 1 2".
    std::size_t i = line.find("BUNDLE");
    if (i == std::string::npos) return out;
    i = line.find(' ', i);
    while (i != std::string::npos && i + 1 < line.size()) {
        ++i;
        if (line[i] == ' ') continue;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ') ++j;
        out.push_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

class EngineSession : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52800;
        cfg.local_port_range_end   = 52900;
        // "shell" is the Slice 8 sentinel strategy: open() succeeds
        // without touching the network.
        cfg.transport_strategy_name = "shell";
        return cfg;
    }
};

// -----------------------------------------------------------------------------
// N1 — the engine resolves a session model through the registry
// -----------------------------------------------------------------------------
TEST_F(EngineSession, SessionModelIsResolvedFromTheRegistry) {
    ASSERT_NE(nimrtc::core::PluginRegistry::instance().get_session("session"),
              nullptr)
        << "the built-in session model must be registered under \"session\"";

    EngineConfig cfg = make_test_config();
    cfg.session_name = "session";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The base audio transceiver exists as soon as the engine is open.
    const auto tr = engine.transceivers();
    ASSERT_FALSE(tr.empty());
    EXPECT_EQ(tr[0].kind, MediaKind::kAudio);
    engine.close();
}

TEST_F(EngineSession, AnUnknownSessionModelFallsBackToTheFixedMediaList) {
    EngineConfig cfg = make_test_config();
    cfg.session_name = "no-such-session-model";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // No session model → no tracks, no transceivers, and the track API
    // reports the failure instead of pretending.
    EXPECT_TRUE(engine.tracks().empty());
    EXPECT_TRUE(engine.transceivers().empty());
    EXPECT_FALSE(engine.add_track("a", MediaKind::kAudio, Direction::kSendRecv));

    // The offer still works — it falls back to the pre-v1.2.0 fixed
    // two-m-line list.
    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());
    EXPECT_FALSE(media_lines(offer).empty());
    engine.close();
}

TEST_F(EngineSession, AnEmptySessionNameDisablesTheSessionModel) {
    EngineConfig cfg = make_test_config();
    cfg.session_name = "";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_TRUE(engine.transceivers().empty());
    EXPECT_FALSE(engine.add_track("a", MediaKind::kAudio, Direction::kSendRecv));
    engine.close();
}

// -----------------------------------------------------------------------------
// N2 — the negotiation trigger
// -----------------------------------------------------------------------------
TEST_F(EngineSession, NeedsNegotiationFlipsOnAMutationAndClearsOnOffer) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The offer that opened the round is the baseline.
    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());
    EXPECT_FALSE(engine.needs_negotiation());

    ASSERT_TRUE(engine.add_track("mic", MediaKind::kAudio, Direction::kSendRecv));
    EXPECT_TRUE(engine.needs_negotiation());

    const std::string offer2 = engine.create_offer();
    ASSERT_FALSE(offer2.empty());
    EXPECT_FALSE(engine.needs_negotiation());

    ASSERT_TRUE(engine.remove_track("mic"));
    EXPECT_TRUE(engine.needs_negotiation());
    engine.close();
}

TEST_F(EngineSession, ARejectedMutationDoesNotFlipTheFlag) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_FALSE(engine.create_offer().empty());
    EXPECT_FALSE(engine.needs_negotiation());

    // A duplicate id is rejected — the session did not change.
    ASSERT_TRUE(engine.add_track("mic", MediaKind::kAudio, Direction::kSendRecv));
    ASSERT_FALSE(engine.create_offer().empty());
    EXPECT_FALSE(engine.needs_negotiation());

    EXPECT_FALSE(engine.add_track("mic", MediaKind::kAudio, Direction::kSendRecv));
    EXPECT_FALSE(engine.needs_negotiation());

    // An unknown track is rejected too.
    EXPECT_FALSE(engine.remove_track("ghost"));
    EXPECT_FALSE(engine.needs_negotiation());
    engine.close();
}

// -----------------------------------------------------------------------------
// N3 — the m-line ↔ track mapping
// -----------------------------------------------------------------------------
TEST_F(EngineSession, AnAddedTrackGrowsTheMediaListByOneMLine) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer1 = engine.create_offer();
    ASSERT_FALSE(offer1.empty());
    const auto before = media_lines(offer1);
    ASSERT_FALSE(before.empty());

    ASSERT_TRUE(engine.add_track("mic2", MediaKind::kAudio, Direction::kSendRecv));
    const std::string offer2 = engine.create_offer();
    ASSERT_FALSE(offer2.empty());

    const auto after = media_lines(offer2);
    EXPECT_EQ(after.size(), before.size() + 1)
        << "a host-added track must produce its own m-line";
    engine.close();
}

TEST_F(EngineSession, TheBaseMidsDoNotMoveWhenATrackIsAdded) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer1 = engine.create_offer();
    ASSERT_FALSE(offer1.empty());
    const std::string mid_before = mid_of(offer1, 0);
    ASSERT_FALSE(mid_before.empty());

    ASSERT_TRUE(engine.add_track("mic2", MediaKind::kAudio, Direction::kSendRecv));
    const std::string offer2 = engine.create_offer();
    ASSERT_FALSE(offer2.empty());

    EXPECT_EQ(mid_of(offer2, 0), mid_before)
        << "the mid of an existing m-line must never move";
    engine.close();
}

TEST_F(EngineSession, TheBundleGroupListsExactlyTheEmittedMids) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_track("mic2", MediaKind::kAudio, Direction::kSendRecv));
    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    const auto mids = bundle_mids(offer);
    const auto lines = media_lines(offer);
    ASSERT_EQ(mids.size(), lines.size());

    // Every mid in the BUNDLE group is a mid that is in the
    // description, and no mid appears twice.
    std::vector<std::string> seen;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string mid = mid_of(offer, i);
        ASSERT_FALSE(mid.empty());
        EXPECT_NE(std::find(mids.begin(), mids.end(), mid), mids.end())
            << "mid " << mid << " is in the description but not in BUNDLE";
        seen.push_back(mid);
    }
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(std::unique(seen.begin(), seen.end()), seen.end())
        << "a mid must not appear twice";
    engine.close();
}

// -----------------------------------------------------------------------------
// N4 — the direction mapping
// -----------------------------------------------------------------------------
TEST_F(EngineSession, ATrackDirectionLandsOnTheMLine) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string mid = engine.add_transceiver(MediaKind::kAudio,
                                                   Direction::kSendRecv);
    ASSERT_FALSE(mid.empty());

    ASSERT_TRUE(engine.set_transceiver_direction(mid, Direction::kSendOnly));
    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    const auto lines = media_lines(offer);
    ASSERT_FALSE(lines.empty());
    // Find the m-line that carries our mid.
    std::size_t index = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (mid_of(offer, i) == mid) { index = i; break; }
    }
    ASSERT_LT(index, lines.size());
    EXPECT_EQ(direction_of(offer, index), "sendonly");
    engine.close();
}

TEST_F(EngineSession, RemovingATrackTurnsItsMLineInactive) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string offer1 = engine.create_offer();
    ASSERT_FALSE(offer1.empty());
    const auto before = media_lines(offer1);
    ASSERT_FALSE(before.empty());
    const std::string mid_before = mid_of(offer1, 0);
    ASSERT_FALSE(mid_before.empty());

    // The base audio track is the one riding on that m-line.
    ASSERT_TRUE(engine.remove_track("nimrtc-audio"));
    const std::string offer2 = engine.create_offer();
    ASSERT_FALSE(offer2.empty());

    const auto after = media_lines(offer2);
    EXPECT_EQ(after.size(), before.size())
        << "OD-A: a removed track keeps its m-line";

    std::size_t index = after.size();
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (mid_of(offer2, i) == mid_before) { index = i; break; }
    }
    ASSERT_LT(index, after.size());
    EXPECT_EQ(direction_of(offer2, index), "inactive");
    EXPECT_EQ(mid_of(offer2, index), mid_before)
        << "OD-A: the mid is not recycled";
    engine.close();
}

TEST_F(EngineSession, AStoppedTransceiverKeepsItsMidAndGoesInactive) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string mid = engine.add_transceiver(MediaKind::kAudio,
                                                   Direction::kSendRecv);
    ASSERT_FALSE(mid.empty());

    ASSERT_TRUE(engine.stop_transceiver(mid));
    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty());

    const auto lines = media_lines(offer);
    std::size_t index = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (mid_of(offer, i) == mid) { index = i; break; }
    }
    ASSERT_LT(index, lines.size());
    EXPECT_EQ(direction_of(offer, index), "inactive");
    EXPECT_EQ(mid_of(offer, index), mid);
    engine.close();
}

TEST_F(EngineSession, ARestartedTransceiverComesBack) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string mid = engine.add_transceiver(MediaKind::kAudio,
                                                   Direction::kSendOnly);
    ASSERT_FALSE(mid.empty());
    ASSERT_TRUE(engine.stop_transceiver(mid));
    ASSERT_TRUE(engine.restart_transceiver(mid));

    const auto tr = engine.transceivers();
    bool found = false;
    for (const auto& t : tr) {
        if (t.mid != mid) continue;
        found = true;
        EXPECT_FALSE(t.stopped);
        EXPECT_EQ(t.direction, Direction::kSendOnly);
    }
    EXPECT_TRUE(found);
    engine.close();
}

// -----------------------------------------------------------------------------
// T2 / T3 — a multi-m-line offer is answered one-to-one
// -----------------------------------------------------------------------------
TEST_F(EngineSession, AMultiMLineOfferIsAnsweredOneToOne) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // A peer offer with two m-lines: audio (mid 0) and video (mid 1).
    const std::string peer_offer =
        "v=0\r\n"
        "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:1\r\n"
        "a=sendrecv\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:102 H264/90000\r\n";

    const auto answer = engine.process_remote_sdp(peer_offer);
    ASSERT_TRUE(answer.has_value());
    ASSERT_FALSE(answer->empty());

    const auto lines = media_lines(*answer);
    ASSERT_EQ(lines.size(), 2u) << "the answer mirrors the offer's m-sections";
    EXPECT_EQ(mid_of(*answer, 0), "0");
    EXPECT_EQ(mid_of(*answer, 1), "1");
    engine.close();
}

TEST_F(EngineSession, ThePeersMidsBecomeTransceivers) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::string peer_offer =
        "v=0\r\n"
        "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtcp-mux\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";

    ASSERT_TRUE(engine.process_remote_sdp(peer_offer).has_value());

    const auto tr = engine.transceivers();
    ASSERT_FALSE(tr.empty());
    EXPECT_EQ(tr[0].mid, "0");
    engine.close();
}

} // namespace

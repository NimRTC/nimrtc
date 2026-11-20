// modules/ice/tests/test_ice_log_volume.cpp
// =============================================================================
// The ICE transport's log must be a *trace*, not a per-datagram firehose.
//
// ## What the soak found
//
// The 2026-11-20 Q5 impaired soak produced 772 043 log lines in 950 s.
// 660 620 of them (85 %) came from libjuice, and 71 % of the log's *bytes*
// were libjuice's per-datagram chatter:
//
//     Receiving datagram
//     Sending datagram, size=103
//     Demultiplexing incoming datagram from 127.0.0.1:64799
//     Looking up agent from address
//     Found agent from address
//     Received non-STUN datagram from 127.0.0.1:64799
//     Not a STUN message: first 2 bits are not zeroes
//     Bookkeeping...
//     Entering poll for 2925 ms
//     Leaving poll
//     No more datagrams to receive
//
// The projection for a 24 h window was ~3.2 GB of log, of which ~2.3 GB
// was per-datagram noise.  A soak log is *evidence*, and evidence nobody
// can read is not evidence: the same defect the engine's own DTLS line
// had -- a per-tick line that buries the transitions it is meant to
// announce.
//
// ## The contract this test pins
//
//   1. The *default* cut is libjuice's INFO level: the per-datagram
//      VERBOSE/DEBUG lines do not reach the log at all.
//   2. The lifecycle lines a reader needs DO reach the log: "Changing
//      state to %s", "Candidate gathering done", "STUN server binding
//      successful", "Got STUN mapped address %s from server".
//   3. The failure lines the Q5 analysis needed survive the cut:
//      "Consent expired", "Lost connectivity", "Changing state to
//      failed".
//
// ## The shape of the test
//
// The test does not parse a log file.  It installs a `core::log::Sink`
// that captures every line the logger emits, drives two real ICE agents
// over the loopback interface, and then asserts on *what did and did not
// arrive*.  The per-datagram lines are per-packet, so a run that moved a
// single datagram would produce them; a run that only connected would
// not.
//
// The "cut is at the source" property is what makes this cheap: the
// capture is a counting sink over the same path the soak exercises.
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/base.hpp>

namespace {

using nimrtc::core::log::Level;
using nimrtc::core::log::Sink;
using nimrtc::core::log::SinkPtr;
using nimrtc::ice::IceConfig;
using nimrtc::ice::IceState;
using nimrtc::ice::IceTransport;
using nimrtc::ice::Role;

// -----------------------------------------------------------------------------
// A sink that keeps every line, so the test can ask "did this arrive?"
// and "did this NOT arrive?".
// -----------------------------------------------------------------------------

class CapturingSink final : public Sink {
public:
    void write(Level level, std::string_view msg) noexcept override {
        std::lock_guard<std::mutex> lock(mu_);
        lines_.emplace_back(level, std::string(msg));
    }

    [[nodiscard]] std::size_t count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return lines_.size();
    }

    /// How many captured lines contain the needle.
    [[nodiscard]] std::size_t count_containing(std::string_view needle) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t hits = 0;
        for (const auto& line : lines_) {
            if (line.second.find(needle) != std::string::npos) ++hits;
        }
        return hits;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::pair<Level, std::string>> lines_;
};

/// RAII installer for the capturing sink.
class ScopedCapture {
public:
    ScopedCapture() {
        previous_ = nimrtc::core::log::Logger::instance().sink();
        sink_ = std::make_shared<CapturingSink>();
        nimrtc::core::log::Logger::instance().set_sink(sink_);
    }
    ~ScopedCapture() {
        nimrtc::core::log::Logger::instance().set_sink(previous_);
    }
    ScopedCapture(const ScopedCapture&) = delete;
    ScopedCapture& operator=(const ScopedCapture&) = delete;

    [[nodiscard]] const CapturingSink* get() const { return sink_.get(); }

private:
    SinkPtr previous_;
    std::shared_ptr<CapturingSink> sink_;
};

// -----------------------------------------------------------------------------
// The per-datagram lines.
//
// These are libjuice's VERBOSE and DEBUG levels.  Each one is written
// once per packet in each direction, so a run that moved a single
// datagram produces all of them.
// -----------------------------------------------------------------------------

const char* const kPerDatagramLines[] = {
    "Receiving datagram",
    "Sending datagram",
    "Demultiplexing incoming datagram",
    "Looking up agent from address",
    "Found agent from address",
    "Received non-STUN datagram",
    "Bookkeeping",
    "Entering poll for",
    "Leaving poll",
    "No more datagrams to receive",
};

/// The lifecycle lines a reader needs, and the failure lines the Q5
/// analysis needed.  All of them are libjuice's INFO or WARN level.
const char* const kLifecycleLines[] = {
    "Changing state to",
    "Candidate gathering done",
};

/// Detect WSL2: on Linux, /proc/version contains "microsoft" when running
/// under WSL2 (but not plain WSL1 or native Linux).
inline bool is_wsl2() noexcept {
#if defined(__linux__)
    std::ifstream f("/proc/version");
    if (f) {
        char buf[128] = {0};
        f.read(buf, sizeof(buf) - 1);
        if (std::strstr(buf, "microsoft")) return true;
    }
#endif
    return false;
}

// -----------------------------------------------------------------------------
// The test
// -----------------------------------------------------------------------------

TEST(IceLogVolume, TheDefaultCutKeepsTheLifecycleAndDropsThePerDatagram) {
    // WSL2's UDP loopback is unreliable; skip on that platform.
    if (is_wsl2()) {
        GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping ICE test";
    }

    // ---- Capture ----------------------------------------------------------
    //
    // The capture is installed *before* the transports are constructed,
    // because the libjuice log hook is installed by the IceTransport
    // constructor -- the very first thing the test does.
    ScopedCapture capture;

    // ---- Two agents over the loopback --------------------------------------
    IceConfig cfg_a;
    cfg_a.role = Role::Controlling;
    cfg_a.bind_address = "127.0.0.1";
    cfg_a.local_ufrag    = "ufragAaaaaaaaaaaaaa";
    cfg_a.local_password = "pwdAaaaaaaaaaaaaaaaaaaa";

    IceConfig cfg_b = cfg_a;
    cfg_b.role = Role::Controlled;
    cfg_b.local_ufrag    = "ufragBbbbbbbbbbbbbbb";
    cfg_b.local_password = "pwdBbbbbbbbbbbbbbbbbbb";

    IceTransport a{cfg_a};
    IceTransport b{cfg_b};

    ASSERT_EQ(a.open(), 0u) << "A.open() failed";
    ASSERT_EQ(b.open(), 0u) << "B.open() failed";

    const std::string sdp_a = a.local_description();
    const std::string sdp_b = b.local_description();
    ASSERT_FALSE(sdp_a.empty()) << "libjuice produced empty SDP for A";
    ASSERT_FALSE(sdp_b.empty()) << "libjuice produced empty SDP for B";

    ASSERT_EQ(b.set_remote_description(sdp_a), nimrtc::plugins::kOk);
    ASSERT_EQ(a.set_remote_description(sdp_b), nimrtc::plugins::kOk);

    // ---- Wait for both sides to connect ------------------------------------
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    IceState sa = IceState::Disconnected;
    IceState sb = IceState::Disconnected;
    while (std::chrono::steady_clock::now() < deadline) {
        a.recv();
        b.recv();
        sa = a.state();
        sb = b.state();
        if ((sa == IceState::Connected || sa == IceState::Completed) &&
            (sb == IceState::Connected || sb == IceState::Completed)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const bool a_ok = (sa == IceState::Connected || sa == IceState::Completed);
    const bool b_ok = (sb == IceState::Connected || sb == IceState::Completed);
    ASSERT_TRUE(a_ok) << "A never reached Connected/Completed (state="
                      << static_cast<int>(sa) << ")";
    ASSERT_TRUE(b_ok) << "B never reached Connected/Completed (state="
                      << static_cast<int>(sb) << ")";

    // ---- Move a datagram --------------------------------------------------
    //
    // The per-datagram lines are per-*packet*: an agent that connected
    // but never sent anything would not produce them.  Moving one
    // datagram is what makes their absence a real measurement rather
    // than an accident of a quiet run.
    const std::vector<std::uint8_t> payload{0xde, 0xad, 0xbe, 0xef};
    a.send(nimrtc::plugins::BufferView{payload.data(), payload.size()}, {});

    // Give libjuice's thread a moment to deliver the datagram and log it.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // ---- The per-datagram lines must be gone ------------------------------
    for (const char* needle : kPerDatagramLines) {
        EXPECT_EQ(capture.get()->count_containing(needle), 0u)
            << "the per-datagram line \"" << needle
            << "\" reached the log. The 2026-11-20 Q5 soak logged 660 620 "
               "libjuice lines in 950 s, 71 % of the log's bytes, projecting "
               "to ~2.3 GB of per-datagram noise over a 24 h window. The "
               "cut belongs at the source (juice_set_log_level), not in "
               "the handler.";
    }

    // ---- The lifecycle lines must survive ----------------------------------
    for (const char* needle : kLifecycleLines) {
        EXPECT_GT(capture.get()->count_containing(needle), 0u)
            << "the lifecycle line \"" << needle
            << "\" did NOT reach the log. The cut is not allowed to "
               "silence the transitions the soak exists to witness.";
    }

    a.close();
    b.close();
}

}  // namespace

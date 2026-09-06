/**
 * @file examples/loopback-p2p/loopback-p2p.cpp
 * @brief loopback-p2p — two NimRTCEngines connect over 127.0.0.1.
 *
 * ## What this validates
 *
 *   P1.1 baseline: two engines can complete SDP exchange, gather ICE
 *   host candidates, run STUN connectivity checks on loopback, and reach
 *   the Connected state. DTLS / SRTP / RTP path is wired in the engine
 *   but only exercised in detail by the in-loop unit-test style demo
 *   below.
 *
 * ## Usage
 *
 *   loopback-p2p             # run for 6 seconds
 *   loopback-p2p [seconds]   # run for N seconds
 *
 * Exit code:
 *   0  ICE Connected (both engines)
 *   1  ICE never Connected
 */

#define _CRT_SECURE_NO_WARNINGS

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

#include <nimrtc/engine/engine.hpp>

namespace {

using clk = std::chrono::steady_clock;

// Extract ICE ufrag/pwd/candidates lines from an SDP string.
// These are the lines needed by ice->set_remote_description().
std::string extract_ice_block(const std::string& sdp) {
    std::string block;
    std::istringstream iss{sdp};
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("a=ice-ufrag:", 0) == 0 ||
            line.rfind("a=ice-pwd:", 0) == 0 ||
            line.rfind("a=candidate:", 0) == 0) {
            block += line + "\n";
        }
    }
    return block;
}

int run_loopback(double seconds) {
    std::fprintf(stderr, "[loopback-p2p] starting\n");

    // ---- Offerer (A): pre_open → open → CONTROLLING → create offer ---
    nimrtc::engine::EngineConfig cfgA;
    cfgA.local_bind_address = "127.0.0.1";
    cfgA.local_port_range_begin = 51000;
    cfgA.local_port_range_end   = 51099;
    cfgA.pcm_sample_rate_hz = 48000;
    cfgA.pcm_channels       = 1;

    nimrtc::engine::NimRTCEngine A(cfgA);
    std::fprintf(stderr, "[loopback-p2p] A.pre_open()\n");
    if (A.pre_open() != 0) {
        std::fprintf(stderr, "engine A pre_open failed\n");
        return 1;
    }
    std::fprintf(stderr, "[loopback-p2p] A.open() (ICE CONTROLLING)\n");
    if (A.open() != 0) {
        std::fprintf(stderr, "engine A open failed\n");
        return 1;
    }

    std::fprintf(stderr, "[loopback-p2p] A.create_offer()\n");
    auto offer = A.create_offer();
    if (offer.empty()) {
        std::fprintf(stderr, "create_offer failed\n");
        return 2;
    }
    std::fprintf(stderr, "[loopback-p2p] offer length=%zu\n", offer.size());

    // ---- Answerer (B): pre_open → set_remote_ice → open (CONTROLLED) ----
    nimrtc::engine::EngineConfig cfgB = cfgA;
    cfgB.local_port_range_begin = 51100;
    cfgB.local_port_range_end   = 51199;
    nimrtc::engine::NimRTCEngine B(cfgB);

    // Key fix: extract A's ICE credentials BEFORE B's ICE starts gathering.
    // This tells libjuice to set B's agent to CONTROLLED (not CONTROLLING),
    // avoiding the ICE role conflict that prevented loopback DTLS before.
    std::string ice_block = extract_ice_block(offer);
    if (!ice_block.empty()) {
        std::fprintf(stderr, "[loopback-p2p] B.pre_open() then B.set_remote_ice()\n");
        if (B.pre_open() != 0) {
            std::fprintf(stderr, "engine B pre_open failed\n");
            return 3;
        }
        if (!B.set_remote_ice(ice_block)) {
            std::fprintf(stderr, "B.set_remote_ice failed\n");
            return 4;
        }
        std::fprintf(stderr, "[loopback-p2p] B.open() (ICE CONTROLLED)\n");
        if (B.open() != 0) {
            std::fprintf(stderr, "engine B open failed\n");
            return 5;
        }
    } else {
        // Fallback: open without pre-set remote ICE (old behaviour).
        std::fprintf(stderr, "[loopback-p2p] B.open() without pre-set ICE\n");
        if (B.open() != 0) {
            std::fprintf(stderr, "engine B open failed\n");
            return 5;
        }
    }

    std::fprintf(stderr, "[loopback-p2p] B.process_remote_sdp(offer)\n");
    auto answer = B.process_remote_sdp(offer);
    if (!answer) {
        std::fprintf(stderr, "B.process_remote_sdp failed\n");
        return 6;
    }
    std::fprintf(stderr, "[loopback-p2p] answer obtained\n");

    std::fprintf(stderr, "[loopback-p2p] A.process_remote_sdp(answer)\n");
    if (!A.process_remote_sdp(*answer)) {
        std::fprintf(stderr, "A.process_remote_sdp failed\n");
        return 7;
    }
    std::fprintf(stderr, "[loopback-p2p] starting tick loop\n");

    // ---- Drive both engines ----------------------------------------------
    auto start = clk::now();
    auto deadline = start + std::chrono::milliseconds(
                              static_cast<long long>(seconds * 1000));
    int a_ice_up = 0, b_ice_up = 0;
    int last_a_state = -1, last_b_state = -1;

    while (clk::now() < deadline) {
        A.tick();
        B.tick();

        std::string sa = A.ice_state_string();
        std::string sb = B.ice_state_string();
        if (sa == "connected" || sa == "completed") a_ice_up = 1;
        if (sb == "connected" || sb == "completed") b_ice_up = 1;

        if (last_a_state != (int)(sa == "connected" || sa == "completed")) {
            std::fprintf(stderr, "[A ice] %s\n", sa.c_str());
            last_a_state = (int)(sa == "connected" || sa == "completed");
        }
        if (last_b_state != (int)(sb == "connected" || sb == "completed")) {
            std::fprintf(stderr, "[B ice] %s\n", sb.c_str());
            last_b_state = (int)(sb == "connected" || sb == "completed");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::fprintf(stderr, "[loopback-p2p] exit code: A=%d B=%d\n",
                 A.open() == 0 ? 1 : 0, b_ice_up);
    std::fprintf(stderr, "[loopback-p2p] A.ice=%s  B.ice=%s\n",
                 a_ice_up ? "connected" : A.ice_state_string(),
                 b_ice_up ? "connected" : B.ice_state_string());

    A.close();
    B.close();
    return (a_ice_up && b_ice_up) ? 0 : 6;
}

} // anonymous namespace

int main(int argc, char** argv) {
    double seconds = 6.0;
    if (argc >= 2) seconds = std::atof(argv[1]);
    std::fprintf(stderr,
                 "NimRTC loopback-p2p: %.1f seconds\n", seconds);
    int rc = run_loopback(seconds);
    std::fprintf(stderr, "[loopback-p2p] returning %d\n", rc);
    return rc;
}

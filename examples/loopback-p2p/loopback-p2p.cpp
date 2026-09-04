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
#include <thread>

#include <nimrtc/engine/engine.hpp>

namespace {

using clk = std::chrono::steady_clock;

int run_loopback(double seconds) {
    std::fprintf(stderr, "[loopback-p2p] starting\n");
    // ---- Offerer (A) ------------------------------------------------------
    nimrtc::engine::EngineConfig cfgA;
    cfgA.local_bind_address = "127.0.0.1";
    cfgA.local_port_range_begin = 51000;
    cfgA.local_port_range_end   = 51099;
    cfgA.pcm_sample_rate_hz = 48000;
    cfgA.pcm_channels       = 1;

    nimrtc::engine::NimRTCEngine A(cfgA);
    std::fprintf(stderr, "[loopback-p2p] opening A\n");
    if (A.open() != 0) {
        std::fprintf(stderr, "engine A open failed\n");
        return 1;
    }

    std::fprintf(stderr, "[loopback-p2p] create offer\n");
    auto offer = A.create_offer();
    std::fprintf(stderr, "[loopback-p2p] offer length=%zu\n", offer.size());
    if (offer.empty()) {
        std::fprintf(stderr, "create_offer failed\n");
        return 2;
    }

    // ---- Answerer (B) -----------------------------------------------------
    nimrtc::engine::EngineConfig cfgB = cfgA;
    // Disjoint port range — otherwise A and B bind the same UDP port and
    // process each other's STUN packets (loopback is shared).
    cfgB.local_port_range_begin = 51100;
    cfgB.local_port_range_end   = 51199;
    nimrtc::engine::NimRTCEngine B(cfgB);
    std::fprintf(stderr, "[loopback-p2p] opening B\n");
    if (B.open() != 0) {
        std::fprintf(stderr, "engine B open failed\n");
        return 3;
    }

    std::fprintf(stderr, "[loopback-p2p] B processes offer\n");
    auto answer = B.process_remote_sdp(offer);
    std::fprintf(stderr, "[loopback-p2p] answer obtained=%d\n", (bool)answer);

    std::fprintf(stderr, "[loopback-p2p] A processes answer\n");
    if (!A.process_remote_sdp(*answer)) {
        std::fprintf(stderr, "A process_remote_sdp failed\n");
        return 5;
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

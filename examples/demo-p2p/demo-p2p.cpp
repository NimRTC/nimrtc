/**
 * @file examples/demo-p2p/demo-p2p.cpp
 * @brief demo-p2p — minimal NimRTC end-to-end demo.
 *
 * Demonstrates the P1 baseline:
 *   - Engine::open()    → ICE agent starts gathering
 *   - Engine::create_offer()  → RFC 8829-compliant Opus offer
 *   - Engine::process_remote_sdp()  → ICE credentials applied
 *   - Engine::tick()    → drain transport queue, dispatch RTP
 *   - Engine::send_audio() → 3A pass-through
 *
 * ## Usage
 *
 *   demo-p2p                      # run as offerer; prints offer to stdout
 *   demo-p2p offer <file>         # load offer from file, print as answerer
 *   demo-p2p answer <file>        # load answer from file (offerer side)
 *
 * P1 scope: this is a CLI scaffolding to validate the SDP/ICE pipeline.
 * Chrome-interop end-to-end (signalling server + audio capture) lands in
 * a follow-up; for P1, integration testing happens in interop/.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstring>
#include <string>

#include <nimrtc/engine/engine.hpp>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s                  Create and print an SDP offer (offerer)\n"
                 "  %s offer <file>     Read SDP offer from <file>, print answer\n"
                 "  %s answer <file>    Read SDP answer from <file>, apply ICE\n",
                 prog, prog, prog);
}

std::string slurp_file(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "demo-p2p: cannot open '%s'\n", path);
        return {};
    }
    std::string s;
    char buf[4096];
    while (auto n = std::fread(buf, 1, sizeof(buf), f)) {
        s.append(buf, n);
    }
    std::fclose(f);
    return s;
}

int run_offerer(nimrtc::engine::NimRTCEngine& engine) {
    auto sdp = engine.create_offer();
    if (sdp.empty()) {
        std::fprintf(stderr, "demo-p2p: create_offer failed\n");
        return 2;
    }
    std::fwrite(sdp.data(), 1, sdp.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

int run_answerer(nimrtc::engine::NimRTCEngine& engine, const char* offer_path) {
    auto offer = slurp_file(offer_path);
    if (offer.empty()) return 2;

    auto answer = engine.process_remote_sdp(offer);
    if (!answer) {
        std::fprintf(stderr, "demo-p2p: process_remote_sdp failed\n");
        return 3;
    }
    std::fwrite(answer->data(), 1, answer->size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

int run_apply_answer(nimrtc::engine::NimRTCEngine& engine, const char* path) {
    auto answer = slurp_file(path);
    if (answer.empty()) return 2;
    auto result = engine.process_remote_sdp(answer);
    if (!result) {
        std::fprintf(stderr, "demo-p2p: process_remote_sdp failed\n");
        return 3;
    }
    std::fprintf(stdout, "demo-p2p: applied answer (len=%zu)\n", result->size());
    return 0;
}

} // anonymous namespace

int main(int argc, char** argv) {
    std::fprintf(stderr, "NimRTC demo-p2p (P1)\n");
    if (argc > 1 && (std::strcmp(argv[1], "-h") == 0 ||
                     std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    nimrtc::engine::EngineConfig cfg;
    cfg.local_bind_address = "0.0.0.0";
    cfg.stun_server_host   = "stun.l.google.com";
    cfg.stun_server_port   = 19302;
    cfg.pcm_sample_rate_hz = 48000;
    cfg.pcm_channels       = 1;

    nimrtc::engine::NimRTCEngine engine(cfg);
    if (engine.open() != 0) {
        std::fprintf(stderr, "demo-p2p: engine open failed\n");
        return 1;
    }

    engine.set_on_state_change([](const char* s) {
        std::fprintf(stderr, "[state] %s\n", s);
    });
    engine.set_on_error([](uint32_t e, std::string_view m) {
        std::fprintf(stderr, "[error 0x%04X] %.*s\n", e,
                     static_cast<int>(m.size()), m.data());
    });

    int rc = 0;
    if (argc >= 3 && std::strcmp(argv[1], "offer") == 0) {
        rc = run_answerer(engine, argv[2]);
    } else if (argc >= 3 && std::strcmp(argv[1], "answer") == 0) {
        rc = run_apply_answer(engine, argv[2]);
    } else if (argc == 1) {
        rc = run_offerer(engine);
    } else {
        print_usage(argv[0]);
        rc = 64;  // EX_USAGE
    }

    engine.close();
    return rc;
}

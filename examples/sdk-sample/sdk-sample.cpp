// sdk-sample.cpp -- the ≤200-line SDK sample: one audio+video+control call
// between two processes, with everything but signalling left to the engine.
//
// Usage: sdk-sample --role caller|callee [--turn-host H --turn-port P
//                                          --turn-user U --turn-pass K]
// The host writes only the four hooks the slice names (call-state, control,
// ICE-recovery, TURN credentials) plus the stdin/stdout signalling client.
// Device, codec, BWE, RTX, recovery and render all live in the engine.  Each
// process runs 10 s, then prints one `summary` line the acceptance test
// asserts on; exit 0 means the control exchange completed.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/engine/engine.hpp>
#include "sdk_signalling.hpp"

namespace {

struct Turn {
    const char* host = nullptr;
    std::uint16_t port = 3478;
    std::string user;
    std::string pass;
};

// N7 provider: pulled at open() and after every N3 restart.  The relay is
// TLS-only, so the transport names Tls.
bool provide_turn(void* ctx, nimrtc::engine::TurnCredentials* out) noexcept {
    auto* t = static_cast<Turn*>(ctx);
    if (!t->host || !*t->host) return false;
    out->host = t->host;
    out->port = t->port;
    out->username = t->user;
    out->password = t->pass;
    out->transport = nimrtc::plugins::TurnTransport::Tls;
    return true;
}

struct App {
    nimrtc::engine::NimRTCEngine* engine;
    nimrtc::plugins::IICETransport* ice;
    bool callee, ice_up = false, ping_done = false;
    std::uint64_t control_rx = 0, audio_tx = 0, video_tx = 0, video_rx = 0;
    double phase = 0.0;
    std::int64_t next_audio_ns = 0, next_video_ns = 0;
    int rc = 1;
};

// Drives tick(), the audio sine and the video source until 10 s elapses,
// then prints the summary the acceptance test asserts on.
void tick_loop(App& a, int sec) {
    using namespace std::chrono;
    const auto end = steady_clock::now() + seconds(sec);
    std::vector<float> pcm(960);                     // 20 ms @ 48 kHz
    while (steady_clock::now() < end) {
        a.engine->tick();
        const std::string st = a.engine->ice_state_string();
        if (st == "connected" || st == "completed") {
            if (!a.ice_up) { a.ice_up = true;
                if (a.callee) {
                    a.engine->send_control_message({reinterpret_cast<const std::uint8_t*>("ping"), 4});
                    a.ping_done = true;
                }
            }
            const auto now_ns = steady_clock::now().time_since_epoch().count();
            if (now_ns >= a.next_audio_ns) {
                for (float& s : pcm) { s = 0.2f * static_cast<float>(std::sin(a.phase)); a.phase += 0.0575955f; }
                a.engine->send_audio(pcm.data(), pcm.size());
                ++a.audio_tx;
                a.next_audio_ns = now_ns + 20'000'000;
            }
            if (now_ns >= a.next_video_ns) {
                if (auto* src = a.engine->video_source()) { src->produce_one(); ++a.video_tx; }
                a.next_video_ns = now_ns + 66'666'667;  // ~15 fps
            }
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    a.rc = (a.control_rx > 0 && a.audio_tx > 0 && a.video_tx > 0) ? 0 : 1;
    std::fprintf(stderr, "[sdk-sample] summary au_tx=%llu au_rx=%llu v_tx=%llu v_rx=%llu c_rx=%llu rc=%d\n",
                 (unsigned long long)a.audio_tx, (unsigned long long)a.engine->stats().audio_frames_received,
                 (unsigned long long)a.video_tx, (unsigned long long)a.video_rx,
                 (unsigned long long)a.control_rx, a.rc);
}

}  // namespace

int main(int argc, char** argv) {
    Turn turn;
    bool callee = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (i + 1 < argc && std::strncmp(a, "--turn-", 7) == 0) {
            if (std::strcmp(a + 7, "host") == 0) turn.host = argv[++i];
            else if (std::strcmp(a + 7, "port") == 0) turn.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
            else if (std::strcmp(a + 7, "user") == 0) turn.user = argv[++i];
            else if (std::strcmp(a + 7, "pass") == 0) turn.pass = argv[++i];
        } else if (std::strcmp(a, "--role") == 0 && i + 1 < argc) {
            callee = std::strcmp(argv[++i], "callee") == 0;
        }
    }

    nimrtc::engine::EngineConfig cfg;
    cfg.local_bind_address = "127.0.0.1";
    cfg.connectivity_profile = "auto";           // D6: engine owns connectivity
    cfg.ice_recovery_policy = "auto";            // N3: engine owns reconnection
    cfg.control_channel_name = "control-channel";
    cfg.turn_credentials = &provide_turn;        // N7
    cfg.turn_credentials_ctx = &turn;
    cfg.video_source_name = "memory";
    cfg.video_sink_name = "headless";
    cfg.video_codec_name = "h264";
    cfg.video_sender_tuning.ssrc = 0xCAFEBABEu;
    cfg.video_sender_tuning.payload_type = 102;
    cfg.video_receiver_tuning.ssrc = cfg.video_sender_tuning.ssrc;
    cfg.video_receiver_tuning.payload_type = cfg.video_sender_tuning.payload_type;

    nimrtc::engine::NimRTCEngine engine(cfg);
    App app{&engine, nullptr, callee};

    engine.set_on_call_state([](const nimrtc::engine::NimRTCEngine::CallStateReport& r) {
        std::fprintf(stderr, "[sdk-sample] state=%d %s\n", (int)r.state, r.failure_reason.c_str());
    });
    engine.set_on_ice_recovery([](const nimrtc::engine::NimRTCEngine::IceRecoveryReport& r) {
        std::fprintf(stderr, "[sdk-sample] recovery=%u %s\n", r.restart_count, r.reason.c_str());
    });
    engine.set_on_control_message([&](std::span<const std::uint8_t> m) {
        ++app.control_rx;
        if (std::string_view(reinterpret_cast<const char*>(m.data()), m.size()) == "ping" && !app.ping_done) {
            engine.send_control_message({reinterpret_cast<const std::uint8_t*>("pong"), 4});
            app.ping_done = true;
        }
    });
    engine.set_on_video_frame([&](const std::uint8_t*, std::size_t, bool) { ++app.video_rx; });

    if (engine.pre_open() != 0 || engine.open() != 0 || !(app.ice = engine.get_ice_transport())) {
        std::fprintf(stderr, "[sdk-sample] open failed\n");
        return 2;
    }
    if (auto* src = engine.video_source())
        src->set_callback([&](const nimrtc::plugins::VideoSourceFrame& f) { engine.send_video(f); });

    auto emit_gathered = [&] {
        app.ice->wait_for_gathering(3000);
        for (const auto& c : app.ice->gathered_local_candidates())
            sdk::emit(stdout, "candidate", "candidate", c);
    };
    std::thread ticker(tick_loop, std::ref(app), 10);

    if (!callee) {
        const std::string offer = engine.create_offer();
        if (offer.empty()) return 2;
        sdk::emit(stdout, "offer", "sdp", offer);
        emit_gathered();
    }

    std::string line;
    while (sdk::read_line(stdin, line)) {
        std::string v;
        if (sdk::get_string(line, "candidate", v)) {
            if (v.rfind("a=", 0) != 0) v = "a=" + v;
            app.ice->add_remote_candidate(v);
        } else if (sdk::get_string(line, "sdp", v)) {
            if (callee) {
                const auto answer = engine.process_remote_sdp(v);
                if (!answer) break;
                sdk::emit(stdout, "answer", "sdp", *answer);
                emit_gathered();
            } else engine.process_remote_sdp(v);
        }
    }

    ticker.join();
    return app.rc;
}

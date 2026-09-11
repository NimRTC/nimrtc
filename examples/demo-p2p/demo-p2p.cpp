/**
 * @file examples/demo-p2p/demo-p2p.cpp
 * @brief demo-p2p — minimal NimRTC end-to-end demo.
 *
 * ## Usage
 *
 *   demo-p2p                       # run as offerer; prints SDP offer to stdout, exits
 *   demo-p2p offer <file>          # read SDP offer from <file>, print answer to stdout
 *   demo-p2p answer <file>         # read SDP answer from <file>, apply ICE (offerer side)
 *   demo-p2p --signaling-proxy [--answerer]
 *                                 # stay alive; exchange JSON messages on stdin/stdout
 *                                 # (bridged to a WebSocket signaling server by the
 *                                 # Python helper `interop/signaling/signaling_proxy.py`)
 *
 * ## --signaling-proxy protocol
 *
 *   The process reads newline-delimited JSON messages from stdin and writes
 *   newline-delimited JSON messages to stdout.  Each message uses the same
 *   shape as the WebSocket payload:
 *
 *     {"type":"offer",   "sdp":"..."}
 *     {"type":"answer",  "sdp":"..."}
 *     {"type":"candidate","candidate":"..."}
 *
 *   Outgoing ICE candidates are pushed to stdout as soon as ICE has them.
 *   The engine tick loop drives state changes.
 *
 *   The Python proxy (signaling_proxy.py) connects this side to the
 *   WebSocket signaling server.
 */

/* _CRT_SECURE_NO_WARNINGS is now provided globally via
 * cmake/NimRTCOptions.cmake → target_compile_definitions.
 * Defining it here would trigger C4005 ("macro redefinition")
 * under MSVC. */

#ifdef _WIN32
    #include <windows.h>
    #define poll(fd, nfds, timeout)  (0)  // not used on Windows path
#else
    #include <poll.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/engine/engine.hpp>
#include <nimrtc/core/log.hpp>

// We deliberately include the ICE-aware *plugin* interface here, not the
// concrete `<nimrtc/ice/ice.hpp>` module header. The proxy used to do
// `dynamic_cast<ice::IceTransport*>(engine.get_transport())` to reach
// `gathered_local_candidates()` and `add_remote_candidate()`; that cast
// crossed the plugin seam and leaked the concrete class into the demo.
// Now we use `engine.get_ice_transport()` which returns the same
// `plugins::IICETransport*` the engine itself holds, so the demo stays
// inside the plugin abstraction.
#include <nimrtc/plugins/ice_transport.hpp>

namespace {

void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s                        Create and print an SDP offer (offerer, file mode)\n"
                 "  %s offer <file>           Read SDP offer from <file>, print answer\n"
                 "  %s answer <file>          Read SDP answer from <file>, apply ICE\n"
                 "  %s --signaling-proxy [--answerer] [--duration <sec>]\n"
                 "                           JSON message loop on stdin/stdout for interop\n"
                 "  %s --codec <name>         Select audio codec: opus (default) or pcmu\n"
                 "                           opus requires NIMRTC_HAS_OPUS; pcmu is\n"
                 "                           always available (WebRTC mandatory fallback)\n"
                 "  %s --bind <ip>            Local ICE bind address (default: 127.0.0.1;\n"
                 "                           use 0.0.0.0 to advertise all host interfaces)\n"
                 "  %s --stun-host <h>        STUN server hostname (default: stun.l.google.com)\n"
                 "  %s --stun-port <p>        STUN server port     (default: 19302)\n"
                 "  %s --no-stun              Disable STUN candidate gathering\n"
                 "  %s --video                Enable synthetic H.264 video stream\n"
                 "                           (640x480 @ 15 fps, stub encoder; off by default)\n",
                 prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

std::string slurp_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "demo-p2p: cannot open '%s'\n", path);
        return {};
    }
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// Trim trailing \r and \n.
void rtrim(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
}

// Minimal JSON string escape (only what SDP / ICE candidates need).
std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\r': out += "\\r"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Unescape a JSON string value body.  Handles the escapes that can appear in
// a well-formed JSON wire stream from Chrome's RTCPeerConnection:
//   \\  -> backslash
//   \"  -> double-quote
//   \/  -> solidus (allowed but never required)
//   \b  -> backspace
//   \f  -> form feed
//   \n  -> newline (0x0A)
//   \r  -> carriage return (0x0D)
//   \t  -> horizontal tab
//   \uXXXX -> Unicode codepoint (BMP; surrogate pair collapsed to one code)
// Leaves unknown escape sequences alone (passes the backslash through) so the
// parser never silently corrupts data.
static void json_unescape(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ) {
        char c = s[i];
        if (c != '\\' || i + 1 >= s.size()) {
            out.push_back(c);
            ++i;
            continue;
        }
        char esc = s[i + 1];
        switch (esc) {
        case '\\': out.push_back('\\'); i += 2; break;
        case '"':  out.push_back('"');  i += 2; break;
        case '/':  out.push_back('/');  i += 2; break;
        case 'b':  out.push_back('\b'); i += 2; break;
        case 'f':  out.push_back('\f'); i += 2; break;
        case 'n':  out.push_back('\n'); i += 2; break;
        case 'r':  out.push_back('\r'); i += 2; break;
        case 't':  out.push_back('\t'); i += 2; break;
        case 'u':
            if (i + 5 < s.size()) {
                auto hex = s.substr(i + 2, 4);
                unsigned cp = 0;
                bool ok = true;
                for (char h : hex) {
                    cp <<= 4;
                    if      (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else { ok = false; break; }
                }
                if (ok) {
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    i += 6;
                    break;
                }
            }
            out.push_back('\\');
            ++i;
            break;
        default:
            out.push_back('\\');
            out.push_back(esc);
            i += 2;
            break;
        }
    }
    s = std::move(out);
}

// Pull "key":"value" out of a flat JSON object.  Only handles the shapes we
// emit ourselves (no nesting, no escape of the value itself).
// Tolerates whitespace after the colon (Python's json.dumps adds a space).
bool json_get_string(std::string_view json, std::string_view key,
                      std::string* out) {
    // Build the needle manually (avoids requiring C++23 std::format on
    // compilers older than GCC 13).
    std::string needle;
    needle.reserve(key.size() + 3);
    needle.append("\"").append(key).append("\":");
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    // Skip optional whitespace after the colon.
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;  // skip opening quote
    auto end = json.find('"', pos);
    if (end == std::string::npos) return false;
    out->assign(json.substr(pos, end - pos));
    // WebRTC SDPs carried as JSON values have CR/LF escaped as \\r / \\n by
    // JSON.stringify (e.g. {"sdp":"v=0\\r\\no=..."}).  Decode them so the
    // SDP parser sees real CRLFs and not 4-character literals.
    json_unescape(*out);
    return true;
}

bool json_get_string_field(std::string_view json, std::string_view key,
                            std::string* out) {
    return json_get_string(json, key, out);
}
[[maybe_unused]] static auto _unused_anchor = &json_get_string_field;

// Emit a JSON line to stdout (flushed) and stderr log it.
void emit_json(std::string_view type, std::string_view body) {
    std::ostringstream oss;
    oss << "{\"type\":\"" << type << "\"," << body << "}\n";
    std::string msg = oss.str();
    std::fprintf(stdout, "%s", msg.c_str());
    std::fflush(stdout);
    std::fprintf(stderr, "[demo-p2p] >> %s\n", type.data());
}

void emit_offer(std::string_view sdp) {
    std::ostringstream oss;
    oss << "\"sdp\":\"" << json_escape(sdp) << "\"";
    emit_json("offer", oss.str());
}
void emit_answer(std::string_view sdp) {
    std::ostringstream oss;
    oss << "\"sdp\":\"" << json_escape(sdp) << "\"";
    emit_json("answer", oss.str());
}
void emit_candidate(std::string_view cand,
                     int sdp_m_line_index,
                     std::string_view sdp_mid) {
    // Chrome's RTCPeerConnection.addIceCandidate() requires sdpMid +
    // sdpMLineIndex to associate the candidate with a media stream.
    // Without both, Chrome rejects the candidate with:
    //   "Candidate missing values for both sdpMid and sdpMLineIndex"
    // and ICE never forms a working pair with the peer, which silently
    // stalls DTLS (DTLS records travel through the same ICE-selected
    // path).  Forward both fields over the signaling channel so Chrome
    // can install the candidate correctly.
    std::string body;
    body.reserve(cand.size() + 64);
    body  = std::string("\"candidate\":\"") + json_escape(cand) + "\",";
    body += std::string("\"sdpMLineIndex\":") + std::to_string(sdp_m_line_index) + ",";
    body += std::string("\"sdpMid\":\"")     + json_escape(sdp_mid)      + "\"";
    emit_json("candidate", body);
}

// Read one line from stdin with a timeout.  Returns true on success; false on
// timeout.  Returns false on EOF (line will be partial).
bool read_line_peek(std::string* line, int timeout_ms) {
    line->clear();
    [[maybe_unused]] auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);

#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);

    // Detect whether stdin is a pipe/redirected file or a console.
    bool is_pipe = GetFileType(h) != FILE_TYPE_CHAR;  // true if not a console TTY

    if (is_pipe) {
        // Pipe path: use PeekNamedPipe for non-blocking detection.
        while (true) {
            DWORD avail = 0;
            if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
                if (avail > 0) {
                    // Data is available — read one line.
                    int ch = fgetc(stdin);
                    while (ch != EOF && ch != '\n') {
                        line->push_back(static_cast<char>(ch));
                        ch = fgetc(stdin);
                    }
                    if (ch == EOF && line->empty()) return false;
                    rtrim(*line);
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            Sleep(10);
        }
    }

    // Console path: use PeekConsoleInput for interactive terminal input.
    while (true) {
        DWORD avail = 0;
        if (!PeekConsoleInput(h, nullptr, 0, &avail) ||
            !GetNumberOfConsoleInputEvents(h, &avail)) {
            // Not a console (piped); fall back to blocking read.
            int c = fgetc(stdin);
            if (c == EOF) return false;
            if (c == '\n') { rtrim(*line); return true; }
            line->push_back(static_cast<char>(c));
            continue;
        }
        if (avail > 0) {
            INPUT_RECORD rec;
            DWORD read = 0;
            if (ReadConsoleInputW(h, &rec, 1, &read) && rec.EventType == KEY_EVENT
                && rec.Event.KeyEvent.bKeyDown) {
                wchar_t wc = rec.Event.KeyEvent.uChar.UnicodeChar;
                if (wc == L'\r') {
                    char c = '\n'; line->push_back(c);
                } else if (wc && wc < 0x80) {
                    line->push_back(static_cast<char>(wc));
                }
                if (!line->empty() && line->back() == '\n') {
                    line->pop_back();
                    rtrim(*line);
                    return true;
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(10);
    }
#else
    // POSIX: use poll() with a short timeout.
    while (true) {
        struct pollfd pfd = {fileno(stdin), POLLIN, 0};
        int r = poll(&pfd, 1, timeout_ms);
        if (r <= 0) return false;
        int c = fgetc(stdin);
        if (c == EOF) return false;
        if (c == '\n') { rtrim(*line); return true; }
        line->push_back(static_cast<char>(c));
    }
#endif
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

int run_answerer_file(nimrtc::engine::NimRTCEngine& engine,
                       const char* offer_path) {
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

// ─────────────────────────────────────────────────────────────────────────────
// Signaling-proxy mode
// ─────────────────────────────────────────────────────────────────────────────
// TODO(P1): demo-p2p uses stdin/stdout for signaling proxy mode, bridged by
// signaling_proxy.py. This is functional but requires Python. For true cross-platform
// deployment, consider embedding a WebSocket client directly in demo-p2p using a
// lightweight library (e.g., libwebsockets, uWebSockets, or a custom minimal WS client).
//
// TODO(P1): The current implementation does not capture microphone audio. For a
// complete demo, add audio capture using platform-specific APIs (CoreAudio on macOS,
// WASAPI on Windows, ALSA/PulseAudio on Linux).
//
// TODO(P1): The ICE connection state is monitored but not reported to the proxy.
// Chrome interop requires ICE connected state to be verified before RTP is sent.

struct ProxyState {
    nimrtc::engine::NimRTCEngine* engine = nullptr;
    // ICE-aware view of the engine's transport. Acquired via the new
    // `engine.get_ice_transport()` accessor (which returns the engine's
    // `plugins::IICETransport*`), so we no longer reach past the plugin
    // seam with `dynamic_cast<ice::IceTransport*>`.
    nimrtc::plugins::IICETransport* ice = nullptr;
    std::atomic<bool> ice_connected{false};
    std::atomic<bool> running{true};
    std::atomic<bool> answerer{false};
    std::atomic<bool> sent_local_candidates{false};
    int duration_sec = 60;

    // ---- Video driver (single-threaded; runs on the tick thread) --------
    // We use the engine's own `video_source_` plugin instance and drive
    // it manually with `produce_one()` from the tick loop.  The source's
    // frame callback is wired to `engine.send_video()`.  This keeps all
    // video codec / sender access on a single thread (the tick thread),
    // avoiding the IVideoCodec / IVideoSender thread-safety hazard called
    // out in engine.cpp::init_video_plugins().
    std::atomic<std::uint64_t> video_tx_frames{0};
    std::atomic<std::uint64_t> video_rx_nals{0};
    std::int64_t              next_video_emit_us = 0;
    int                       video_fps = 0;     // 0 = video disabled
};

// Drain transport every 10 ms; emit ICE candidates once when gathering completes.
//
// We also synthesise a 440 Hz sine-wave PCM frame at the engine's
// configured PCM rate (8 kHz mono for PCMU, 48 kHz mono for Opus) and feed
// it into engine->send_audio() once ICE has reached connected/completed.
// Without this, the e2e Chrome test never sees any RTP packets from
// NimRTC and "audioReceived"/"rtpPackets" stay at zero — DTLS can be
// fully connected and SRTP keys installed, but if we never emit audio
// Chrome's inbound-rtp stats never move.
static void engine_tick_thread(ProxyState* st) {
    auto end = std::chrono::steady_clock::now()
             + std::chrono::seconds(st->duration_sec);

    // Wait for ICE gathering to settle before sending the offer/answer.
    if (st->ice) {
        st->ice->wait_for_gathering(2000);
    }

    // PCM ring buffer + 440 Hz sine generator.  20 ms frames at the
    // engine's rate = pcm_sample_rate_hz / 50 samples.  We cycle
    // monotonically so the Opus encoder sees a real signal (not silence).
    double phase  = 0.0;
    double dphase = 0.0;     // set once we know the engine's PCM rate
    std::vector<float> pcm;
    bool rate_set = false;
    static thread_local int audio_packets_sent = 0;

    while (st->running.load() && std::chrono::steady_clock::now() < end) {
        st->engine->tick();

        std::string state = st->engine->ice_state_string();
        if ((state == "connected" || state == "completed")
            && !st->ice_connected.load()) {
            st->ice_connected.store(true);
            std::fprintf(stderr, "[demo-p2p] ICE %s\n", state.c_str());
        }

        // Drive synthetic audio into the engine once ICE is up.  We
        // do not gate on DTLS — engine.send_audio() will internally
        // enqueue the SRTP-protected packet and send_buffer through
        // ice_t_->send() which drops packets if ICE isn't connected
        // (so before ICE=connected we drop on the floor).
        if (st->ice_connected.load()) {
            if (!rate_set) {
                const auto& cfg = st->engine->config();
                dphase = 2.0 * 3.14159265358979323846 * 440.0 /
                         static_cast<double>(cfg.pcm_sample_rate_hz);
                // 20 ms frame at the engine's PCM rate.
                std::size_t frame = cfg.pcm_sample_rate_hz / 50u;
                pcm.assign(frame, 0.0f);
                rate_set = true;
            }
            for (auto& s : pcm) {
                s = static_cast<float>(0.2 * std::sin(phase));
                phase += dphase;
                if (phase > 6.283185307179586) phase -= 6.283185307179586;
            }
            auto rc = st->engine->send_audio(pcm.data(), pcm.size());
            ++audio_packets_sent;
            if (audio_packets_sent % 50 == 0) {
                std::fprintf(stderr,
                    "[demo-p2p] audio frames sent=%d (sample_rate=%u) rc=0x%X\n",
                    audio_packets_sent,
                    st->engine->config().pcm_sample_rate_hz,
                    static_cast<unsigned>(rc));
            }

            // ---- Video drive loop ---------------------------------------
            // Pull a frame from the engine's video source at the
            // configured cadence; the source callback (wired in
            // run_signaling_proxy below) forwards the frame into
            // engine.send_video().  The cadence here is wall-clock,
            // independent of audio — so audio at 48 kHz / 20 ms and
            // video at e.g. 15 fps / 66 ms coexist.
            if (st->video_fps > 0) {
                const std::int64_t now_us = std::chrono::duration_cast<
                    std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (now_us >= st->next_video_emit_us) {
                    if (auto* src = st->engine->video_source()) {
                        src->produce_one();
                        st->video_tx_frames.fetch_add(1);
                        if (st->video_tx_frames.load() % 30 == 0) {
                            std::fprintf(stderr,
                                "[demo-p2p] video frames sent=%llu (rx_nals=%llu)\n",
                                static_cast<unsigned long long>(
                                    st->video_tx_frames.load()),
                                static_cast<unsigned long long>(
                                    st->video_rx_nals.load()));
                        }
                    }
                    const std::int64_t interval_us =
                        1'000'000 / st->video_fps;
                    // Catch-up if we fell behind (avoid burst-stacking
                    // when the thread was stalled on a slow tick).
                    st->next_video_emit_us = now_us + interval_us;
                    if (st->next_video_emit_us < now_us) {
                        st->next_video_emit_us = now_us + interval_us;
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    st->running.store(false);
}

int run_signaling_proxy(nimrtc::engine::NimRTCEngine& engine,
                         bool answerer, int duration_sec,
                         const char* turn_host, int turn_port,
                         const char* turn_user, const char* turn_pass) {
    ProxyState st;
    st.engine = &engine;
    st.answerer.store(answerer);
    st.duration_sec = duration_sec;
    st.video_fps = (engine.video_source() != nullptr) ? 15 : 0;

    // Apply TURN relay configuration before open() so libjuice gathers
    // relay candidates from the TURN server.
    if (turn_host && turn_host[0]) {
        const int rc = engine.add_turn_server(
            turn_host,
            static_cast<std::uint16_t>(turn_port),
            turn_user ? turn_user : "",
            turn_pass ? turn_pass : "");
        if (rc != 0) {
            std::fprintf(stderr, "demo-p2p: add_turn_server failed\n");
            return 1;
        }
    }

    if (engine.open() != 0) {
        std::fprintf(stderr, "demo-p2p: engine open failed rc=0x%04X (state=%s)\n",
                     engine.last_open_rc(),
                     engine.ice_state_string());
        return 1;
    }

    // Resolve the ICE-aware transport through the engine's new accessor
    // (no more `dynamic_cast<ice::IceTransport*>` reaching past the
    // plugin seam). Returns nullptr only if the engine was configured
    // with a transport that doesn't implement IICETransport.
    st.ice = engine.get_ice_transport();
    if (!st.ice) {
        std::fprintf(stderr, "demo-p2p: transport does not expose IICETransport\n");
        engine.close();
        return 1;
    }

    engine.set_on_state_change([](const char* s) {
        std::fprintf(stderr, "[state] %s\n", s);
    });
    engine.set_on_error([](uint32_t e, std::string_view m) {
        std::fprintf(stderr, "[error 0x%04X] %.*s\n", e,
                     static_cast<int>(m.size()), m.data());
    });

    // ---- Video source wiring ---------------------------------------------
    // The video source's frame callback fires synchronously inside
    // produce_one(); we forward the frame into engine.send_video().
    // The codec adapter in init_video_plugins() picks the I420 planes
    // up and feeds the H.264 stub encoder → video sender → RTP.
    if (auto* src = engine.video_source()) {
        src->set_callback([&engine](
                const nimrtc::plugins::VideoSourceFrame& f) {
            // Called on the tick thread (single-threaded driver).
            engine.send_video(f);
        });
    }
    // Increment RX counter from the NAL callback.  We capture `&st` (a
    // stack-stable pointer — ProxyState outlives the engine) so the
    // callback does NOT need to live on the engine.
    engine.set_on_video_frame([&st](const std::uint8_t* /*nal*/,
                                    std::size_t /*len*/,
                                    bool /*keyframe*/) {
        st.video_rx_nals.fetch_add(1, std::memory_order_relaxed);
    });

    // If NIMRTC_DTLS_SPKI_FILE is set, write the local DTLS fingerprint
    // (base64 SPKI hash) to it now that the engine is open.  Chrome's
    // --ignore-certificate-errors-spki-list flag expects this format and
    // the signaling_proxy uses the file to launch Chrome with the right
    // allow-list entry.  See interop/signaling/signaling_proxy.py.
    if (const char* spki_path = std::getenv("NIMRTC_DTLS_SPKI_FILE")) {
        std::string b64 = engine.local_dtls_fingerprint_sha256_base64();
        if (!b64.empty()) {
            std::FILE* f = std::fopen(spki_path, "wb");
            if (f) {
                std::fwrite(b64.data(), 1, b64.size(), f);
                std::fclose(f);
                std::fprintf(stderr,
                             "[demo-p2p] wrote DTLS SPKI (base64 SHA-256) "
                             "to %s (len=%zu)\n",
                             spki_path, b64.size());
            } else {
                std::fprintf(stderr,
                             "[demo-p2p] failed to open %s for SPKI write\n",
                             spki_path);
            }
        } else {
            std::fprintf(stderr,
                         "[demo-p2p] NIMRTC_DTLS_SPKI_FILE set but local "
                         "fingerprint unavailable yet (engine not open?)\n");
        }
    }

    // Tick thread drives ICE + drains transport.
    std::thread tick_t(engine_tick_thread, &st);

    if (!answerer) {
        // Offerer: send offer + local candidates.
        auto offer = engine.create_offer();
        if (offer.empty()) {
            std::fprintf(stderr, "demo-p2p: create_offer failed\n");
            st.running.store(false);
            tick_t.join();
            engine.close();
            return 2;
        }
        std::fprintf(stderr, "[demo-p2p] offer SDP len=%zu:\n%s\n",
                     offer.size(), offer.c_str());
        emit_offer(offer);

        // For BUNDLE-only audio sessions (RFC 8829), all media shares a single
        // transport with mid="0" and sdpMLineIndex=0.  If we ever generate
        // multi-m-line SDP, look up the right mid per candidate via the
        // engine's bundle_mids[] instead of hard-coding "0".
        constexpr const char* kAudioMid   = "0";
        constexpr int         kAudioMLine = 0;

        for (const auto& c : st.ice->gathered_local_candidates()) {
            // libjuice emits each candidate as a single-line SDP attribute
            // line, with the "a=candidate:" prefix and NO trailing CRLF:
            //
            //   a=candidate:<foundation> <component> UDP <priority>
            //            <host> <port> typ <type> [raddr ...]
            //
            // We forward each line as-is to the peer.  DO NOT strip the
            // "a=" prefix (which is what older versions of this code did)
            // because the foundation and transport tokens immediately
            // follow the colon — stripping "a=candidate:" (11 chars) would
            // drop the foundation, and stripping "candidate:" (10 chars)
            // would corrupt the foundation number.  Either form produces
            // an "Unsupported candidate type" SDP parse error in Chrome
            // because the line then reads as:
            //
            //   candidate:1 UDP ... typ host     (missing foundation)
            //
            // and on top of that the trailing `a=ssrc:...` line (which
            // we inject right after this) ends up glued to the candidate
            // line, e.g.
            //
            //   candidate:1 UDP ... typ hosta=ssrc:3735928559 ...
            //
            // Just forward the raw libjuice line; the signaling server
            // passes JSON strings through unchanged.
            emit_candidate(c, kAudioMLine, kAudioMid);
        }
        st.sent_local_candidates.store(true);
    } else {
        std::fprintf(stderr, "[demo-p2p] waiting for offer on stdin...\n");
    }

    // Main loop: read JSON lines from stdin and dispatch.
    while (st.running.load()) {
        // Non-blocking peek: try stdin; if no data, sleep briefly and recheck.
        std::string line;
        if (!read_line_peek(&line, 100)) {
            continue;  // no data yet, recheck st.running
        }
        if (line.empty()) continue;

        std::string type;
        if (!json_get_string(line, "type", &type)) {
            std::fprintf(stderr, "[demo-p2p] malformed JSON: %s\n",
                          line.substr(0, 80).c_str());
            continue;
        }

        std::fprintf(stderr, "[demo-p2p] << %s\n", type.c_str());

        if (type == "offer") {
            // We're answerer (or renegotiation): process offer and emit answer.
            std::string sdp;
            if (!json_get_string(line, "sdp", &sdp)) continue;
            std::fprintf(stderr, "[demo-p2p] offer SDP len=%zu:\n%s\n",
                         sdp.size(), sdp.c_str());
            auto answer = engine.process_remote_sdp(sdp);
            if (!answer) {
                std::fprintf(stderr, "demo-p2p: process_remote_sdp(offer) failed\n");
                continue;
            }
            std::fprintf(stderr, "[demo-p2p] answer SDP len=%zu:\n%s\n",
                         answer->size(), answer->c_str());

            // Append a=ssrc:<SSRC> cname:<cname> to the last media section
            // of the answer SDP if not already present.  This is required
            // for Chrome interop (see notes in interop/signaling/signaling_proxy.py
            // — Chrome refuses to count inbound RTP packets in
            // `inbound-rtp` reports and treats the audio track as silent
            // until the answer SDP carries at least one a=ssrc line that
            // matches the RTP-header SSRC on the wire).
            //
            // The SSRC must match what src/engine/src/engine.cpp::tick_audio
            // stamps into the RTP header: static std::atomic<uint32_t>
            // ssrc_val{0xDEADBEEF}.  If that value ever changes, this
            // constant must be updated in lockstep.
            static constexpr std::uint32_t kAudioSsrc = 0xDEADBEEFu;
            {
                std::string& s = *answer;
                bool has_ssrc = s.find("a=ssrc:") != std::string::npos;
                if (!has_ssrc) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                                  "a=ssrc:%u cname:nimrtc-audio\r\n",
                                  static_cast<unsigned>(kAudioSsrc));
                    // Append the ssrc line to the end of the SDP.
                    //
                    // BUGFIX: the previous implementation ran a `while` loop
                    // that popped EVERY trailing '\r' / '\n' before appending
                    // `buf`.  Because `buf` itself ends with "\r\n", the
                    // pop-then-append sequence consumed the CRLF that
                    // separated the munger's last emitted line from our new
                    // `a=ssrc:` line, producing malformed SDP like:
                    //
                    //   ...a=candidate:4 1 UDP ... 172.25.80.1 55363 typ
                    //   hosta=ssrc:3735928559 cname:nimrtc-audio
                    //
                    // Chrome rejects this with "Unsupported candidate type"
                    // because it parses the whole blob as a single
                    // `a=candidate:` line whose `typ` token is
                    // "hosta=ssrc:3735928559" — which isn't a known ICE
                    // candidate type.  setRemoteDescription then fails
                    // BEFORE the DTLS handshake ever starts, so
                    // `audioReceived=false`, `rtpPackets=0`,
                    // `connectionState=failed` even though NimRTC's DTLS
                    // stack and certificate are perfectly healthy.
                    //
                    // The munger already terminates every emitted line
                    // (including the last one) with "\r\n" (see
                    // `src/modules/sdp/src/munger.cpp`), so the cleanest
                    // fix is to simply concatenate `buf` — its own trailing
                    // "\r\n" becomes the separator between the two lines.
                    s.append(buf);
                    std::fprintf(stderr,
                                 "[demo-p2p] appended a=ssrc:%u to answer SDP\n",
                                 static_cast<unsigned>(kAudioSsrc));
                }
            }

            // Optional debug dump: NIMRTC_ANSWER_DUMP=<path> writes the
            // emitted answer SDP verbatim for offline inspection.
            if (const char* dump = std::getenv("NIMRTC_ANSWER_DUMP")) {
                std::FILE* f = std::fopen(dump, "wb");
                if (f) {
                    std::fwrite(answer->data(), 1, answer->size(), f);
                    std::fclose(f);
                    std::fprintf(stderr, "[demo-p2p] answer SDP written to %s\n",
                                 dump);
                }
            }
            emit_answer(*answer);
            // Forward local candidates.  See note above re: BUNDLE mid.
            constexpr const char* kAudioMid   = "0";
            constexpr int         kAudioMLine = 0;
            for (const auto& c : st.ice->gathered_local_candidates()) {
                // libjuice already returns a complete "a=candidate:..." SDP
                // attribute line — pass it through verbatim (see long
                // comment in the offerer path above for why stripping the
                // prefix corrupts the candidate).
                emit_candidate(c, kAudioMLine, kAudioMid);
            }
            st.sent_local_candidates.store(true);

        } else if (type == "answer") {
            // We're offerer: apply answer.
            std::string sdp;
            if (!json_get_string(line, "sdp", &sdp)) continue;
            std::fprintf(stderr, "[demo-p2p] got answer SDP len=%zu full=%s\n",
                         sdp.size(), sdp.c_str());
            auto result = engine.process_remote_sdp(sdp);
            if (!result) {
                std::fprintf(stderr, "demo-p2p: process_remote_sdp(answer) failed\n");
                continue;
            }
            std::fprintf(stderr, "[demo-p2p] process_remote_sdp(answer) OK\n");

        } else if (type == "candidate") {
            // Add remote ICE candidate.
            std::string cand;
            if (!json_get_string(line, "candidate", &cand)) continue;
            // libjuice's juice_add_remote_candidate requires the "a=" SDP
            // attribute prefix (see ice.c::ice_parse_candidate_sdp — it
            // match_prefix() against "a=candidate:" before delegating to
            // parse_sdp_candidate, which itself strips both prefixes).
            // The JSON wire format carries the value with a leading
            // "candidate:" prefix already, so prepend only the missing "a=".
            constexpr std::string_view kFull = "a=candidate:";
            constexpr std::string_view kBare = "candidate:";
            if (cand.size() >= kFull.size() &&
                cand.compare(0, kFull.size(), kFull) == 0) {
                // already correctly prefixed — leave alone
            } else if (cand.size() >= kBare.size() &&
                       cand.compare(0, kBare.size(), kBare) == 0) {
                cand = std::string("a=") + cand;
            } else {
                cand = std::string(kFull) + cand;
            }
            st.ice->add_remote_candidate(cand);
            std::fprintf(stderr, "[demo-p2p] added remote candidate\n");

        } else if (type == "bye") {
            std::fprintf(stderr, "[demo-p2p] received bye, exiting\n");
            break;
        } else {
            std::fprintf(stderr, "[demo-p2p] unknown type '%s'\n",
                          type.c_str());
        }
    }

    st.running.store(false);
    tick_t.join();
    engine.close();
    return st.ice_connected.load() ? 0 : 6;
}

} // anonymous namespace

int main(int argc, char** argv) {
    std::fprintf(stderr, "NimRTC demo-p2p\n");
    // Enable verbose logging so we can see ICE/DTLS state transitions.
    nimrtc::core::log::Logger::instance().set_level(
        nimrtc::core::log::Level::Debug);

    bool proxy_mode = false;
    bool proxy_answerer = false;
    int  proxy_duration = 60;
    std::string codec_arg;  // empty = use default (opus or auto-fallback)
    std::string bind_arg;          // empty = default ("127.0.0.1")
    std::string stun_host_arg;     // empty = default ("stun.l.google.com")
    int  stun_port_arg = 0;        // 0 = default (19302)
    bool no_stun = false;
    std::string turn_host_arg;
    int  turn_port_arg = 3478;
    std::string turn_user_arg;
    std::string turn_pass_arg;

    // ---- Video flag (defaults to off; H.264 stub @ 640x480 / 15 fps) -----
    bool video_enabled = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--signaling-proxy") == 0) {
            proxy_mode = true;
        } else if (std::strcmp(argv[i], "--answerer") == 0) {
            proxy_answerer = true;
        } else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            proxy_duration = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            codec_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--stun-host") == 0 && i + 1 < argc) {
            stun_host_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--stun-port") == 0 && i + 1 < argc) {
            stun_port_arg = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-stun") == 0) {
            no_stun = true;
        } else if (std::strcmp(argv[i], "--turn-host") == 0 && i + 1 < argc) {
            turn_host_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--turn-port") == 0 && i + 1 < argc) {
            turn_port_arg = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--turn-user") == 0 && i + 1 < argc) {
            turn_user_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--turn-pass") == 0 && i + 1 < argc) {
            turn_pass_arg = argv[++i];
        } else if (std::strcmp(argv[i], "--video") == 0) {
            video_enabled = true;
        }
    }

    nimrtc::engine::EngineConfig cfg;
    cfg.local_bind_address = bind_arg.empty() ? "127.0.0.1" : bind_arg;
    if (no_stun) {
        cfg.stun_server_host.clear();
        cfg.stun_server_port = 0;
    } else {
        cfg.stun_server_host = stun_host_arg.empty() ? "stun.l.google.com" : stun_host_arg;
        cfg.stun_server_port = stun_port_arg == 0 ? 19302
                                                  : static_cast<std::uint16_t>(stun_port_arg);
    }
    if (!turn_host_arg.empty()) {
        std::fprintf(stderr, "[demo-p2p] TURN server: %s:%d\n",
                     turn_host_arg.c_str(), turn_port_arg);
    }
    cfg.pcm_sample_rate_hz = 48000;
    cfg.pcm_channels       = 1;

    // ---- Video config (when --video is passed) ---------------------------
    if (video_enabled) {
        cfg.video_source_name   = "memory";
        cfg.video_sink_name     = "headless";
        cfg.video_receiver_name = "reference";
        cfg.video_sender_name   = "reference";
        cfg.video_codec_name    = "h264";
        cfg.video_sender_tuning.ssrc         = 0xCAFEBABEu;
        cfg.video_sender_tuning.payload_type = 102;
        cfg.video_sender_tuning.mtu          = 1200;
        cfg.video_receiver_tuning.ssrc         = cfg.video_sender_tuning.ssrc;
        cfg.video_receiver_tuning.payload_type = cfg.video_sender_tuning.payload_type;
        std::fprintf(stderr,
                     "[demo-p2p] video: enabled (640x480 @ 15 fps, H.264 stub)\n");
    }

    // Codec selection: user-specified takes priority; otherwise auto-fallback.
    // opus: RFC 7587, payload type 111, 48 kHz stereo.
    // pcmu: G.711 μ-law, payload type 0, 8 kHz mono (WebRTC mandatory).
    auto set_codec_opus = [&] {
        cfg.audio_codec.payload_type  = 111;
        cfg.audio_codec.encoding      = "opus";
        cfg.audio_codec.clock_rate    = 48000;
        cfg.audio_codec.channels      = 2;
        cfg.audio_codec.fmtp          = "minptime=10;useinbandfec=1;stereo=0;sprop-stereo=0";
        cfg.codec_name               = "opus";
        cfg.pcm_sample_rate_hz        = 48000;
        cfg.pcm_channels             = 1;
    };
    auto set_codec_pcmu = [&] {
        cfg.audio_codec.payload_type  = 0;
        cfg.audio_codec.encoding      = "PCMU";
        cfg.audio_codec.clock_rate    = 8000;
        cfg.audio_codec.channels      = 1;
        cfg.audio_codec.fmtp          = "";
        cfg.codec_name                = "";  // no plugin; engine uses fallback path
        cfg.pcm_sample_rate_hz        = 8000;
        cfg.pcm_channels             = 1;
    };

    if (!codec_arg.empty()) {
        if (codec_arg == "opus") {
            set_codec_opus();
        } else if (codec_arg == "pcmu") {
            set_codec_pcmu();
        } else {
            std::fprintf(stderr, "demo-p2p: unknown codec '%s' (use opus or pcmu)\n",
                         codec_arg.c_str());
            return 64;
        }
    } else {
        // Auto-detect: use opus if NIMRTC_HAS_OPUS is defined, otherwise pcmu.
#ifdef NIMRTC_HAS_OPUS
        set_codec_opus();
        std::fprintf(stderr, "[demo-p2p] audio codec: opus (NIMRTC_HAS_OPUS)\n");
#else
        set_codec_pcmu();
        std::fprintf(stderr, "[demo-p2p] audio codec: pcmu (opus not available)\n");
#endif
    }

    nimrtc::engine::NimRTCEngine engine(cfg);

    // Only "offer" and "answer" are valid positional sub-commands.
    // Any other token after flags is an error (e.g. --codec opus <junk>).
    if (argc > 1) {
        const bool is_offer  = (std::strcmp(argv[1], "offer")  == 0);
        const bool is_answer = (std::strcmp(argv[1], "answer") == 0);
        const bool is_flag   = (argv[1][0] == '-');
        if (!is_offer && !is_answer && !is_flag) {
            print_usage(argv[0]);
            return 64;
        }
    }

    if (proxy_mode) {
        return run_signaling_proxy(engine, proxy_answerer, proxy_duration,
                                 turn_host_arg.c_str(), turn_port_arg,
                                 turn_user_arg.c_str(), turn_pass_arg.c_str());
    }

    if (argc > 1 && (std::strcmp(argv[1], "-h") == 0 ||
                     std::strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

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
    if (const char* spki_path = std::getenv("NIMRTC_DTLS_SPKI_FILE")) {
        std::string b64 = engine.local_dtls_fingerprint_sha256_base64();
        if (!b64.empty()) {
            std::FILE* f = std::fopen(spki_path, "wb");
            if (f) {
                std::fwrite(b64.data(), 1, b64.size(), f);
                std::fclose(f);
                std::fprintf(stderr,
                             "[demo-p2p] wrote DTLS SPKI (base64 SHA-256) "
                             "to %s (len=%zu)\n",
                             spki_path, b64.size());
            }
        }
    }

    int rc = 0;
    // Valid positional sub-commands are: [no args] → offerer, "offer <file>", "answer <file>".
    const bool has_codec_flag = !codec_arg.empty();
    if (argc >= 3 && std::strcmp(argv[1], "offer") == 0) {
        rc = run_answerer_file(engine, argv[2]);
    } else if (argc >= 3 && std::strcmp(argv[1], "answer") == 0) {
        rc = run_apply_answer(engine, argv[2]);
    } else if (argc == 1 || has_codec_flag) {
        // No positional args (or only --codec was used): run as offerer.
        rc = run_offerer(engine);
    } else {
        print_usage(argv[0]);
        rc = 64;
    }

    engine.close();
    return rc;
}

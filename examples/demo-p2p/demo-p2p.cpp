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

#define _CRT_SECURE_NO_WARNINGS

#ifdef _WIN32
    #include <windows.h>
    #define poll(fd, nfds, timeout)  (0)  // not used on Windows path
#else
    #include <poll.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

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
                 "  %s --no-stun              Disable STUN candidate gathering\n",
                 prog, prog, prog, prog, prog, prog, prog, prog, prog);
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
                    if      (h >= '0' && h <= '9') cp |= (h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
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
    std::string needle = std::format("\"{}\":", key);
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

// Emit a JSON line to stdout (flushed) and stderr log it.
void emit_json(std::string_view type, std::string_view body) {
    std::string msg = std::format("{{\"type\":\"{}\",{}}}", type, body);
    std::fprintf(stdout, "%s\n", msg.c_str());
    std::fflush(stdout);
    std::fprintf(stderr, "[demo-p2p] >> %s\n", type.data());
}

void emit_offer(std::string_view sdp) {
    emit_json("offer", std::format("\"sdp\":\"{}\"", json_escape(sdp)));
}
void emit_answer(std::string_view sdp) {
    emit_json("answer", std::format("\"sdp\":\"{}\"", json_escape(sdp)));
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
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);

#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    while (true) {
        DWORD avail = 0;
        if (!PeekConsoleInput(h, nullptr, 0, &avail) ||
            !GetNumberOfConsoleInputEvents(h, &avail)) {
            // Not a console (piped); fall back to blocking read.
            int c = fgetc(stdin);
            if (c == EOF) return false;
            if (c == '\n') return true;
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
};

// Drain transport every 10 ms; emit ICE candidates once when gathering completes.
static void engine_tick_thread(ProxyState* st) {
    auto end = std::chrono::steady_clock::now()
             + std::chrono::seconds(st->duration_sec);

    // Wait for ICE gathering to settle before sending the offer/answer.
    if (st->ice) {
        st->ice->wait_for_gathering(2000);
    }

    while (st->running.load() && std::chrono::steady_clock::now() < end) {
        st->engine->tick();

        std::string state = st->engine->ice_state_string();
        if ((state == "connected" || state == "completed")
            && !st->ice_connected.load()) {
            st->ice_connected.store(true);
            std::fprintf(stderr, "[demo-p2p] ICE %s\n", state.c_str());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    st->running.store(false);
}

int run_signaling_proxy(nimrtc::engine::NimRTCEngine& engine,
                         bool answerer, int duration_sec) {
    ProxyState st;
    st.engine = &engine;
    st.answerer.store(answerer);
    st.duration_sec = duration_sec;

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
            std::string stripped = c;
            constexpr std::string_view kA = "a=candidate:";
            constexpr std::string_view kC = "candidate:";
            if (stripped.size() >= kA.size() &&
                stripped.compare(0, kA.size(), kA) == 0) {
                stripped = stripped.substr(kA.size());
            } else if (stripped.size() >= kC.size() &&
                       stripped.compare(0, kC.size(), kC) == 0) {
                stripped = stripped.substr(kC.size());
            }
            std::string line = std::string("candidate:") + stripped;
            emit_candidate(line, kAudioMLine, kAudioMid);
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
            auto answer = engine.process_remote_sdp(sdp);
            if (!answer) {
                std::fprintf(stderr, "demo-p2p: process_remote_sdp(offer) failed\n");
                continue;
            }
            emit_answer(*answer);
            // Forward local candidates.  See note above re: BUNDLE mid.
            constexpr const char* kAudioMid   = "0";
            constexpr int         kAudioMLine = 0;
            for (const auto& c : st.ice->gathered_local_candidates()) {
                std::string stripped = c;
                constexpr std::string_view kA = "a=candidate:";
                constexpr std::string_view kC = "candidate:";
                if (stripped.size() >= kA.size() &&
                    stripped.compare(0, kA.size(), kA) == 0) {
                    stripped = stripped.substr(kA.size());
                } else if (stripped.size() >= kC.size() &&
                           stripped.compare(0, kC.size(), kC) == 0) {
                    stripped = stripped.substr(kC.size());
                }
                std::string cand = std::string("candidate:") + stripped;
                emit_candidate(cand, kAudioMLine, kAudioMid);
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
    cfg.pcm_sample_rate_hz = 48000;
    cfg.pcm_channels       = 1;

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
        return run_signaling_proxy(engine, proxy_answerer, proxy_duration);
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

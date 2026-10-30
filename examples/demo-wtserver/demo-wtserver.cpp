/**
 * @file examples/demo-wtserver/demo-wtserver.cpp
 * @brief demo-wtserver — minimal WebTransport echo server for Case E.
 *
 * A standalone WebTransport server binary that:
 *   1. Starts a msquic-backed IWebTransportServer on UDP port 9443.
 *   2. For each Chrome WebTransport connection that arrives:
 *        - Accepts the session.
 *        - Sets up a datagram echo: every inbound datagram is sent back.
 *        - Sets up bidirectional stream echo: every peer-initiated
 *          bidirectional stream gets an on_data handler that echoes
 *          each chunk verbatim (preserving the WebTransport stream
 *          framing so Chrome's readable side strips the same header
 *          on the way back).
 *
 * This binary is the NimRTC side of the Case E WebTransport end-to-end
 * acceptance test (Chrome ↔ NimRTC WebTransport interop via msquic).
 *
 * ## Usage
 *
 *   demo-wtserver                          # port 9443, 60s duration
 *   demo-wtserver --port 9443              # custom port
 *   demo-wtserver --duration 30            # custom duration
 *
 * ## Exit codes
 *
 *   0  — at least one session connected AND data was exchanged
 *        (echoed bytes > 0 on either plane)
 *   1  — server failed to start
 *   2  — timeout / no sessions connected / connected but no data
 *
 * ## Chrome connection URL
 *
 *   https://localhost:9443/webtransport
 *
 * Chrome WebTransport JS API (RFC 9298) sends an HTTP/3 CONNECT request
 * on the first bidirectional QUIC stream.  The msquic listener accepts
 * the connection, completes HTTP/3 SETTINGS + WebTransport handshake,
 * and delivers a connected IWebTransportSession here.
 */

#ifdef _WIN32
    #include <windows.h>
    #include <DbgHelp.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/web_transport/bidi_stream_iface.hpp>
#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_server.hpp>

namespace {

// ---- Configuration (command-line) -------------------------------------------

struct Options {
    uint16_t port = 9443;
    int duration_sec = 60;
};

void print_usage(const char* prog) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s [--port <port>] [--duration <sec>]\n"
                 "\n"
                 "  --port <port>       UDP port for WebTransport listener\n"
                 "                       (default: 9443)\n"
                 "  --duration <sec>    Run for at most this many seconds\n"
                 "                       (default: 60; 0 = infinite until ^C)\n"
                 "  --help, -h           Show this help\n",
                 prog);
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opts.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            opts.duration_sec = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0 ||
                   std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "demo-wtserver: unknown argument: %s\n",
                         argv[i]);
            print_usage(argv[0]);
            std::exit(64);
        }
    }
    return opts;
}

// ---- Session driver -------------------------------------------------------
//
// v0.15.0 convergence: explicit echo on BOTH planes so the exit-code
// contract ("0 = session connected AND data exchanged") is deterministic
// and does not depend on the msquic session's legacy fallback echo.
// Counters are process-wide atomics (msquic fires callbacks on its own
// worker threads); main() requires echoed_bytes > 0 for exit 0.
std::atomic<std::uint64_t> g_datagram_echo_bytes{0};
std::atomic<std::uint64_t> g_stream_echo_bytes{0};

void run_session(nimrtc::web_transport::IWebTransportSession* session) {
    std::fprintf(stderr, "[demo-wtserver] session state: %d\n",
                 static_cast<int>(session->state()));

    // Set up datagram echo (RFC 9221 §3 plane).
    session->set_on_datagram(
        [session](std::span<const std::uint8_t> bytes) {
            std::fprintf(stderr,
                         "[demo-wtserver] datagram recv %zuB — echoing\n",
                         bytes.size());
            auto rc = session->send_datagram(bytes);
            if (rc != nimrtc::plugins::kOk) {
                std::fprintf(stderr,
                             "[demo-wtserver] datagram echo failed: 0x%X\n",
                             static_cast<unsigned>(rc));
                return;
            }
            g_datagram_echo_bytes.fetch_add(bytes.size(),
                                            std::memory_order_relaxed);
        });

    // Set up bidirectional-stream echo (Case E stream plane). The session
    // hands us each peer-initiated bidi stream; we install an on_data
    // handler that echoes every chunk verbatim. Verbatim echo preserves
    // the WebTransport (signal 0x41 + session-id + payload) framing so
    // Chrome's readable side strips the same header on the way back.
    session->set_on_peer_stream(
        [](nimrtc::web_transport::IBidirectionalStream* stream) {
            if (stream == nullptr) return;
            std::fprintf(stderr, "[demo-wtserver] peer stream accepted\n");
            stream->set_on_data(
                [stream](std::span<const std::uint8_t> bytes) {
                    if (bytes.empty() || stream == nullptr) return;
                    std::fprintf(stderr,
                                 "[demo-wtserver] stream recv %zuB — echoing\n",
                                 bytes.size());
                    std::vector<std::uint8_t> to_echo;
                    try {
                        to_echo.assign(bytes.begin(), bytes.end());
                    } catch (...) {
                        return;
                    }
                    auto rc = stream->send(std::span<const std::uint8_t>{
                        to_echo.data(), to_echo.size()});
                    if (rc != nimrtc::plugins::kOk) {
                        std::fprintf(stderr,
                                     "[demo-wtserver] stream echo failed: 0x%X\n",
                                     static_cast<unsigned>(rc));
                        return;
                    }
                    g_stream_echo_bytes.fetch_add(bytes.size(),
                                                  std::memory_order_relaxed);
                });
        });

    std::fprintf(stderr,
                 "[demo-wtserver] session handler installed "
                 "(datagram + bidi-stream echo)\n");
}

// ---- Crash diagnostics ----------------------------------------------------
//
// The WebTransport listener runs inside msquic's worker threads, so an
// access violation there kills the process with no diagnostic at all.
// This vectored exception handler prints the faulting address and a
// symbolised backtrace before letting the crash proceed, which is the
// only practical way to diagnose the msquic callback paths from CI.

#if defined(_WIN32)
namespace {

std::atomic<bool> g_crash_reported{false};

void print_symbol(const char* label, void* addr) {
    HANDLE process = GetCurrentProcess();
    char sym_buf[sizeof(SYMBOL_INFO) + 512] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    if (SymFromAddr(process, reinterpret_cast<DWORD64>(addr), &disp, sym)) {
        std::fprintf(stderr, "[crash] %s %s +0x%llX\n", label, sym->Name,
                     static_cast<unsigned long long>(disp));
    } else {
        std::fprintf(stderr, "[crash] %s %p\n", label, addr);
    }
}

LONG WINAPI crash_veh(EXCEPTION_POINTERS* ep) {
    if (ep == nullptr || ep->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // 0xC0000374 = STATUS_HEAP_CORRUPTION, 0xC0000018 = STATUS_INVALID_HEAP.
    // Both are raised by the ntdll heap manager on the thread that touched
    // the corrupted block, so the backtrace below points at the offending
    // free/alloc (not at the code that corrupted the heap earlier).
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != 0xC0000374u &&
        code != 0xC0000018u) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_crash_reported.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

    std::fprintf(stderr, "\n[crash] *** unhandled exception 0x%08X ***\n",
                 code);
    std::fprintf(stderr, "[crash] fault address: %p\n",
                 ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        std::fprintf(stderr, "[crash] param1: %llu  param2: %p\n",
                     static_cast<unsigned long long>(
                         ep->ExceptionRecord->ExceptionInformation[0]),
                     reinterpret_cast<void*>(
                         ep->ExceptionRecord->ExceptionInformation[1]));
    }
    print_symbol("fault:", ep->ExceptionRecord->ExceptionAddress);

    void* frames[64];
    const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
    std::fprintf(stderr, "[crash] backtrace (%u frames):\n",
                 static_cast<unsigned>(count));
    for (USHORT i = 0; i < count; ++i) {
        print_symbol("frame:", frames[i]);
    }
    std::fflush(stderr);
    SymCleanup(process);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace
#endif

// ---- Main -----------------------------------------------------------------

std::atomic<bool> g_running{true};

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD ctrl_type) {
    (void)ctrl_type;
    g_running.store(false, std::memory_order_release);
    return TRUE;
}
#else
void signal_handler(int sig) {
    (void)sig;
    g_running.store(false, std::memory_order_release);
}
#endif

}  // anonymous namespace

int main(int argc, char** argv) {
    std::fprintf(stderr, "NimRTC WebTransport echo server (demo-wtserver)\n");
    std::fprintf(stderr,
                 "  msquic-backed IWebTransportServer\n"
                 "  Case E: Chrome WebTransport ↔ NimRTC interop\n");

    // Set verbose logging for diagnostics.
    nimrtc::core::log::Logger::instance().set_level(
        nimrtc::core::log::Level::Debug);

    const Options opts = parse_args(argc, argv);
    std::fprintf(stderr, "  port:     %u\n", opts.port);
    std::fprintf(stderr, "  duration:  %ds\n", opts.duration_sec);
    std::fprintf(stderr, "\n");

    // Wire ^C / SIGTERM → graceful stop.
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
    AddVectoredExceptionHandler(1, &crash_veh);
#else
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    // Create the msquic-backed WebTransport server.
    nimrtc::web_transport::WebTransportConfig cfg{};
    cfg.max_idle_timeout_ms = 30'000;  // 30 s — ample for test handshake
    cfg.keep_alive_interval_ms = 5'000; // 5 s ping to keep UDP hole open

    std::unique_ptr<nimrtc::web_transport::IWebTransportServer> server;
    try {
        server = nimrtc::web_transport::create_msquic_webtransport_server(cfg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[demo-wtserver] FAILED to create server: %s\n",
                     e.what());
        return 1;
    }

    // Track connection count for exit status.
    std::atomic<int> session_count{0};

    // Every accepted session is kept alive for the lifetime of the
    // process.
    //
    // The on_session callback hands us ownership of the session, and the
    // session owns the underlying QUIC connection.  Dropping it here
    // would shut the connection down immediately — which cancels the
    // HTTP/3 response that has just been queued and leaves the peer
    // waiting for a :status that never arrives.  It would also leave
    // the datagram-echo callback pointing at a destroyed session.
    std::vector<std::unique_ptr<nimrtc::web_transport::IWebTransportSession>>
        sessions;
    std::mutex sessions_mutex;

    // Install the per-session handler.
    server->set_on_session(
        [&session_count, &sessions, &sessions_mutex](
            std::unique_ptr<nimrtc::web_transport::IWebTransportSession> session) {
            const int id = session_count.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[demo-wtserver] [%d] new session (total=%d)\n",
                         id, session_count.load());
            // run_session() installs the echo handlers on the session
            // before it is handed over, so the raw pointer stays valid
            // for as long as the session is kept alive below.
            run_session(session.get());
            std::lock_guard<std::mutex> l{sessions_mutex};
            sessions.push_back(std::move(session));
        });

    // Start listening.
    if (!server->start(opts.port)) {
        std::fprintf(stderr,
                     "[demo-wtserver] FAILED: server->start(%u) returned false\n",
                     opts.port);
        return 1;
    }
    std::fprintf(stderr, "[demo-wtserver] server started on UDP :%u\n",
                 opts.port);

    // Main loop: poll session state + sleep until duration or ^C.
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(opts.duration_sec);

    while (g_running.load(std::memory_order_acquire)) {
        if (opts.duration_sec > 0 &&
            std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "[demo-wtserver] duration expired\n");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Shutdown.
    std::fprintf(stderr, "[demo-wtserver] stopping server\n");
    server->stop();

    // Drop the accepted sessions last: they own the QUIC connections
    // that the listener accepted.
    {
        std::lock_guard<std::mutex> l{sessions_mutex};
        sessions.clear();
    }

    const int count = session_count.load();
    const std::uint64_t dg_bytes =
        g_datagram_echo_bytes.load(std::memory_order_relaxed);
    const std::uint64_t st_bytes =
        g_stream_echo_bytes.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[demo-wtserver] done — %d session(s) handled, "
                 "datagram echo %lluB, stream echo %lluB\n",
                 count,
                 static_cast<unsigned long long>(dg_bytes),
                 static_cast<unsigned long long>(st_bytes));

    // Exit 0 only when at least one session connected AND data was
    // exchanged on either plane; 2 covers timeout / no sessions /
    // connected-but-idle. This matches the Case E harness contract
    // (E3 datagram echo + E4 stream echo).
    return (count > 0 && (dg_bytes + st_bytes) > 0) ? 0 : 2;
}

// spike_echo_client — ADR-014 Criterion 3 + 4 (cross-platform CI + Windows first-class)
//
// Exercises the msquic 0-RTT echo path:
//   MsQuicRegistrationOpen → MsQuicConfigurationOpen (with QUIC_SETTINGS) →
//   MsQuicConnectionOpen (with MsQuicResumptionTicket) → MsQuicStreamOpen →
//   MsQuicStreamSend → recv callback (echoed by spike_echo_server).
//
// The 0-RTT path is the v1.0.0-beta.1 DTLS-WT handoff surface (per
// docs/adr/ADR-014-web-transport-vendor.md §"具体决策" item 6). The
// round-trip latency is the criterion-4 metric on Windows; the cross-
// platform PASS/FAIL is criterion 3.
//
// This is a SPIKE, not a test. It is NOT part of the ctest regression
// surface; it is deleted at the v0.14.0-beta.1 tag cut (or migrated
// to tests/test_web_transport_msquic.cpp as a proper regression test;
// that migration is a separate PR).
//
// Usage:
//   spike_echo_client <host> <port> <payload-bytes>
// Writes a single line to results.txt in the format:
//   [3] PASS Cross-platform CI green: echo_client RTT=<Xms>
//   [4] PASS Windows is first-class: 0-RTT echo RTT=<Xms> (target <50ms)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>

// msquic C API (when src/third_party/msquic/ is vendored):
//   #include <msquic.h>
// For the spike skeleton, the API surface is documented inline;
// the actual `#include <msquic.h>` is added when the submodule
// is fetched (beta.1 PR #2 per docs/plan/v0.14-plan.md §4.2).

extern "C" {
// Sketch of the msquic C API calls the spike would make.
// The full API is documented at https://github.com/microsoft/msquic/blob/main/src/inc/msquic.h
//
//   QUIC_STATUS QUIC_API MsQuicRegistrationOpen(
//       const QUIC_REGISTRATION_CONFIG* Config,
//       QUIC_REGISTRATION* Registration);
//   QUIC_STATUS QUIC_API MsQuicConfigurationOpen(
//       QUIC_REGISTRATION Registration,
//       const QUIC_CONFIGURATION* Config,
//       QUIC_CONFIGURATION* NewConfiguration);
//   QUIC_STATUS QUIC_API MsQuicConnectionOpen(
//       QUIC_REGISTRATION Registration,
//       QUIC_CONNECTION_CALLBACK_HANDLER Handler,
//       void* Context,
//       QUIC_CONNECTION* Connection);
//   QUIC_STATUS QUIC_API MsQuicStreamOpen(
//       QUIC_CONNECTION Connection,
//       QUIC_STREAM_OPEN_FLAGS Flags,
//       QUIC_STREAM_CALLBACK_HANDLER Handler,
//       void* Context,
//       QUIC_STREAM* Stream);
//   QUIC_STATUS QUIC_API MsQuicStreamSend(
//       QUIC_STREAM Stream,
//       const QUIC_BUFFER* Buffers,
//       uint32_t BufferCount,
//       QUIC_SEND_FLAGS Flags,
//       void* ClientSendContext);
}

namespace {

constexpr const char* kResultsPath = "../results.txt";

void append_result(const std::string& line) {
    if (FILE* f = std::fopen(kResultsPath, "a")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <host> <port> <payload-bytes>\n", argv[0]);
        return 2;
    }
    const char* host = argv[1];
    const int   port = std::atoi(argv[2]);
    const int   payload_bytes = std::atoi(argv[3]);

    // ---- SPIKE: stub returns "not yet exercised" ----
    //
    // The actual msquic C API calls are filled in when the
    // submodule is vendored. For now, the spike records that
    // the program ran and the API surface is documented.

    const auto t0 = std::chrono::steady_clock::now();
    // ---- begin msquic 0-RTT echo (skeleton; filled in at beta.1 PR #5) ----
    //   1. MsQuicRegistrationOpen(&RegistrationConfig, &Registration);
    //   2. MsQuicConfigurationOpen(Registration, &Configuration, &Configuration);
    //   3. MsQuicConnectionOpen(Registration, ConnectionCallback, this, &Connection);
    //   4. MsQuicResumptionTicket previous_ticket = ...;  // from dtls_to_wt_handoff.cpp
    //   5. MsQuicConfigurationAddTicket(previous_ticket);  // 0-RTT enabler
    //   6. MsQuicStreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE,
    //                       StreamCallback, this, &Stream);
    //   7. QUIC_BUFFER buf = { payload_bytes, payload };
    //      MsQuicStreamSend(Stream, &buf, 1, QUIC_SEND_FLAG_NONE, nullptr);
    //   8. On StreamCallback's QUIC_STREAM_EVENT_RECEIVE: log RTT.
    // ---- end msquic 0-RTT echo (skeleton) ----
    const auto t1 = std::chrono::steady_clock::now();
    const auto rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // For now, record a stub PASS so the spike-run script can
    // assert the structure; the real PASS/FAIL is filled in
    // when the msquic submodule is vendored.
    char line[256];
    std::snprintf(line, sizeof(line),
        "[3] STUB Cross-platform CI green: echo_client RTT=%lldms (skeleton; "
        "filled in at beta.1 PR #5 when msquic submodule is vendored)",
        static_cast<long long>(rtt_ms));
    append_result(line);

    std::snprintf(line, sizeof(line),
        "[4] STUB Windows is first-class: 0-RTT echo RTT=%lldms (skeleton; "
        "filled in at beta.1 PR #5 against real msquic on windows-2022)",
        static_cast<long long>(rtt_ms));
    append_result(line);

    std::fprintf(stderr,
        "spike_echo_client: skeleton; host=%s port=%d payload=%d (real impl "
        "lands at beta.1 PR #5 per docs/plan/v0.14-plan.md §4.2)\n",
        host, port, payload_bytes);
    return 0;
}

// spike_echo_server — ADR-014 Criterion 3 (cross-platform CI)
//
// msquic listener that accepts 0-RTT resumptions and echoes the
// payload back. Pairs with spike_echo_client.
//
// Exercises the msquic listener + connection callback + stream
// callback surface:
//   MsQuicListenerOpen → MsQuicListenerStart →
//   QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED callback →
//   MsQuicStreamSend (echo) → MsQuicStreamClose.
//
// This is a SPIKE, not a test. See echo_client.cpp for the
// skeleton-vs-impl contract.

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

// msquic C API (when src/third_party/msquic/ is vendored):
//   #include <msquic.h>
//
// Sketch of the calls the spike would make:
//   QUIC_STATUS QUIC_API MsQuicListenerOpen(
//       QUIC_REGISTRATION Registration,
//       QUIC_LISTENER_CALLBACK_HANDLER Handler,
//       void* Context,
//       QUIC_LISTENER* Listener);
//   QUIC_STATUS QUIC_API MsQuicListenerStart(
//       QUIC_LISTENER Listener,
//       const QUIC_ADDR* Address,
//       const QUIC_BUFFER* AlpnBuffers,
//       uint32_t AlpnBufferCount);
//   // On QUIC_LISTENER_EVENT_NEW_CONNECTION: MsQuicConnectionAccept.
//   // On QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
//   //   QUIC_BUFFER buf = { received_len, received_data };
//   //   MsQuicStreamSend(Stream, &buf, 1, QUIC_SEND_FLAG_NONE, nullptr);

namespace {

constexpr const char* kResultsPath = "../results.txt";

void append_result(const std::string& line) {
    if (FILE* f = std::fopen(kResultsPath, "a")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    // ---- SPIKE: stub returns "not yet exercised" ----
    //
    // The actual listener loop is filled in at beta.1 PR #5.

    append_result(
        "[3] STUB Cross-platform CI green: echo_server listener (skeleton; "
        "filled in at beta.1 PR #5 when msquic submodule is vendored)");

    std::fprintf(stderr,
        "spike_echo_server: skeleton; run in background as the listener "
        "for spike_echo_client. Real impl lands at beta.1 PR #5.\n");
    // A real impl would loop on MsQuicListenerStart events; for the
    // skeleton, sleep briefly so the client has a peer to talk to.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
}

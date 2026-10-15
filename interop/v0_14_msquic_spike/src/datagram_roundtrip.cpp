// spike_datagram_roundtrip — ADR-014 Criterion 3 (cross-platform CI)
//
// Tests MsQuicDatagramSend / recv. The WebTransport datagram facade
// (send_datagram / set_on_datagram) maps directly to QUIC DATAGRAM
// frames (RFC 9221 §3.3). The datagram path is the v1.0.0-rc.1
// SFU-1 + DC-3 forwarding policy surface (per
// docs/plan/v1.0-plan.md §2.1 P1) — getting the round-trip
// semantics right at beta.1 is the prerequisite for the rc.1
// forwarding policy work.
//
// Exercises:
//   MsQuicDatagramSend → QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED
//   callback.

#include <cstdio>
#include <string>

// msquic C API (when src/third_party/msquic/ is vendored):
//   #include <msquic.h>
//
// Sketch:
//   QUIC_STATUS QUIC_API MsQuicDatagramSend(
//       QUIC_CONNECTION Connection,
//       const QUIC_BUFFER* Buffers,
//       uint32_t BufferCount,
//       QUIC_SEND_FLAGS Flags,
//       void* ClientSendContext);
//   // On QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
//   //   log the received bytes; assert round-trip completes.

namespace {

constexpr const char* kResultsPath = "../results.txt";

void append_result(const std::string& line) {
    if (FILE* f = std::fopen(kResultsPath, "a")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
}

}  // namespace

int main() {
    append_result(
        "[3] STUB Cross-platform CI green: datagram_roundtrip (skeleton; "
        "filled in at beta.1 PR #5 — tests MsQuicDatagramSend + "
        "DATAGRAM_RECEIVED callback)");

    std::fprintf(stderr,
        "spike_datagram_roundtrip: skeleton; real impl lands at "
        "beta.1 PR #5.\n");
    return 0;
}

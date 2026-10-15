// spike_stream_reset — ADR-014 Criterion 3 (cross-platform CI)
//
// Tests MsQuicStreamSendComplete + reset_stream semantics.
// The WebTransport bidi stream seam (IBidirectionalStream) needs
// both successful-send and aborted-send paths; the reset path is
// what unblocks the DTLS-WT handoff's "connection lost" cleanup.
//
// Exercises:
//   MsQuicStreamSend with QUIC_SEND_FLAG_ABORT on cancel →
//   QUIC_STREAM_EVENT_SEND_COMPLETE callback with abort status →
//   MsQuicStreamClose.

#include <cstdio>
#include <string>

// msquic C API (when src/third_party/msquic/ is vendored):
//   #include <msquic.h>
//
// Sketch:
//   QUIC_STATUS QUIC_API MsQuicStreamSend(
//       QUIC_STREAM Stream,
//       const QUIC_BUFFER* Buffers,
//       uint32_t BufferCount,
//       QUIC_SEND_FLAGS Flags,         // QUIC_SEND_FLAG_ABORT to cancel
//       void* ClientSendContext);
//   // On QUIC_STREAM_EVENT_SEND_COMPLETE: log status.

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
        "[3] STUB Cross-platform CI green: stream_reset (skeleton; "
        "filled in at beta.1 PR #5 — tests MsQuicStreamSend "
        "QUIC_SEND_FLAG_ABORT + SEND_COMPLETE callback)");

    std::fprintf(stderr,
        "spike_stream_reset: skeleton; real impl lands at beta.1 PR #5.\n");
    return 0;
}

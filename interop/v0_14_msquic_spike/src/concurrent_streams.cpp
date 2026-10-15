// spike_concurrent_streams — ADR-014 Criterion 3 (cross-platform CI)
//
// Tests fanout of 16 concurrent bidi streams; send on each, recv
// on each, close all. The concurrent-stream-fanout test is the
// v1.0.0-rc.1 SFU-1 load-shape smoke (per docs/plan/v1.0-plan.md
// §2.1 P1) — a v1.0.0 stable "Stable" banner needs to demonstrate
// that the data plane handles a non-trivial number of concurrent
// streams without drops or stalls.
//
// Exercises:
//   MsQuicStreamOpen × 16 → concurrent MsQuicStreamSend × 16 →
//   recv callbacks × 16 → MsQuicStreamClose × 16.

#include <cstdio>
#include <string>

// msquic C API (when src/third_party/msquic/ is vendored):
//   #include <msquic.h>
//
// Sketch (in a loop, 16 iterations):
//   MsQuicStreamOpen(Connection, QUIC_STREAM_OPEN_FLAG_NONE,
//                     StreamCallback, this, &streams[i]);
//   QUIC_BUFFER buf = { payload_len, payload };
//   MsQuicStreamSend(streams[i], &buf, 1, QUIC_SEND_FLAG_NONE, nullptr);
// // On each StreamCallback's QUIC_STREAM_EVENT_RECEIVE:
// //   mark streams[i] as recv-done.
// // After all 16 recv-done:
//   for (i = 0; i < 16; ++i) MsQuicStreamClose(streams[i]);

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
        "[3] STUB Cross-platform CI green: concurrent_streams (skeleton; "
        "filled in at beta.1 PR #5 — tests 16-stream fanout: "
        "open × 16, send × 16, recv × 16, close × 16)");

    std::fprintf(stderr,
        "spike_concurrent_streams: skeleton; real impl lands at "
        "beta.1 PR #5.\n");
    return 0;
}

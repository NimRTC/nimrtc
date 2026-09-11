# examples/

This directory contains sample programs that demonstrate NimRTC's APIs.

Each example is self-contained and may depend on one or more NimRTC modules.
Examples are built only when `NIMRTC_BUILD_EXAMPLES=ON` is set at configure time.

## Current examples

| Example | Status | Description |
|---------|--------|-------------|
| `demo-p2p`     | P1 | Two-process end-to-end demo: spins up the signaling server, runs NimRTC as the offerer (or answerer) over a WebSocket signaling channel, captures microphone audio via the platform audio backend, encodes with libopus, and pushes RTP → SRTP (RFC 3711) → ICE → DTLS 1.2 (wolfSSL) → Opus decode → audio playback. Verifies the Chrome interop matrix on the NimRTC side. Expected console snippet:<br/><br/>`[engine] icestate=Connected  dtlsstate=Connected  srtpstate=Active  audiorecv=ok  rtp_pkts=4723  audio_drop=0` |
| `loopback-p2p` | P1 | Single-process self-loopback smoke test: two `NimRTCEngine` instances connect over `127.0.0.1` UDP and run a synthetic tone (no real microphone / speaker required) through the full media path so ICE, DTLS, and SRTP can all be exercised headlessly. Intended for CI and pre-merge confidence checks. Expected console snippet:<br/><br/>`[loopback-p2p] A ice=Connected srtp=Active  B ice=Connected srtp=Active  rtcp_rtt_ms=1.4  exit=0` |

## Adding an example

```cmake
# examples/CMakeLists.txt — add one line per example
nimrtc_add_example(example_name
    PATH    examples/example_name.cpp
    DEPS    nimrtc::rtp
    LIBS    nimrtc_vendor_libopus)
```

See `cmake/NimRTCTest.cmake` for the `nimrtc_add_example()` macro definition
(P1 when the first example lands).

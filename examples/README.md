# examples/

This directory contains sample programs that demonstrate NimRTC's APIs.

Each example is self-contained and may depend on one or more NimRTC modules.
Examples are built only when `NIMRTC_BUILD_EXAMPLES=ON` is set at configure time.

## Current examples

| Example | Status | Description |
|---------|--------|-------------|
| `demo-p2p`     | P1 | Two-process end-to-end demo: spins up the signaling server, runs NimRTC as the offerer (or answerer) over a WebSocket signaling channel, captures microphone audio via the platform audio backend, encodes with libopus, and pushes RTP → SRTP (RFC 3711) → ICE → DTLS 1.2 (wolfSSL) → Opus decode → audio playback. Verifies the Chrome interop matrix on the NimRTC side. Expected console snippet:<br/><br/>`[engine] icestate=Connected  dtlsstate=Connected  srtpstate=Active  audiorecv=ok  rtp_pkts=4723  audio_drop=0` |
| `loopback-p2p` | P1 | Single-process self-loopback smoke test: two `NimRTCEngine` instances connect over `127.0.0.1` UDP and run a synthetic tone (no real microphone / speaker required) through the full media path so ICE, DTLS, and SRTP can all be exercised headlessly. Intended for CI and pre-merge confidence checks. Expected console snippet:<br/><br/>`[loopback-p2p] A ice=Connected srtp=Active  B ice=Connected srtp=Active  rtcp_rtt_ms=1.4  exit=0` |
| `remote-desktop` | I2 | **Python, no CMake target** — the reference signalling service for the remote-desktop product (plan Track I, item I2): device registration with a short-lived pairing code, room join with a session credential, ICE candidate relay, and RFC 7635 style TURN credential issuance. Standard library only (the WebSocket layer is hand-rolled). The canonical description is [`docs/guides/remote-desktop.md`](../docs/guides/remote-desktop.md); plan RD-7 keeps the service out of `src/`. See [`remote-desktop/README.md`](remote-desktop/README.md). |
| `rd_device` / `rd_viewer` | G | The remote-desktop **client pair** (plan Track G): the device captures the desktop, encodes it and publishes it; the viewer receives it, renders it and sends the mouse / keyboard back over the control plane. Both speak newline-delimited JSON on stdin/stdout (the `rd_signalling.hpp` protocol); `python tools/rd_pair.py` runs the two against each other over a local relay, so a full session is exercised on one machine without a signalling server. |
| `rd_verify` | M1 | The **product-level sign-off program** for the M1 exit criterion. It drives the *whole* media chain in **one process** on **one real desktop** — the DXGI / WGC capture → the zero-copy GPU mailbox → the encoder → the RTP packetization → the depacketizer → the decoder → the sink — and prints a table whose most important line is `cpu bytes copied`. The bytes that cross its in-memory wire are the *same* RTP the engine produced, and they enter the receiving engine through the same `feed_media_inbound()` seam a real connection uses. `--no-zero-copy` measures the CPU fallback on the same machine. Run it on the machine whose hardware encoder you want to sign off. |

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

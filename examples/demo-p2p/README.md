# demo-p2p — NimRTC ↔ Chrome Interop Demo

## Overview

`demo-p2p` is a minimal end-to-end demonstration of NimRTC's WebRTC-compatible peer-to-peer audio connectivity. It generates SDP offers/answers and exchanges ICE candidates via WebSocket signaling, establishing a real-time audio connection.

## Build

```bash
# Requires NimRTC built with NIMRTC_BUILD_EXAMPLES=ON
cmake --build build --config Debug --target demo-p2p
```

Output: `build/examples/Debug/demo-p2p.exe`

## Usage

### File Mode (SDP exchange via files)

```bash
# Generate SDP offer (offerer mode)
demo-p2p > offer.sdp

# Process a remote offer and generate answer
demo-p2p offer remote_offer.sdp > answer.sdp

# Apply a remote answer
demo-p2p answer remote_answer.sdp
```

### Signaling Proxy Mode (WebSocket interop)

For interop with Chrome or other WebRTC peers, use the signaling proxy:

```bash
# Start signaling server (in one terminal)
python interop/signaling/signaling_server.py --port 8765

# Run offerer demo-p2p (in another terminal)
demo-p2p --signaling-proxy --duration 30

# Run answerer demo-p2p (in third terminal)
demo-p2p --signaling-proxy --answerer --duration 30
```

Or use the Python proxy harness:

```bash
# Start signaling server
python interop/signaling/signaling_server.py --port 8765

# Run Chrome as offerer
python interop/chrome/test_chrome_opus.html?room=interop&ws=ws://localhost:8765/interop&offerer=1

# Run NimRTC as answerer via proxy
python interop/signaling/signaling_proxy.py \
    --signaling ws://localhost:8765/interop \
    --binary build/examples/Debug/demo-p2p.exe \
    --answerer
```

## SDP Format

`demo-p2p` generates RFC 8829 compatible SDP with:

- **Opus codec**: Payload type 111, 48 kHz stereo (when NIMRTC_HAS_OPUS is defined)
- **PCMU fallback**: Payload type 0, 8 kHz mono (always available)
- **ICE**: Host candidates on all interfaces, STUN candidate from stun.l.google.com:19302
- **DTLS**: SHA-256 fingerprint, setup:actpass
- **RTCP-mux**: Enabled

## Chrome Interop

The `interop/chrome/test_chrome_opus.html` page provides a headless test harness that:

1. Connects to the signaling server WebSocket
2. Receives SDP offer from NimRTC
3. Creates and sends SDP answer
4. Exchanges ICE candidates
5. Verifies ICE connectivity
6. Reports RTP packet reception

Run with Chrome headless:

```bash
chrome --headless=new \
       --virtual-time-budget=10000 \
       --use-fake-ui-for-media-stream \
       --use-fake-device-for-media-stream \
       "file:///$(pwd)/interop/chrome/test_chrome_opus.html?room=interop&ws=ws://localhost:18765/interop"
```

## Signaling Protocol

Messages are JSON text frames over WebSocket:

```json
{"type": "offer", "sdp": "v=0\r\no=- ..."}
{"type": "answer", "sdp": "v=0\r\no=- ..."}
{"type": "candidate", "candidate": "candidate:1 1 UDP ..."}
```

The signaling server (`signaling_server.py`) relays messages between peers in the same room.

## Limitations

- **Audio direction**: Chrome ↔ NimRTC is tested as unidirectional (NimRTC sends, Chrome receives)
- **Microphone access**: demo-p2p does not capture microphone audio; it's a test harness
- **DataChannel**: Not implemented in P1
- **Video**: Not implemented in P1

## TODO (P1 open items)

- [ ] Full bidirectional audio (Chrome → NimRTC direction)
- [ ] Microphone capture integration
- [ ] RTCP statistics reporting
- [ ] DataChannel support
- [ ] Video support

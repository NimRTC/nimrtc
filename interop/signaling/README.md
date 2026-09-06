# interop/signaling/

Minimal WebSocket signaling server for SDP exchange between NimRTC and Chrome.

## What it does

```
NimRTC (demo-p2p)  <---WS--->  signaling server  <---WS--->  Chrome (test page)
     |                              |
     |--- offer SDP ---------------->|
     |<-- answer SDP ----------------|
     |                              |
     |<--- ICE candidates ----------->|
```

Chrome and NimRTC exchange SDP/ICE via this server; the server does NOT
process media — it only relays JSON messages.

## Message format (WebSocket text frames)

### Chrome → NimRTC (forwarded as-is)

```json
{"type": "offer",    "sdp": "v=0\r\no=- ..."}
{"type": "answer",   "sdp": "v=0\r\no=- ..."}
{"type": "candidate", "candidate": "candidate:1 1 UDP ..."}
```

### NimRTC → Chrome (forwarded as-is)

```json
{"type": "offer",    "sdp": "v=0\r\no=- ..."}
{"type": "answer",   "sdp": "v=0\r\no=- ..."}
{"type": "candidate", "candidate": "candidate:1 1 UDP ..."}
```

## Usage

```bash
pip install websockets  # or: pip install -r ../requirements.txt
python signaling_server.py [--port 8765]
```

## Protocol state machine

```
Chrome (offerer)          Server              NimRTC (answerer)
      |                       |                       |
      |--- connect WS ------->|                       |
      |                       |<--- connect WS -------|
      |                       |                       |
      |<========= room-id exchange (future) ========>|
      |                       |                       |
      |--- {type:offer} ----->|--- {type:offer} ---->|
      |                       |                       |
      |<-- {type:answer} -----|<-- {type:answer} -----|
      |                       |                       |
      |--- {type:candidate} ->|--- {type:candidate} ->|
      |<-- {type:candidate} -|<-- {type:candidate} --|
      |                       |                       |
      |=========== ICE Connected (direct) ==========>|
      |                       |                       |
      |<=========== RTP audio (direct, no server) ===>|
```

Both peers discover each other's public IP:port via STUN/TURN,
then speak RTP directly — the signaling server is only used for SDP/ICE.

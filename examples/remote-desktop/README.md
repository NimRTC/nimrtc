# examples/remote-desktop — the reference signalling service

> **This is a reference implementation, not a production server.**
> Plan RD-7 keeps the signalling server out of `src/`: NimRTC ships a
> signalling *seam* (`create_offer()` / `process_remote_sdp()` /
> `add_turn_server()` / candidate forwarding), and the room management,
> the pairing and the TURN credential issuance are product concerns.
>
> The canonical description of the exchange is
> [`docs/guides/remote-desktop.md`](../../docs/guides/remote-desktop.md).
> This directory is its executable form.

## What is here

| File | What it is |
|---|---|
| `signalling.py` | The asyncio WebSocket server. |
| `signalling_core.py` | The pure logic: the pairing store, the TURN credential issuer, the message router. No I/O. |
| `wsproto.py` | A hand-rolled RFC 6455 WebSocket layer — **standard library only**, no `pip install` needed. |
| `client.py` | The example client. Bridges the signalling channel to the `demo-p2p` media binary. |
| `test_signalling.py` | The self-running test. |

## Dependencies

Python 3 standard library only. Nothing to install.

## Running it

```bash
# Terminal 1 — the service.
python examples/remote-desktop/signalling.py --host 127.0.0.1 --port 8765

# Terminal 2 — the device.  It prints the pairing code.
python examples/remote-desktop/client.py --role device --url ws://127.0.0.1:8765

# Terminal 3 — the operator.  It types the pairing code.
python examples/remote-desktop/client.py --role viewer --code 123456 \
    --url ws://127.0.0.1:8765
```

The **viewer** is the offerer and the **device** answers — see
[`docs/guides/remote-desktop.md` §2](../../docs/guides/remote-desktop.md)
for why.

## The test

```bash
python examples/remote-desktop/test_signalling.py
```

It covers the pairing-code lifecycle (issue / validate / expiry /
one-shot), the RFC 7635 TURN credential (issuance / validation /
expiry / tamper detection), the message routing (pairing, room join,
candidate relay, ICE restart, reconnect, TURN issuance, the error
paths), and a live WebSocket round trip through the hand-rolled RFC
6455 layer. The script prints one line per check and exits non-zero on
failure.

## CMake

**There is no CMake target.** The service is Python; the examples'
CMake targets are all C++ binaries. The `demo-p2p` binary that
`client.py` drives is built by the ordinary `examples/CMakeLists.txt`
target and needs `NIMRTC_BUILD_EXAMPLES=ON`.

## The media plane

`client.py` bridges the signalling channel to the existing
`examples/demo-p2p` binary, which already speaks the same message
shapes (`offer` / `answer` / `candidate`) over newline-delimited JSON
on stdin/stdout. Build it with:

```bash
cmake -S . -B build/msvc-full -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DNIMRTC_BUILD_EXAMPLES=ON
.\scripts\msvc_env.ps1 -Command "cmake --build build\msvc-full -j 4"
```

When the binary cannot be found, the clients still complete the whole
signalling flow — pairing, join, TURN issuance, candidate relay — and
report `media plane skipped`.

## What a product has to add

- TLS.
- Authentication of the service itself.
- Rate limiting on the pairing code (it is a six-digit secret).
- Persistence of the credential store.
- Session limits (the plan's item I6): maximum concurrent sessions,
  session timeouts, idle disconnect, an administrative kill switch.
- A second signalling transport for the browser case, if needed.

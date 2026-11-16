# LAN Runbook — Two Machines, One Remote-Desktop Session

> **Audience**: an engineer running a remote-desktop session across **two
> computers on the same private network** (a device machine and an
> operator's machine).
>
> **Status**: v1.1.1. Verified end-to-end on 2026-11-16 on Windows 11 +
> RTX 3060: the media plane delivered `decoded=25 rendered=25 dropped=0`
> and the control plane carried the heartbeat in both directions.
>
> **Scope**: how to build, wire up and *judge* a two-machine run. The
> signalling protocol itself is described in
> [`remote-desktop.md`](remote-desktop.md); this document is the
> operational procedure.

---

## 0. The short version

```bash
# --- On BOTH machines, from the same source tree -------------------------
python tools/rd_lan.py --media rd --duration 60 --backend hardware
```

That single command is the **single-machine** shape: the signalling
service, the device client and the viewer client all run on one host and
talk to each other over `127.0.0.1`. It exercises the real WebSocket
relay, the real ICE/DTLS/SRTP handshake and the real capture → encode →
decode → render path — only the *wire between the machines* is loopback.

For the **genuine two-machine run** the same pieces are started by hand
on each machine; §4 has the exact commands.

**A run is a pass when the viewer reports `decoded > 0` and
`rendered > 0`.** Everything else (the exit codes, the silence of the
logs) is noise — see §6.

---

## 1. What has to be true before you start

### 1.1 The hardware

| Plane | Requirement | Notes |
|---|---|---|
| Media (capture) | Windows 8.1+ | `windows-wgc` (Windows Graphics Capture) is the default; `windows-dxgi` is the fallback. |
| Media (encode) | A hardware H.264 encoder, or the vendored OpenH264 | NVENC on NVIDIA, AMF on AMD, QSV on Intel. Without one, OpenH264 runs the CPU path. |
| Media (decode) | A hardware H.264 decoder, or OpenH264 | NVDEC on NVIDIA. |
| Control | `SendInput` rights | The device must run in a session that may synthesize input. |

Both machines need the **same codec** — the engine checks the peer's
`a=rtpmap:` against its own and refuses a session whose video family
does not match (§4.4).

### 1.2 The network

The two machines must be able to reach each other's **host candidates**
directly. In practice:

- Same IPv4 subnet, or a routed path between the two subnets.
- **No** client-side isolation on the Wi-Fi AP ("AP isolation" / "client
  isolation" breaks the peer-to-peer path on many office networks).
- The firewall must allow **UDP** in both directions. ICE uses an
  ephemeral port per candidate; the RTP/DTLS/SRTP media flows over the
  *same* port pair as ICE (RFC 8838 style), so it is the ICE port that
  has to be reachable.
- There is **no** STUN or TURN server in the LAN setup (§3.3).

### 1.3 The source tree

Both machines need the **same source tree and the same build**, because
the two binaries exchange an SDP that names the codec family, the
payload type and the ICE setup. A mismatch shows up as a session that
connects but never renders.

---

## 2. Build

The remote-desktop binaries are `rd_device` (the machine whose screen is
shared) and `rd_viewer` (the operator's client).

```bash
# Ninja + MSVC on Windows
cmake -S . -B build/msvc-full -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DNIMRTC_PLUGINS_NVENC=ON \
      -DNIMRTC_PLUGINS_NVDEC=ON \
      -DNIMRTC_PLUGINS_OPENH264=ON
cmake --build build/msvc-full
```

The three plugin switches are what turn the **hardware** path on:

| Switch | Effect | Without it |
|---|---|---|
| `NIMRTC_PLUGINS_NVENC=ON` | The NVIDIA hardware **encoder**. | `--backend hardware` fails with *"no hardware H.264 encoder is available"*; only the software path remains. |
| `NIMRTC_PLUGINS_NVDEC=ON` | The NVIDIA hardware **decoder**. | The CPU decode path is used (slower, but it works). |
| `NIMRTC_PLUGINS_OPENH264=ON` | The vendored **software** codec. | The synthetic `software-stub` bitstream is used, which is *not* a video codec — the peer will not be able to decode it. |

> **The `--backend hardware` policy never falls back silently.** If the
> host asked for hardware and there is none, the engine *fails* rather
> than quietly running the CPU path — a silent fallback would hand the
> host a 1080p30 frame budget it cannot meet, without saying so. So a
> `--backend hardware` run on a tree without NVENC/NVDEC fails loudly
> (see §6.2).

The binaries land in `build/msvc-full/examples/rd_device.exe` and
`build/msvc-full/examples/rd_viewer.exe`.

---

## 3. The three planes

### 3.1 The signalling plane

The reference service in
[`examples/remote-desktop/`](../../examples/remote-desktop/) is a
WebSocket relay. It hands out the **pairing code**, relays the
SDP/candidate messages verbatim, and issues a TURN credential.

```bash
python examples/remote-desktop/signalling.py --host 0.0.0.0 --port 8765
```

`--host 0.0.0.0` matters for a two-machine run: the default
`127.0.0.1` binds the loopback interface only, and the operator's
machine cannot reach it.

### 3.2 The media plane

`rd_device` captures the screen (WGC → NVENC → RTP → SRTP);
`rd_viewer` receives it (SRTP → RTP → NVDEC → sink). The first frame
that reaches the peer must be an **IDR carrying the parameter sets** —
§7 explains why, and what was fixed to guarantee it.

### 3.3 The control plane

The input events ride a **DataChannel** (`control-channel`), separately
from the media. The viewer sends the operator's mouse/keyboard plus a
**liveness heartbeat**; the device's dead-man watchdog counts the
beats and releases every held key when they stop.

> **LAN mode: no STUN, no TURN.** Pass `--no-stun` to both clients. In
> a private network a server-reflexive candidate is not merely useless —
> it *competes* with the host candidate in the pair selection, and the
> session breaks the moment the NAT mapping moves. TURN is likewise
> unnecessary (and the reference service's default TURN URL points at a
> server that is usually not running).

---

## 4. The genuine two-machine run

### 4.1 The device machine

```bash
# Terminal 1 — the signalling service (only ONE machine needs to run it;
# in the two-machine setup the device machine usually hosts it).
python examples/remote-desktop/signalling.py --host 0.0.0.0 --port 8765

# Terminal 2 — the device.  It prints the pairing code.
python examples/remote-desktop/client.py \
    --role device \
    --url ws://<device-lan-ip>:8765 \
    --media rd \
    --duration 600 \
    --backend hardware \
    --no-stun
```

`--duration` is in seconds; `0` means "forever". The device is the
**answerer** — that is fixed by the binary, no flag needed.

### 4.2 The operator's machine

```bash
# The pairing code the device printed.
python examples/remote-desktop/client.py \
    --role viewer \
    --url ws://<device-lan-ip>:8765 \
    --code 123456 \
    --media rd \
    --duration 600 \
    --backend hardware \
    --no-stun
```

The viewer is the **offerer**. `--sink d3d11` renders into a window
instead of the headless sink.

### 4.3 What you should see

On the device:

```
[device] connected to ws://192.168.1.20:8765
[device] pairing code: 400683 (valid for 300 s)
[device] joined room=room-8324e16f87b47a6f as device (peer_present=False)
[device] the viewer joined
[media] started ...\rd_device.exe (answerer=True)
[info] nvenc: H.264 / AVC encoder initialized 1920x1080 @30 fps, 1000000 bps, preset P4
[info] h264: selected encoder backend 'nvenc_h264' (nvenc, hw=yes, zero_copy=yes)
[info] engine[dtls→srtp]: SRTP keys INSTALLED ✓ (state=Open)
[info] engine[dtls→srtp]: armed the video encoder for an IDR — the first frame on the wire will carry the parameter sets
[rd_device] capture=19 pumped=19 ctrl_in=39 ctrl_sent=0 injected=0 denied=0 refused=0
```

On the viewer:

```
[info] nvdec: CUDA context ready (device 0, CC 8.x)
[info] h264: selected decoder backend 'nvdec_h264' (nvdec)
[rd_viewer] ready (sink=headless codec=h264 control=control-channel)
[info] engine[video]: negotiated payload type 102 (encoding H264)
[info] nvdec: decoder created for 1920x1088 (display 1920x1080)
[rd_viewer] frames=25 decoded=25 rendered=25 dropped=0 events_sent=0 denied=0 beats=39 ctrl_out=39
```

### 4.4 The three numbers that matter

| Field | Meaning | A pass looks like |
|---|---|---|
| `decoded` | Frames the decoder turned into pictures. | `> 0` — and growing. |
| `rendered` | Pictures that reached the sink. | `> 0`, and tracking `decoded`. |
| `dropped` | Frames the decoder threw away. | Small; a rising count means the decoder is missing references. |

`beats` / `ctrl_out` / `ctrl_in` are the **control plane**: the heartbeat
count and the DataChannel message counters in each direction. `beats`
growing means the dead-man watchdog on the device is being kept quiet.

### 4.5 The packaged bundle (no build toolchain on the operator's machine)

§4.1 and §4.2 need a Python 3 and, on the device machine, a source
tree. When the operator's machine must not carry a toolchain, ship the
**deployment bundle** instead: `build/rd-deploy.zip` (built by
`scripts/package_rd_deploy.ps1`, refreshed whenever the binaries
change).

The zip contains `rd_device.exe`, `rd_viewer.exe`, `msquic.dll`, the
MSVC runtime under `crt/`, the four Python files (`client.py`,
`wsproto.py`, `signalling.py`, `signalling_core.py`), the two `.bat`
entry points and a `README.md`.

**Device machine** (unzip anywhere, then two terminals):

```bat
python signalling.py --host 0.0.0.0 --port 8765

rd_device.bat --url ws://127.0.0.1:8765 --duration 3600 --backend hardware --no-stun
```

**Operator's machine** (unzip anywhere, then one terminal):

```bat
rd_viewer.bat --url ws://<device-lan-ip>:8765 --code <pairing code> ^
               --duration 3600 --backend hardware --no-stun --sink d3d11
```

The `.bat` wrappers put `crt/` on the DLL search path, so **no
"VC++ redistributable" install is needed**. `msquic.dll` sits next to
the executables; without it the viewer dies with `0xc000007b`.

Two rules the operator must respect:

- **The `--url` on the operator's machine is the device's LAN address**,
  never `127.0.0.1`.
- **The pairing code is one-shot.** After the operator disconnects, the
  device prints a *fresh* code — use the newest one. Reusing a spent
  code yields `pairing_invalid`.

Two more that silently break a run:

- **The device's console session must stay unlocked.** Windows locks
  the screen when it is idle; a locked session starts `LogonUI.exe` and
  the capture then either stops after one frame (WGC) or is refused
  outright (DXGI reports `E_ACCESSDENIED`). Turn off the screen saver
  and the automatic lock, or keep the session active. The device now
  prints the DXGI reason, so a locked screen says so instead of
  looking like a broken capture.
- **The device needs `--inject` for the operator's input to reach the
  desktop.** The device does not inject input events by default —
  injecting real mouse and keyboard events into the desktop is not
  something a test harness should do by surprise. Without `--inject`
  the control channel still runs, but the events are dropped
  (`injected=0`).

The operator's machine needs **Python 3** (the `.bat` invokes
`python`); everything else is self-contained.

---

## 5. The self-running harness

`tools/rd_lan.py` drives the whole thing on **one** machine and prints a
verdict:

```bash
python tools/rd_lan.py --media rd --duration 30 --backend hardware
```

To exercise the **cross-network shape** on one host — the signalling
service bound to the LAN address and the clients connecting to it over
that address instead of the loopback — add `--bind`:

```bash
python tools/rd_lan.py --media rd --duration 30 --backend hardware \
    --no-stun --bind 192.168.1.20
```

| Flag | Meaning |
|---|---|
| `--duration <s>` | The media session's lifetime in seconds. |
| `--port <p>` | The signalling service's port. |
| `--backend auto\|hardware\|software` | The codec backend class. |
| `--no-stun` | LAN mode: host candidates only. |
| `--bind <addr>` | The address the signalling service binds to, and the address the clients connect to. The default is `127.0.0.1`; use the **LAN address** to exercise the cross-network shape instead of the loopback. |
| `--binary <path>` | An explicit path to the media binaries (the directory or either binary; the right one is picked per role). |
| `--inject` | Let the device inject the received input into its own desktop (**off** by default — it moves the operator's mouse on the device machine). |
| `--sink <name>` | The viewer's renderer. |
| `--capture <name>` | The device's capture backend. |

The harness ends the run when the **media session** ends (not when the
signalling clients exit — they keep the WebSocket open), and then
prints:

```
======================================================================
rc device=0 viewer=0
media plane: decoded=25 rendered=25 dropped=0
OK: the media plane delivered and rendered frames
```

A **non-zero exit code** means the run failed *and* the logs were
dumped. The logs live in `build/lan_device.log` and
`build/lan_viewer.log`.

### 5.1 A note on the binary the harness picks

The harness searches the build trees and takes the **most recently
modified** binary. That is deliberate: this repository keeps several
build trees alive at once (`build/msvc-full`, `build/ci-msvc`,
`build/zcs`, …) and a stale binary in one of them will connect,
negotiate and then fail in confusing ways — a media plane that never
decodes a frame because the fix for it landed in a *different* tree. Pass
`--binary` to pin an explicit path when you need to.

---

## 6. Reading a failure

### 6.1 `decoded=0, dropped=N`

**The symptom**: the session connects, the handshake completes, the
device encodes — and the viewer never shows a picture.

**The cause** (this was the bug fixed on 2026-11-16): the encoder
started capturing *before* ICE and DTLS were ready, so the first IDR —
the one carrying the SPS/PPS — was produced when there was no SRTP key
to protect it and was dropped by libjuice (`Send while ICE is not
connected`). Every frame after it was a P-frame. The decoder never saw
a parameter set, never created its decoder context, and dropped every
frame it was handed until the *next* IDR came around — which, on a
static desktop at 2–4 fps with a 60-frame GOP, is 15–30 seconds away.

**The fix**: the engine arms the encoder for a fresh IDR at the moment
the SRTP keys are installed, so the first frame that actually reaches
the peer carries the parameter sets. The decoder side also asks for a
key frame (RTCP PLI) when a *complete* frame fails to decode, which
covers the reference-frame case.

**If you still see it**: check that the log contains

```
engine[dtls→srtp]: armed the video encoder for an IDR
```

If it does not, the binary is stale — see §5.1.

### 6.2 `no hardware H.264 encoder is available`

The build tree does not have NVENC (or the host has no supported GPU).
Rebuild with `NIMRTC_PLUGINS_NVENC=ON` (§2), or run with
`--backend software` to use the vendored OpenH264.

### 6.3 The session connects but nothing is rendered

Check, in this order:

1. `h264: selected decoder backend 'nvdec_h264'` appears in the
   **viewer's** log. If the viewer says `software-stub`, the peer is
   not sending real H.264.
2. Both sides report the **same** codec family and payload type
   (`engine[video]: negotiated payload type 102 (encoding H264)` on the
   viewer).
3. The device's log shows frames being *pumped*
   (`capture=N pumped=N` — the two counters must track).

### 6.3a The device captures one frame and then stops

`capture` sticks at 1 (or stops growing while `pumped` follows it).

**The most common cause is a locked console session.** Windows starts
`LogonUI.exe` when the screen locks, which switches the active input
desktop to Winlogon; a WGC capture then produces a single static frame
and no more, and a DXGI `DuplicateOutput` call is refused with
`E_ACCESSDENIED`.

Turn off the screen saver and the automatic lock, or keep the session
active. `tsdiscon 1` followed by `tscon 1 /dest:console` clears a
locked session without killing the running processes.

The device now names the reason in its log, so the message tells you
which case you are in:

| The message says | What it means |
|---|---|
| `access denied ... LOCKED workstation` | The console session is locked |
| `another program already holds the desktop-duplication slot` | A screen recorder or another remote-desktop tool owns the output; close it |
| `this display does not support desktop duplication` | A remote or virtual display; use the WGC capture instead |

### 6.4 ICE never connects

- Is the signalling service bound to `0.0.0.0`? (§3.1)
- Can the operator's machine `ping` the device's LAN address?
- Is UDP blocked? ICE uses an ephemeral UDP port; SRTP media flows over
  the *same* pair.
- Is AP/client isolation on? It silently breaks the peer-to-peer path.
- Did you forget `--no-stun` on one side? A public STUN server is
  usually unreachable from a private network and the reflexive candidate
  it yields can out-prioritise the host candidate.

### 6.5 The control plane is silent

`ctrl_in` stays 0 while `beats` grows: the heartbeat rides the control
channel, so a dead control channel kills both. Check that both clients
were started with the **same** `--no-control` setting (both on, or both
off) and that the DataChannel was created on both sides.

---

## 7. Why the first frame must be an IDR

This is the one piece of protocol knowledge a LAN tester needs, so it is
spelled out.

H.264 streams carry **parameter sets** (SPS/PPS) out of band. A decoder
that has never seen them cannot create its decoder context at all — it
does not matter how many correct P-frames arrive; it drops every one of
them. The parameter sets travel with an **IDR** frame, and only with it.

So the rule is: *the first frame that reaches the peer must be an IDR.*

The engine guarantees this by arming the encoder at the moment the SRTP
keys are installed:

```cpp
// src/engine/src/engine_transport.cpp — maybe_install_srtp_keys()
if (video_codec_) {
    (void)video_codec_->force_keyframe();
    log.info("engine[dtls→srtp]: armed the video encoder for an IDR — "
             "the first frame on the wire will carry the parameter sets");
}
```

The decoder side is the mirror image: when a *complete* frame fails to
decode (a missing reference, or a context that was never created), the
receiver asks the sender for a key frame over RTCP PLI — rate-limited
to 250 ms so a broken stream cannot turn into a PLI storm.

---

## 8. Checklist

- [ ] Both machines run the **same** build.
- [ ] The hardware codecs are compiled in (`NIMRTC_PLUGINS_NVENC=ON`,
      `NIMRTC_PLUGINS_NVDEC=ON`).
- [ ] The signalling service is bound to `0.0.0.0`.
- [ ] Both clients were given `--no-stun`.
- [ ] The operator can `ping` the device's LAN address.
- [ ] The device's console session is **unlocked** (no screen saver, no
      automatic lock) — a locked session stops the capture (§6.3a).
- [ ] The device's log shows `armed the video encoder for an IDR`.
- [ ] The viewer's log shows `decoded` growing and `rendered` tracking it.
- [ ] The viewer's log shows `beats` growing (the control plane is alive).
- [ ] If the operator should control the device's desktop, the device
      was started with `--inject` — otherwise the events are dropped.

---

## 9. See also

- [`remote-desktop.md`](remote-desktop.md) — the signalling contract.
- [`examples/remote-desktop/`](../../examples/remote-desktop/) — the
  reference service and the two clients.
- [`tools/rd_lan.py`](../../tools/rd_lan.py) — the self-running harness.
- [`docs/plan/remote-desktop-plan.md`](../plan/remote-desktop-plan.md) —
  the plan the whole feature is tracked in.

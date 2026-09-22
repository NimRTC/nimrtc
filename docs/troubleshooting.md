# NimRTC Troubleshooting Guide

> **Audience**: integrators and contributors running into problems
> with NimRTC. Most of these entries come from issues we've
> debugged ourselves and want to spare the next person.
>
> **If you have an issue not covered here**, search the GitHub
> Discussions and Issues first; if nothing matches, open an issue
> with a [BUG] / [INTEROP] / [DOCS] prefix as appropriate.

---

## 1. Build & toolchain

### 1.1 "Could not find Ninja"

Ninja is recommended but not strictly required. CMake falls back
to Makefiles or Visual Studio projects without it.

| OS | Fix |
|---|---|
| Ubuntu / Debian | `sudo apt install ninja-build` |
| Fedora / RHEL | `sudo dnf install ninja-build` |
| macOS | `brew install ninja` |
| Windows (Chocolatey) | `choco install ninja` |
| Windows (winget) | `winget install Ninja-build.Ninja` |

### 1.2 MSVC errors "MSB8020" / "v143 not found"

You're in a non-VS-2022 Developer Prompt. Open **"x64 Native Tools
Command Prompt for VS 2022"** and reconfigure.

```bat
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset debug.msvc
```

### 1.3 GCC too old for `-std=c++20`

| Distro | Default GCC | Upgrade |
|---|---|---|
| Ubuntu 20.04 / Debian 11 | GCC 9–10 | `sudo apt install gcc-11 g++-11 && export CC=gcc-11 CXX=g++-11` |
| Ubuntu 22.04 / Debian 12 | GCC 11–12 | ✅ Use as-is |
| Fedora 36+ | GCC 12+ | ✅ Use as-is |
| RHEL / Rocky 8 | GCC 8 | `sudo dnf install gcc-toolset-11 && source /opt/rh/gcc-toolset-11/enable` |
| CentOS 7 | GCC 4.8 | ❌ Not supported — upgrade OS or use a container |
| Arch Linux | GCC 13+ | ✅ Use as-is |
| macOS | Apple Clang | Xcode ≥ 15 required (`clang++ --version`) |

### 1.4 "WebRTC APM pre-built library not found"

The vendored WebRTC APM only has prebuilts for x86_64 targets. On
aarch64 or when cross-compiling, either skip 3A or fetch the
prebuilt:

```bash
# Skip 3A entirely (raw PCM passthrough):
cmake --preset dev -DNIMRTC_VENDORED_WEBRTC_APM=OFF

# Fetch prebuilt for current arch:
python tools/fetch_webrtc_apm.py
cmake --build build
```

The engine falls back to `NullAudio3A` when the prebuilt is missing
(but `NIMRTC_VENDORED_WEBRTC_APM=ON`); no further config change is
required to compile.

### 1.5 Linker errors / "undefined reference"

1. **Submodules not initialised** — `git submodule update --init --recursive`
2. **Compiler < GCC 11** — see §1.3.
3. **Stale build cache** — `rm -rf build && cmake --preset dev && cmake --build build -j`
4. **Wrong generator** — if you configured with `--preset release.msvc` (multi-config)
   but call `cmake --build build` without `--config Release`, you get stale
   outputs. Either pass `--config Release` or reconfigure with
   `--preset release` (single-config).

### 1.6 `OPENSSL_VERSION_NUMBER` mismatch on GMSSL build

```text
error: 'OPENSSL_VERSION_NUMBER' was not declared in this scope
```

GMSSL exports OpenSSL-symbols but may not expose the macro the way
your code expects. Add:

```cmake
target_compile_definitions(nimrtc_dtls_seam PRIVATE
    NIMRTC_GMSSL_VERSION_OVERRIDE=1
    OPENSSL_NO_PSK=1)
```

Or use `target_compile_features(... cxx_std_20)` only and avoid
OpenSSL macros.

### 1.7 "undefined symbol: nimrtc::core::register_all_default_plugins"

You're not linking one of `nimrtc_core`, `nimrtc_engine`, etc. Add
to your target:

```cmake
target_link_libraries(your_app PRIVATE
    nimrtc::engine)
```

`register_all_default_plugins` lives in the core static library.

---

## 2. Compile-time issues

### 2.1 `static_cast` ambiguity in HW plugin code

```text
error: 'static_cast': ambiguous conversion
```

Multiple base classes of `IHwVideoEncoder` (`IVideoSender` + the
HW base) export methods of the same name. Resolve with explicit
upcast:

```cpp
static_cast<nimrtc::plugins::IHwVideoEncoder*>(encoder)->backend();
```

### 2.2 "no matching function for call to 'IDtlsSession::open'"

You forgot to override both `open()` overloads — the seam exposes
two `open` methods with subtly different signatures (one returns
`void` matching `start()`, the other returns `core::Result<void>`
matching `DtlsSession::open()`). Override **both**:

```cpp
void open() noexcept override { /* ... */ }
nimrtc::core::Result<void> open() noexcept override { /* ... */ }
```

Both must be implemented or the build fails (multiple
`override`-matchable candidates collide).

### 2.3 "use of deleted function" when constructing EngineConfig

`EngineConfig` uses `std::string_view` for plugin ids. Assigning
from a `const char*` is fine; from an `std::string`, you must take
`.c_str()` or rely on the `std::string_view` constructor:

```cpp
cfg.dtls_name = "gmssl";                                  // OK
cfg.dtls_name = std::string{"gmssl"};                    // OK (implicit conversion)
cfg.dtls_name = std::string_view{"gmssl"};               // OK
cfg.dtls_name = "gmssl"sv;                               // OK (string_view literal)
```

`dtls_name` is `std::string` (not `string_view`) because empty
means "default"; length is significant.

---

## 3. Runtime issues

### 3.1 Engine stuck in `ice_state_string() != "connected"` forever

Common causes (in order of likelihood):

1. **Both engines on the same host, same port range** — each takes
   the same UDP port and processes the other's STUN. Pick
   non-overlapping ranges:
   ```cpp
   A.cfg.local_port_range_begin = 51000;  A.cfg.local_port_range_end = 51099;
   B.cfg.local_port_range_begin = 51100;  B.cfg.local_port_range_end = 51199;
   ```

2. **Answerer doesn't know offerer's ICE ufrag/pwd** — call
   `set_remote_ice(...)` on the answerer before `open()`:
   ```cpp
   offerer.pre_open();
   answerer.pre_open();
   answerer.set_remote_ice(extract_ice_block(offerer.create_offer()));
   offerer.open();
   answerer.open();
   ```

3. **Firewall** — drop iptables / Windows Defender rules blocking
   UDP for testing:
   ```bash
   sudo iptables -A INPUT -p udp -j ACCEPT   # demo only
   ```

4. **`tick()` is missing or too slow** — call at 50 ms cadence
   minimum; the DTLS pump retries depend on it.

5. **No STUN server and both hosts are symmetric NATs** — add TURN:
   ```cpp
   engine.add_turn_server("turn.example.com", 3478, "user", "pass");
   ```

### 3.2 `dtls_state` stuck at `HelloSent` / `HelloReceived`

DTLS handshake didn't complete. Most common causes:

1. **Cert isn't loading**. Look for `dtls_open_failed` in the
   error log. Verify `tests/certs/server-ecc.pem` and
   `ecc-key.pem` are present and readable.
2. **Cipher suite mismatch**. The default Western suite is what
   Chrome / Firefox advertise. If your backend compiles with a
   narrower set, the handshake will silently fail.
3. **Missing `tick()` after `open()`**. DTLS retransmits need the
   50 ms pump.
4. **WolfSSL Init not called**. Should never happen with the
   engine, but if you instantiate `DtlsSessionWolfSSL` directly,
   you must call `wolfSSL_Init()` once per process.

### 3.3 DTLS Connected but no audio

SRTP key installation failed. Most likely cause: the DTLS backend
doesn't support `SSL_export_keying_material`.

- **OpenSSL (stock)**: SRTP patch required; install GMSSL or
  `openssl-srtp` instead.
- **WolfSSL built without SRTP**: rebuild wolfSSL with
  `--enable-srtp` (vendored configures this by default).
- **mbedTLS**: requires `MBEDTLS_SSL_EXPORT_KEYS` and the SRTP
  profile macros. Less common — verify before pinning.

Symptom: `srtp_installed() == false` after `dtls_connected() == true`.

### 3.4 Bidirectional audio works one way but not the other

Almost always a STUN/ICE candidate selection issue. Check that
your DTLS server-cookie / role is consistent. The loopback-p2p
example shows the correct pattern: pre-set remote ICE on the
answerer.

### 3.5 "Address already in use" on rapid restarts

SO_REUSEADDR or SO_REUSEPORT not set. Wait ~30 s or kill the
stray process. On Windows the TCP table reclaims ports faster.

```bash
lsof -iUDP:51000  # which process holds the port
```

### 3.6 PCM tap callback fires but PCM is silence

You replaced the audio3a plugin with one that has no source. Either:

- Re-enable `webrtc_apm` (default).
- Add a `null` source test: feed `engine.send_audio(silent_pcm, n)`
  once per frame and verify tap fires with non-trivial RMS.
- Or use `engine.audio3a()->process_capture(...)` directly — the
  `demo-agent-gateway` shows this.

### 3.7 Plugin `*_name` field fails with `kErrNotFound`

The factory id is not registered. Verify:

```cpp
// In a debugging build, call this once at program start:
auto& reg = nimrtc::core::PluginRegistry::instance();
std::cerr << "audio3a plugins: ";
for (auto id : reg.list_audio3a()) std::cerr << std::string(id) << " ";
std::cerr << "\n";
```

If your id is missing: the static-init order is wrong; make sure
the `.cpp` containing `NIMRTC_REGISTER_*` is linked into your
binary (forget `#ifdef` in some platform configurations).

### 3.8 Build is green but engine.open() returns non-zero

Inspect with:

```cpp
nimrtc::engine::NimRTCEngine e(cfg);
uint32_t rc = e.open();
if (rc != 0) {
    std::cerr << "open failed rc=" << std::hex << rc << std::dec << "\n";
    std::cerr << "last_open_rc=" << std::hex << e.last_open_rc() << std::dec << "\n";
}
```

Refer to the bit-region table in [`engine_api.md`](./api/engine_api.md)
§13. The typical values:

| rc | Subsystem |
|---|---|
| `0x2xxx` | DTLS (`0x2000` — factory not found; `0x2010` — open() failed) |
| `0x1FFF` | Transport (typically ICE socket bind) |
| `0x1A00` | audio3A |
| `0x1800` | codec |
| `0x1700` | JB |

### 3.9 `on_error` fires constantly with `kErrResourceExhausted`

A factory's `create()` returned null. Likely an out-of-memory
issue OR a backend that depends on a missing SDK. Run with
`NIMRTC_LOG=info` and check what was being created when the
failure happened.

### 3.10 Audio plays fine but DTMF / RFC 2833 is missing

`DTMF` is not in v0.12.0-alpha scope (not in `IRTP`, not in
`Codec` interface). Use a dedicated data channel — see
`engine.create_data_channel(label)` + DataChannel via the
[engine API docs](./api/engine_api.md).

### 3.11 Video pipelines linked but no frames decoded

`video_source_name` / `video_receiver_name` resolve, but the
engine doesn't pipe them into the RTP/DTLS/SRTP stack yet
(P1.1 future work). To exercise the video plugin chain, drive
the plugins directly:

```cpp
auto* rx = engine.video_receiver();
auto* tx = engine.video_sender();
rx->push_rtp(pkt, /*now_us=*/0);   // manual RTP injection
tx->push_frame(encoded_nal, /*rtp_ts=*/0);
```

### 3.12 "WebRTC APM pre-built library not found" at runtime

APM was skipped at build but your code calls into the engine
3A path. Either:

```bash
cmake --preset dev -DNIMRTC_VENDORED_WEBRTC_APM=OFF
```

and reconfigure, or actually fetch the prebuilt:

```bash
python tools/fetch_webrtc_apm.py
```

Then re-link.

---

## 4. Performance / quality

### 4.1 High jitter / packet loss on a local loopback

Likely BWE misconfig. Try:

```cpp
cfg.bwe_config.initial_bitrate_bps = 1'000'000;   // 1 Mbps
cfg.scheduler_config.strict_priority = true;      // guarantee audio
```

### 4.2 Audio glitches every ~10 s

JB resetting to initial delay; often a clock drift between RTP
timestamps and capture timestamps. Check:

- `audio_codec.clock_rate` matches `pcm_sample_rate_hz`.
- `send_audio` cadence is steady (`std::chrono::steady_clock` vs
  `system_clock` — use the former).
- `process_capture` (audio3A path) is called at 48 kHz / 10 ms,
  not 44.1 kHz / 20 ms if the codec is configured for 48/10.

### 4.3 Decode block on large audio frame

Opus enforces a max packet duration (default 60 ms). For longer
frames you need:

```cpp
cfg.audio_codec.fmtp = "maxplaybackrate=48000;stereo=1;sprop-stereo=1";
```

Or pre-split into 20 ms frames before `send_audio`.

### 4.4 High CPU usage on aarch64

- `-O3 -DNDEBUG` (Release) is mandatory on aarch64; Debug
  builds are 5–8x slower.
- `L2` (codec + 3A) is the heaviest. Disable 3A via
  `cfg.audio3a_name = ""` for a quick check; if that resolves
  the CPU issue, the APM is the bottleneck.
- See `tools/benchmark_size.py` — run after every CMake option
  change to spot regressions.

### 4.5 Memory grows steadily during long-running sessions

Eighth-byte leaks in codec encoders / JB fragments. Run under
ASAN:

```bash
cmake --preset asan -DNIMRTC_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --preset tests -R leak
```

ASAN will show the offending stack frame.

---

## 5. Interop

### 5.1 Chrome rejects SDP `a=fingerprint`

The cert in the SDP doesn't match the cert the server actually
presents. Reproduce the cert that the engine generates and verify
fingerprint locally:

```bash
./build/examples/loopback-p2p --print-fingerprint
```

If the printed fingerprint doesn't match `a=fingerprint` in the
offer, your cert generator and your SDP embetter are out of sync.
File an issue with `[interop]` prefix.

### 5.2 Safari disconnects after ~30 s

Safari is strict about `a=rtpmap` ordering and `bundle-only`.
Generate the SDP with:

```cpp
auto offer = engine.create_offer();   // produces RFC 8829-conformant SDP
```

Do not hand-edit the SDP and expect Safari to keep the call up.

### 5.3 Firefox shows "incompatible SDP"

Firefox insists on `a=rtcp-rsize` and a specific SSRC ordering.
The engine emits these; verify with Wireshark. If you've replaced
the SDP parser plugin (`sdp_name = "..."`), the new plugin may not
emit them.

### 5.4 OpenHarmony (OHOS) device: build green but binary won't load

Almost certainly a sysroot mismatch (compiler and link step use
different `--target`). See
[`building-on-openharmony.md`](./guides/building-on-openharmony.md)
§7.1 (R1 musl audit) and §7 troubleshooting.

---

## 6. Where to look next

- `docs/api/engine_api.md` — method references.
- `docs/api/engine_config.md` — config field reference.
- `docs/guides/switching-dtls-backend.md` — DTLS swap guide.
- `docs/guides/building-on-openharmony.md` — OHOS build issues.
- `docs/plugin_author_guide.md` — plugin authoring pitfalls.
- `CONTRIBUTING.md` — DCO / PR process for fixes.
- GitHub Discussions — community Q&A.
- GitHub Issues (with `[BUG]` / `[INTEROP]` / `[DOCS]` prefix).

# ADR-014 spike — msquic 5-criterion validation

> **Status: opening.** The spike is the source of truth for the
> 5 evaluation criteria in
> [`docs/adr/ADR-014-web-transport-vendor.md`](../docs/adr/ADR-014-web-transport-vendor.md)
> §"The 5 evaluation criteria". `results.txt` is the gating
> evidence; if any criterion fails, ADR-014 reopens and the
> fallback path is **quiche** (BSD-2, C API, userspace only on
> Windows), per ADR-014 §"Consequences → Fallback path".

## What this spike does

The spike exercises the full **msquic C API surface** against
the planned `IWebTransportSession` methods (per
`docs/plan/v1.0-plan.md` §10.4). Five small programs:

| Program | Tests | Maps to msquic C API |
|---|---|---|
| `echo_client.cpp` | 0-RTT echo client (round-trip latency < 50 ms target) | `MsQuicRegistrationOpen` + `MsQuicConfigurationOpen` + `MsQuicConnectionOpen` (with `QUIC_SETTINGS` + `MsQuicResumptionTicket`) + `MsQuicStreamOpen` + `MsQuicStreamSend` + recv callback |
| `echo_server.cpp` | msquic listener; accepts the 0-RTT resumption; echoes payload | `MsQuicListenerOpen` + `MsQuicListenerStart` + `MsQuicConnectionCallback` (event `QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED`) + `MsQuicStreamSend` |
| `stream_reset.cpp` | tests `MsQuicStreamSendComplete` + reset_stream semantics | `MsQuicStreamSend` (with `QUIC_SEND_FLAG_ABORT` on cancel) + `MsQuicStreamClose` + `MsQuicStreamSendComplete` callback |
| `datagram_roundtrip.cpp` | tests `MsQuicDatagramSend` / recv | `MsQuicDatagramSend` + `QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED` callback |
| `concurrent_streams.cpp` | tests fanout of 16 bidi streams; send on each, recv on each, close all | `MsQuicStreamOpen` × 16 + concurrent `MsQuicStreamSend` × 16 + `MsQuicStreamClose` × 16 |

Each program writes a single status line to `results.txt`
with PASS/FAIL + a per-criterion marker. The CMake build links
against `src/third_party/msquic/` (the vendored submodule).

## 5-criterion check

The spike output is the **gating evidence** for ADR-014. The
5 criteria are exactly the ones in `v1.0-plan.md` §10.2:

| # | Criterion | How the spike checks it |
|---|---|---|
| 1 | **License**: MIT / BSD-2 / Apache-2 only (no GPL, no SSPL) | `results.txt` line `[1]` records the contents of `src/third_party/msquic/LICENSE` header (typically `MIT License` + `Copyright (c) Microsoft Corporation`). A CMake script in `CMakeLists.txt` (the `assert_mit_license` block) reads the LICENSE file and asserts the MIT identifier. |
| 2 | **No Rust / no CFFI**: preserve the `src/plugins/` header-only invariant; no FFI surface that violates it | `results.txt` line `[2]` records the count of `.rs` files in `src/third_party/msquic/` and in the msquic dependency tree (via `cargo tree` if Rust is present; if Rust is absent, criterion is trivially met). A CMake script (`assert_no_rust`) walks the msquic tree and asserts 0 `.rs` files. |
| 3 | **Cross-platform CI green**: windows-2022 + linux-gcc + macos-clang + linux-aarch64 | `results.txt` line `[3]` records per-platform PASS/FAIL of the 5 programs above. The spike is run on all 4 platforms via the same `ci.yml` matrix that gates the beta.1 tag cut. |
| 4 | **Windows is first-class**: msquic's kernel-mode QUIC support is the differentiator; quiche is userspace-only on Windows | `results.txt` line `[4]` records the `0-RTT echo` round-trip latency on Windows-2022 with kernel QUIC enabled. A latency < 50 ms target passes; > 200 ms (the userspace-only range) fails. The Windows-specific check is gated on the platform being Windows. |
| 5 | **Active maintenance**: last commit within 6 months of v0.14.0 tag cut | `results.txt` line `[5]` records the latest commit SHA + timestamp from the msquic submodule (via `git -C src/third_party/msquic log -1 --format=%H %ci`). A timestamp < 6 months before the spike-run time passes. |

## How to run

```bash
# 1. Fetch the msquic submodule (SHA-pinned via vendor.json)
cmake -DNIMRTC_FETCH_VENDORED=ON -B build/spike
# 2. Build the 5 spike programs
cmake --build build/spike --target spike_echo_client \
                                    spike_echo_server \
                                    spike_stream_reset \
                                    spike_datagram_roundtrip \
                                    spike_concurrent_streams
# 3. Run each program; each writes to results.txt
./build/spike/spike_echo_client   >> results.txt
./build/spike/spike_echo_server   >> results.txt   # background; sleep 5
./build/spike/spike_stream_reset  >> results.txt
./build/spike/spike_datagram_roundtrip >> results.txt
./build/spike/spike_concurrent_streams  >> results.txt
# 4. Final assertion: all 5 criteria PASS
grep -q '^\[1\] PASS' results.txt && \
grep -q '^\[2\] PASS' results.txt && \
grep -q '^\[3\] PASS' results.txt && \
grep -q '^\[4\] PASS' results.txt && \
grep -q '^\[5\] PASS' results.txt && \
echo "ADR-014 spike: ALL 5 CRITERIA PASS" || \
echo "ADR-014 spike: FAIL — see results.txt; reopen ADR-014"
```

## Results

`results.txt` is the gating evidence. **At plan open, the
spike has not been run yet** (the submodule is not yet
fetched; the spike programs compile but are not yet exercised
against a real msquic build). The `results.txt` template
below is what the spike-run will populate.

```
# ADR-014 spike — msquic 5-criterion validation
#
# Spike-run timestamp: <TBD at spike-run time>
# Platform: <windows-2022 | linux-gcc | macos-clang | linux-aarch64>
# msquic submodule SHA: <TBD — `git -C src/third_party/msquic rev-parse HEAD`>
# msquic submodule timestamp: <TBD — `git -C src/third_party/msquic log -1 --format=%ci`>
#
[1] <PASS|FAIL> License: <license string from LICENSE header>
[2] <PASS|FAIL> No Rust / no CFFI: <count of .rs files in msquic tree>
[3] <PASS|FAIL> Cross-platform CI green: <per-platform PASS/FAIL>
[4] <PASS|FAIL> Windows is first-class: <0-RTT echo round-trip latency in ms>
[5] <PASS|FAIL> Active maintenance: <submodule age in days>
```

## What the spike does NOT do

- **It does not implement `IWebTransportSession`**. The spike
  exercises the msquic C API directly to verify the 5
  criteria; the seam impl is a separate PR (beta.1 PR #5
  per `v0.14-plan.md` §4.2).
- **It does not run against the NimRTC engine**. The spike
  is a standalone msquic C API exercise; the engine
  integration is a separate PR (beta.1 PR #9 per
  `v0.14-plan.md` §4.2 — the `EngineConfig::datachannel_backend`
  field + `Impl::datachannel` tagged union).
- **It does not exercise Chrome e2e Case E**. The Case E
  harness is a separate PR (beta.1 PRs #12-13 per
  `v0.14-plan.md` §4.2) and depends on the
  `WebTransportDataChannel` impl landing first.

## Why this is a spike, not a test

A spike is a time-boxed investigation; the goal is **decision
evidence**, not regression coverage. The 5 programs above are
the smallest possible code that exercises the msquic C API
surface against the planned `IWebTransportSession` methods.
They are not part of the ctest regression surface; they are
deleted at the beta.1 tag cut (or moved to
`tests/test_web_transport_msquic.cpp` with a proper test
harness; the regression test is a separate PR).

## References

- [`docs/adr/ADR-014-web-transport-vendor.md`](../docs/adr/ADR-014-web-transport-vendor.md) —
  the parent ADR; this spike is the source of truth for
  ADR-014 §"The 5 evaluation criteria".
- [`docs/plan/v1.0-plan.md`](../docs/plan/v1.0-plan.md) §10.2 —
  the 5 evaluation criteria (verbatim).
- [`docs/plan/v0.14-plan.md`](../docs/plan/v0.14-plan.md) §9 —
  the spike framework; this directory is the implementation
  of §9.3.
- [msquic on GitHub](https://github.com/microsoft/msquic) —
  the vendored library; SHA-pinned via `vendor.json`.

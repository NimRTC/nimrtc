# Slice: v0.15.0-web-transport-product-route

## Purpose

v0.15.0 — WebTransport/QUIC becomes the default DataChannel backend
(product route for v1.0.0). SCTP-over-DTLS is retained as the legacy
path. Chrome ↔ NimRTC Case E interop (datagram + bidi-stream echo)
is the acceptance gate.

## Scope

- `src/engine/include/nimrtc/engine/engine.hpp` — `datachannel_backend`
  default `"sctp"` → `"web_transport"`; `web_transport_peer_host` /
  `web_transport_peer_port` client-mode fields; `create_data_channel`
  reliability overload.
- `src/engine/src/engine.cpp` — lazy WT session creation in
  `create_data_channel()`, `create_offer()` SDP split, `tick()` state
  poll, `close()` session teardown.
- `src/web_transport/` — dual QUIC stream callbacks (no tagged
  pointers), session-bound channel factory, Case E legacy echo.
- `examples/demo-wtserver/` — datagram + bidi-stream echo server,
  exit-code contract (0 = session + data, 1 = start failure, 2 = timeout).
- `tests/` — `test_web_transport_data_channel`,
  `test_datachannel_engine`, `test_engine_plugin_loading` regression;
  `test_api_classification` baseline 101 → 102 (`sctp/dcep.hpp`).
- `tests/chrome_interop_harness/run_webtransport_harness.py` +
  `interop/chrome/test_chrome_webtransport.html` — Case E harness.

## Acceptance criteria

1. `datachannel_backend` defaults to `"web_transport"`; explicit
   `"sctp"` still opens the legacy path; `""` disables DataChannel.
2. Full `ctest` suite passes (616/616 on Windows/MSVC Debug, excluding
   browser-gated harnesses); `test_api_classification` baseline = 102.
3. Case E harness PASS: E1 API available, E2 session connected,
   E3/E4 datagram send/echo, E5/E6 stream send/echo
   (`build/e2e/case_e_webtransport_results.json`).
4. No tagged-pointer contexts in `src/web_transport/`; no `TODO`/`stub`
   markers on the WT product path.
5. `AGENTS.md` L1–L7 hard constraints hold (see `docs/plan/v0.15.0-self-review.md`).

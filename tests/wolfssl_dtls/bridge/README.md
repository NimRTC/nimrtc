# wolfSSL ↔ Chrome DTLS bridge

Prerequisites:

* Python Playwright: `pip install playwright && playwright install chromium`.
* A build with `NIMRTC_BUILD_TESTS=ON` and the `nimrtc_dtls` library.
* `tests/wolfssl_dtls/chrome_dtls_test.html` available to the browser.

Build the responder and run the orchestrator:

```powershell
cmake -S tests/wolfssl_dtls/bridge -B build/wolfssl_dtls_bridge
cmake --build build/wolfssl_dtls_bridge --target wolfssl_responder_target --config Release
python tests/wolfssl_dtls/bridge/run_bridge.py --build-dir build
```

All logs and JSON artifacts belong under `build/wolfssl_bridge/`. A successful
full deployment writes `chrome.json` with `dtlsState: "connected"` and a
`responder.log` containing the negotiated SHA-256 fingerprint and RFC 5764
`EXTRACTOR-dtls_srtp` keying material export, for example:

```
[responder] peer fingerprint: 5A:3C:...:91
[responder] SRTP keying material exported (60 bytes)
```

The bridge validates DTLS and key export only; it does not exercise SRTP media.
Chrome may require `--use-fake-ui-for-media-stream` and
`--use-fake-device-for-media-stream`. Network ICE candidates and the exact
Chrome page signaling adapter must be supplied by the deployment harness.

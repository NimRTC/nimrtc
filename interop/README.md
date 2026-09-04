# interop/

Browser and third-party interop test fixtures, scripts, and runners.

## Scope

- Test vectors (SDP offers/answers, RTP packet dumps) for Chrome / Firefox interop
- Automation scripts (Python / Bash) that drive headless browsers
- Expected-results matrices per NimRTC release

## Browser matrix (P1 baseline)

| Browser | Version | Platform |
|---------|---------|----------|
| Chrome  | fixed stable | Windows / Linux |
| Firefox | fixed stable | Windows / Linux |

## CI integration

`interop/` is consumed by `.github/workflows/interop.yml`. The runner uses
headless Chrome + Firefox via Selenium or Playwright to validate:
1. SDP offer / answer exchange (webrtc-internal API)
2. Opus audio round-trip (PCM capture → encode → decode → playback)
3. RTP packet loss concealment behaviour

## Adding a test fixture

Drop a `.json` or `.rtpdump` file alongside a matching `expected.md` that
describes the expected behaviour. The interop CI script picks them up automatically.

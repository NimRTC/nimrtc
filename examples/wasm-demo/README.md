# Wasm browser demo (live placeholder)

> **Current state:** a static landing page is **live** at
> [`https://nimrtc.github.io/nimrtc/wasm/`](https://nimrtc.github.io/nimrtc/wasm/).
> It hosts an embedded terminal recording of the in-process
> `examples/loopback-p2p` example — not a runnable browser demo. The
> **real** NimRTC-in-a-browser experience (Chrome ↔ NimRTC audio + video,
> with the `transport` profile compiled to `wasm32-unknown-unknown`) is on
> the **v0.12.0** milestone; see the v0.12.0 issue list for the current
> tracking items.
>
> This page exists so the v0.11.0 Show HN / v0.12.0 announcement link does
> not 404, and so visitors who do click through see a credible
> "this project exists and the engine runs" signal before the wasm build
> itself lands.

---

## What lives here today

```
examples/wasm-demo/
├── README.md              (this file)
└── www/                   ← served by GitHub Pages at /wasm/
    ├── index.html         landing page (hero, demo, quick start, status, arch)
    ├── style.css
    ├── player.js          minimal vanilla-JS asciicast v2 player
    ├── player.css
    ├── loopback-p2p.cast  synthesized asciicast (see "Recording a real one" below)
    └── favicon.svg
```

The page is a self-contained static site — no build step, no JS framework,
no third-party fetches at runtime. It is published by the
`.github/workflows/wasm-demo-pages.yml` workflow on every push to `main`
that touches `examples/wasm-demo/www/**`.

### What the page shows

- **Hero** — the headline numbers from `README.md` (~5.6 MB static lib,
  4-platform CI, ~50k LOC, 5 profiles).
- **Live demo** — an embedded terminal widget that plays back the
  `loopback-p2p` example's stderr. The recording is **synthesized** from
  the `fprintf` calls in `examples/loopback-p2p/loopback-p2p.cpp`, not a
  real terminal capture. The page says so explicitly.
- **Quick start** — copy-paste build instructions matching the root
  `README.md`'s Quick Start.
- **Status** — what's shipped in v0.11.0, what's deferred to v1.x, and
  what's on the v0.12.0 roadmap.
- **Architecture in 30 seconds** — the L0–L3 + PAL diagram in plain text.
- **Roadmap** — honest framing of why this page is a placeholder.

---

## How to enable Pages (one-time)

The workflow writes the artifact; you still have to flip the switch:

1. Repo → **Settings** → **Pages**
2. **Source:** `GitHub Actions`
3. Save. The next push to `examples/wasm-demo/www/**` (or the workflow
   file itself) will deploy automatically.

If Pages is not yet enabled, the workflow fails the **Preflight** step
with a clear `::error::` pointing at the missing environment, instead of
silently timing out at the deploy step.

---

## Recording a real `loopback-p2p` session

The synthesized cast gets the structure right but is not a real terminal
recording. To replace it with a real one:

1. Build the example:
   ```bash
   cmake --preset debug          # or debug.msvc / debug.macos
   cmake --build build --target loopback-p2p
   ```
2. Install [asciinema](https://asciinema.org/) (`brew install asciinema`
   / `apt install asciinema` / `pipx install asciinema` / on Windows
   `pipx install asciinema` or `scoop install asciinema`).
3. Run the recorder:
   ```bash
   bash tools/record_loopback_demo.sh                     # Unix
   pwsh tools/record_loopback_demo.ps1                    # Windows
   ```
   The script locates the binary under `build/`, runs a 2 s smoke
   check, backs up the current cast, then records with
   `asciinema rec --cols 132 --rows 30 --idle-time-limit 2`.
4. Verify locally:
   ```bash
   python -m http.server -d examples/wasm-demo/www
   # open http://127.0.0.1:8000/
   ```
5. Commit + push:
   ```bash
   git add examples/wasm-demo/www/loopback-p2p.cast
   git commit -s -m "demo(wasm): record real loopback-p2p terminal session"
   git push
   ```

The Pages workflow re-deploys on the next push.

---

## What the v0.12.0 page will look like

When the wasm build lands, this directory will grow:

```
examples/wasm-demo/
├── README.md
├── CMakeLists.txt                (wasm32 preset, hidden from default build)
├── www/                          ← still served at /wasm/
│   ├── index.html                ← updated landing + a real interactive demo
│   ├── engine.js                 ← JS-side wrapper around the wasm engine
│   ├── engine.wasm               ← NimRTCEngine compiled to wasm32
│   └── main.ts                   ← demo logic (~150 LOC, getUserMedia + MediaStream)
└── ci/
    └── build-wasm.yml            (Emscripten toolchain, emcmake cmake --preset wasm)
```

The build target will be:

```bash
emcmake cmake --preset wasm
emmake make -C build/wasm nimrtc_wasm
```

The browser side will use a real `RTCPeerConnection` on Chrome and a
`NimRTC` instance on the other, with audio + video flowing both ways.
The plugin seam (`IDtlsSession` for wolfSSL DTLS 1.3, `IICETransport` for
libjuice, `IVideoSource` / `IVideoSink` adapter shims for
`getUserMedia` / `MediaStream`) is the contract the v0.12.0 wasm adapter
has to fit through. See [`docs/plugin_author_guide.md`](../../docs/plugin_author_guide.md)
for the plugin contract.

If you have prior wasm / emscripten experience and want to take this on,
open a "claim" comment on a v0.12.0 milestone issue before sending a PR
— coordinate the wasm DTLS / ICE backend selection early, since both
have cross-platform CI implications.

---

## License

Apache-2.0, same as the rest of the project.

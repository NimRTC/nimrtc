"""Use Playwright CDP to query Chrome's RTCPeerConnection DTLS state during a fresh test."""
import asyncio
import sys
from pathlib import Path


async def main():
    import os
    from playwright.async_api import async_playwright

    chrome_exe = os.environ.get('NIMRTC_CHROME_PATH', '')
    if not chrome_exe:
        print('NIMRTC_CHROME_PATH not set', file=sys.stderr)
        return

    url = 'file:///' + str(Path('D:/MyOpen/NimRTC/interop/chrome/test_chrome_opus.html').resolve().as_posix()) + '?room=diag&ws=ws://localhost:8765'

    async with async_playwright() as p:
        browser = await p.chromium.launch(
            headless=True,
            executable_path=chrome_exe,
            args=[
                '--headless=new',
                '--no-sandbox',
                '--disable-dev-shm-usage',
                '--use-fake-ui-for-media-stream',
                '--use-fake-device-for-media-stream',
                '--log-level=0',
                '--v=1',
                '--enable-logging=stderr',
                '--vmodule=*/webrtc/*=2,*/dtls*=2,*/ssl*=2',
                '--allow-running-insecure-content',
            ],
        )
        context = await browser.new_context()
        page = await context.new_page()
        console_msgs = []
        page.on('console', lambda m: console_msgs.append(f'[{m.type}] {m.text}'))

        await page.goto(url)
        await page.wait_for_timeout(8000)

        # Try to inspect WebRTC internals via getStats
        stats = await page.evaluate("""async () => {
            const pc = window.pc || (window._pc || null);
            if (!pc) return {error: 'no pc in window'};
            const s = await pc.getStats();
            const out = [];
            for (const [k, v] of s) {
                out.push({type: v.type, id: v.id, ...v});
            }
            return out.filter(x => x.type && (x.type.includes('transport') || x.type.includes('candidate') || x.type.includes('dtls')));
        }""")
        print('=== PC Stats ===')
        if isinstance(stats, list):
            for s in stats:
                print(json.dumps(s, indent=2, default=str))
        else:
            print(stats)

        # Check connection state
        state = await page.evaluate("""() => {
            const pc = window.pc || (window._pc || null);
            if (!pc) return 'no pc';
            return {
                iceConnectionState: pc.iceConnectionState,
                connectionState: pc.connectionState,
                signalingState: pc.signalingState,
                sctp: pc.sctp ? pc.sctp.state : null,
            };
        }""")
        print('=== PC State ===')
        print(state)

        await browser.close()
        print('=== Console (last 40) ===')
        for m in console_msgs[-40:]:
            print(m)


import json
asyncio.run(main())

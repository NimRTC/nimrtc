// test_nimrtc_chrome.js — NimRTC ↔ Chrome Opus audio interop test
// Uses puppeteer-core for Chrome control + ws for signaling

const pup = require('puppeteer-core');
const { WebSocket } = require('ws');
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

const CHROME_PATH = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const DEMO_PATH = 'D:/MyOpen/NimRTC/build/examples/Debug/demo-p2p.exe';
const PROXY_PATH = 'D:/MyOpen/NimRTC/interop/signaling/signaling_proxy.py';
const HTML_PATH = 'file:///D:/MyOpen/NimRTC/interop/chrome/test_chrome_opus.html';
const SIGNALING_URL = 'ws://localhost:8765/interop';

const ROOM = 'interop';
const TEST_DURATION_MS = 15000;
const PROXY_LOG = 'D:\\MyOpen\\NimRTC\\build\\nimrtc_chrome_proxy.log';

async function run() {
    console.log('[test] NimRTC ↔ Chrome Opus interop test');
    console.log('[test] Signaling:', SIGNALING_URL);

    // ── Start demo-p2p as offerer ──────────────────────────────────────────
    console.log('[test] Starting demo-p2p as offerer...');
    const proxyProc = spawn('python', [
        PROXY_PATH,
        '--signaling', SIGNALING_URL,
        '--binary', DEMO_PATH,
        '--no-stun',
        '--duration', '30'
    ], {
        cwd: path.dirname(DEMO_PATH),
        stdio: ['ignore', 'pipe', 'pipe']
    });

    // Clear log file
    try { fs.writeFileSync(PROXY_LOG, ''); } catch(e) {}
    const logFd = fs.openSync(PROXY_LOG, 'a');

    let proxyOutput = '';
    proxyProc.stdout.on('data', d => {
        const s = d.toString();
        proxyOutput += s;
        process.stdout.write('[demo] ' + s);
        fs.writeSync(logFd, s);
    });
    proxyProc.stderr.on('data', d => {
        const s = d.toString();
        proxyOutput += s;
        process.stderr.write('[demo] ' + s);
        fs.writeSync(logFd, s);
    });

    // Give demo-p2p 2 seconds to open, then start Chrome
    await new Promise(r => setTimeout(r, 2000));

    // ── Start Chrome as answerer ──────────────────────────────────────────
    console.log('[test] Starting Chrome as answerer...');
    const browser = await pup.launch({
        executablePath: CHROME_PATH,
        headless: true,
        args: [
            '--no-sandbox',
            '--disable-setuid-sandbox',
            '--disable-gpu',
            '--use-fake-ui-for-media-stream',
            '--use-fake-device-for-media-stream',
            '--disable-dev-shm-usage',
            '--disable-web-security',
        ]
    });

    const page = await browser.newPage();
    const consoleLogs = [];
    const errors = [];

    page.on('console', msg => {
        const text = msg.text();
        consoleLogs.push(text);
        // Filter to important lines
        if (text.includes('[RESULT]') || text.includes('[ICE]') ||
            text.includes('[SDP]') || text.includes('[WS]') ||
            text.includes('[RTP]') || text.includes('[ERROR]') ||
            text.includes('ICE Connected') || text.includes('audioReceived')) {
            console.log('[chrome:console]', text);
        }
    });

    page.on('pageerror', err => {
        console.log('[chrome:pageerror]', err.message);
        errors.push(err.message);
    });

    const testUrl = HTML_PATH +
        '?room=' + ROOM +
        '&ws=' + SIGNALING_URL +
        '&timeout=' + TEST_DURATION_MS;

    console.log('[test] Loading:', testUrl);
    await page.goto(testUrl, { waitUntil: 'domcontentloaded', timeout: 10000 });

    // Wait for test to complete
    await new Promise(r => setTimeout(r, TEST_DURATION_MS + 3000));

    // ── Read results ───────────────────────────────────────────────────────
    const results = await page.evaluate(() => {
        return window._interopResults || null;
    });

    console.log('\n=== Interop Results ===');
    if (results) {
        console.log('wsConnected:   ', results.wsConnected);
        console.log('sdpOfferSeen:  ', results.sdpOfferSeen);
        console.log('sdpAnswerSent: ', results.sdpAnswerSent);
        console.log('iceConnected:  ', results.iceConnected);
        console.log('audioReceived: ', results.audioReceived);
        console.log('rtpPackets:    ', results.rtpPackets);
        console.log('rtpPayloadType:', results.rtpPayloadType);
        console.log('lastSeqNum:    ', results.lastSeqNum);
        console.log('errors:        ', JSON.stringify(results.errors));
        console.log('exitCode:      ', results.exitCode);
    } else {
        console.log('No _interopResults found (page may have crashed)');
    }

    console.log('\n=== Console Errors ===');
    errors.forEach(e => console.log('ERROR:', e));

    await browser.close();

    // Kill proxy
    proxyProc.kill();

    // Read and save proxy log
    let savedLog = '';
    try {
        savedLog = fs.readFileSync(PROXY_LOG, 'utf8');
        console.log('\n=== Proxy Log ===');
        console.log(savedLog || '(empty)');
    } catch(e) { console.log('Could not read proxy log:', e.message); }

    // ── Summary ────────────────────────────────────────────────────────────
    console.log('\n=== Summary ===');
    if (results) {
        const ok = results.iceConnected && results.sdpOfferSeen && results.sdpAnswerSent;
        console.log('ICE Connected:    ', results.iceConnected ? '✅ PASS' : '❌ FAIL');
        console.log('SDP Offer Seen:  ', results.sdpOfferSeen ? '✅ PASS' : '❌ FAIL');
        console.log('SDP Answer Sent:  ', results.sdpAnswerSent ? '✅ PASS' : '❌ FAIL');
        console.log('Audio Received:   ', results.audioReceived ? '✅ PASS' : '❌ FAIL');
        console.log('RTP Packets:     ', results.rtpPackets > 0 ? '✅ ' + results.rtpPackets : '❌ 0');
        console.log('');
        console.log(ok ? '🎉 Chrome ↔ NimRTC INTEROP SUCCESS' : '❌ Interop test FAILED');
        process.exit(results.exitCode || (ok ? 0 : 1));
    } else {
        console.log('❌ No results — test inconclusive');
        process.exit(2);
    }
    try { fs.closeSync(logFd); } catch(e) {}
}

run().catch(e => {
    console.error('[test] Fatal:', e);
    process.exit(3);
});

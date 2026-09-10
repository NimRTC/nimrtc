/**
 * test_audio_interop_wolfssl.js — Chrome ↔ NimRTC audio interop test
 * Uses puppeteer to control Chrome as the offerer with the full audio
 * test page (test_chrome_opus.html). demo-p2p runs as answerer via
 * signaling_proxy.py.
 *
 * Run with: node test_audio_interop_wolfssl.js
 */

const puppeteer = require('puppeteer-core');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const http = require('http');

const SIGNALING_URL = 'ws://localhost:8765/audio_interop';
const ROOM = 'audio_interop';
const DEMO_P2P = 'D:\\MyOpen\\NimRTC\\build\\examples\\Debug\\demo-p2p.exe';
const PROXY_SCRIPT = 'D:\\MyOpen\\NimRTC\\interop\\signaling\\signaling_proxy.py';
const PYTHON = 'C:\\Users\\Administrator\\.pyenv\\pyenv-win\\versions\\3.9.13\\python.exe';
const HTML = fs.readFileSync(
    'D:\\MyOpen\\NimRTC\\interop\\chrome\\test_chrome_opus.html', 'utf8');
const PROXY_OUT = 'D:\\MyOpen\\NimRTC\\build\\audio_interop_proxy.log';
const TEST_DURATION_MS = 18000;

async function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function main() {
    console.log('[test] Chrome ↔ NimRTC audio interop test (wolfSSL DTLS)');
    fs.writeFileSync(PROXY_OUT, '');

    // Serve the HTML page on localhost
    const httpPort = 9989;
    const server = http.createServer((req, res) => {
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(HTML);
    });
    await new Promise(r => server.listen(httpPort, '127.0.0.1', r));
    console.log('[test] HTTP server on http://127.0.0.1:' + httpPort);

    // Start demo-p2p as answerer (waits for offer from Chrome)
    const proxyFd = fs.openSync(PROXY_OUT, 'a');
    console.log('[test] Starting demo-p2p as answerer...');
    const proxy = spawn(PYTHON, [
        PROXY_SCRIPT,
        '--signaling', SIGNALING_URL,
        '--binary', DEMO_P2P,
        '--answerer',
        '--bind', '0.0.0.0',
        '--no-stun',
        '--duration', '30'
    ], {
        stdio: ['ignore', 'pipe', 'pipe'],
        cwd: path.dirname(PROXY_SCRIPT),
        env: {
            ...process.env,
            'NIMRTC_PROXY_STDERR': 'D:\\MyOpen\\NimRTC\\build\\demo_p2p_audio_stderr.txt',
            'NIMRTC_DTLS_TRACE': 'D:\\MyOpen\\NimRTC\\build\\audio_dtls_trace.log',
        }
    });

    let proxyOutput = '';
    proxy.stdout.on('data', d => {
        const s = d.toString();
        proxyOutput += s;
        process.stdout.write('[proxy] ' + s);
        fs.writeSync(proxyFd, s);
    });
    proxy.stderr.on('data', d => {
        const s = d.toString();
        proxyOutput += s;
        process.stderr.write('[proxy] ' + s);
        fs.writeSync(proxyFd, s);
    });
    await sleep(2000);  // give demo-p2p time to join the room

    // Launch Chrome as offerer
    console.log('[test] Launching Chrome as offerer...');
    const browser = await puppeteer.launch({
        executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
        headless: true,
        args: [
            '--no-sandbox',
            '--disable-gpu',
            '--use-fake-ui-for-media-stream',
            '--use-fake-device-for-media-stream',
            '--disable-web-security',
            '--autoplay-policy=no-user-gesture-required',
        ]
    });

    const page = await browser.newPage();
    const consoleLogs = [];
    page.on('console', msg => {
        const text = msg.text();
        consoleLogs.push(text);
        // Print interesting lines
        if (text.match(/\[(RESULT|RTP|ICE|SDP|WS|ERROR|STATE)\]/)) {
            console.log('[chrome]', text);
        }
    });
    page.on('pageerror', err => {
        console.error('[chrome:pageerror]', err.message);
        consoleLogs.push('PAGE ERROR: ' + err.message);
    });

    const testUrl = `http://127.0.0.1:${httpPort}/?room=${ROOM}&ws=${SIGNALING_URL}&timeout=${TEST_DURATION_MS}&offerer=1`;
    console.log('[test] Loading:', testUrl);
    await page.goto(testUrl, { waitUntil: 'domcontentloaded', timeout: 15000 });

    // Wait for test to complete
    await sleep(TEST_DURATION_MS + 5000);

    const results = await page.evaluate(() => window._interopResults || null);
    await browser.close();
    server.close();

    // Cleanup
    try { proxy.kill(); } catch(e) {}
    try { fs.closeSync(proxyFd); } catch(e) {}

    // Summary
    console.log('\n=== Audio Interop Results ===');
    if (results) {
        console.log('wsConnected:    ', results.wsConnected);
        console.log('sdpOfferSeen:  ', results.sdpOfferSeen);
        console.log('sdpAnswerSent: ', results.sdpAnswerSent);
        console.log('iceConnected:  ', results.iceConnected);
        console.log('audioReceived: ', results.audioReceived);
        console.log('rtpPackets:    ', results.rtpPackets);
        console.log('rtpPayloadType:', results.rtpPayloadType);
        console.log('lastSeqNum:    ', results.lastSeqNum);
        console.log('errors:        ', JSON.stringify(results.errors));
        console.log('exitCode:      ', results.exitCode);
        console.log('');

        const ok = results.iceConnected && results.sdpOfferSeen &&
                   results.sdpAnswerSent && results.audioReceived &&
                   results.rtpPackets > 0;
        console.log(ok ? '🎉 AUDIO INTEROP SUCCESS' : '❌ Audio interop failed');
        process.exit(ok ? 0 : 1);
    } else {
        console.log('❌ No _interopResults — test inconclusive');
        process.exit(2);
    }
}

main().catch(e => { console.error('Fatal:', e); process.exit(3); });

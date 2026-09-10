/**
 * NimRTC ↔ Chrome WebRTC Interop Test (puppeteer-based)
 * 
 * Uses puppeteer to control Chrome headless as the WebRTC offerer.
 * Flow:
 * 1. demo-p2p (answerer) via proxy — connects to signaling, waits for offer
 * 2. puppeteer launches Chrome headless with HTML page
 * 3. Chrome page acts as WebRTC offerer, creates offer, sends via WebSocket
 * 4. demo-p2p receives offer, creates answer
 * 5. Both exchange ICE candidates
 * 6. DTLS handshake attempts (wolfSSL ↔ BoringSSL)
 */

const puppeteer = require('puppeteer-core');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const http = require('http');
const { createServer } = http;

const SIGNALING_URL = 'ws://localhost:8765/interop_test';
const DEMO_P2P = 'D:\\MyOpen\\NimRTC\\build\\examples\\Debug\\demo-p2p.exe';
const PROXY_SCRIPT = 'D:\\MyOpen\\NimRTC\\interop\\signaling\\signaling_proxy.py';
const PYTHON = 'C:\\Users\\Administrator\\.pyenv\\pyenv-win\\versions\\3.9.13\\python.exe';
const PROXY_OUT = 'D:\\MyOpen\\NimRTC\\build\\proxy_interop_log.txt';
const CHROME_HTML = fs.readFileSync('D:\\MyOpen\\NimRTC\\build\\interop_chrome.html', 'utf8');

async function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function main() {
    console.log('[test] NimRTC ↔ Chrome WebRTC DTLS interop test (puppeteer)');
    console.log('[test] Signaling:', SIGNALING_URL);
    fs.writeFileSync(PROXY_OUT, '');

    // 1. Start HTTP server to serve Chrome HTML
    const httpPort = 9988;
    const server = createServer((req, res) => {
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(CHROME_HTML);
    });
    await new Promise(r => server.listen(httpPort, '127.0.0.1', r));
    console.log('[test] HTTP server on http://127.0.0.1:' + httpPort);

    // 2. Start demo-p2p (answerer) with debug env
    console.log('[test] Step 1: Starting demo-p2p (answerer) via proxy...');
    const demoEnv = {
        ...process.env,
        'NIMRTC_PROXY_STDERR': 'D:\\MyOpen\\NimRTC\\build\\demo_p2p_stderr.txt',
        'NIMRTC_DTLS_TRACE': 'D:\\MyOpen\\NimRTC\\build\\dtls_trace.txt',
        'NIMRTC_DTLS_KEYLOG': 'D:\\MyOpen\\NimRTC\\build\\dtls_keylog.txt',
        'NIMRTC_DTLS_SPKI_FILE': 'D:\\MyOpen\\NimRTC\\build\\nimrtc_spki.txt',
    };
    const proxy = spawn(PYTHON, [
        PROXY_SCRIPT,
        '--signaling', SIGNALING_URL,
        '--binary', DEMO_P2P,
        '--answerer',
        '--bind', '0.0.0.0',
        '--no-stun',
        '--duration', '25'
    ], {
        stdio: ['ignore', 'pipe', 'pipe'],
        cwd: path.dirname(PROXY_SCRIPT),
        env: demoEnv
    });

    const proxyOutFile = fs.openSync(PROXY_OUT, 'a');
    let proxyOutput = '';
    proxy.stdout.on('data', d => {
        const s = d.toString();
        proxyOutput += s;
        fs.writeSync(proxyOutFile, s);
    });
    proxy.stderr.on('data', d => {
        const s = d.toString();
        proxyOutput += s;
        fs.writeSync(proxyOutFile, s);
    });
    proxy.on('error', e => {
        console.error('[proxy] error:', e.message);
        fs.writeSync(proxyOutFile, '[proxy-error] ' + e.message + '\n');
    });
    console.log('[test] Proxy PID:', proxy.pid);
    await sleep(2000);

    // 3. Launch Chrome via puppeteer
    console.log('[test] Step 2: Launching Chrome (offerer) via puppeteer...');
    let chromeLogs = [];
    let chromeDone = false;
    let wsConnected = false;

    const browser = await puppeteer.launch({
        executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
        headless: true,
        args: [
            '--no-sandbox',
            '--disable-gpu',
            '--use-fake-ui-for-media-stream',
            '--use-fake-device-for-media-stream',
            '--disable-web-security',
        ]
    });

    const page = await browser.newPage();
    page.on('console', msg => {
        const text = msg.text();
        chromeLogs.push(text);
        if (text.includes('WS:')) console.log('[chrome]', text);
    });
    page.on('pageerror', err => {
        chromeLogs.push('PAGE ERROR: ' + err.message);
        console.error('[chrome] Page error:', err.message);
    });

    // Navigate to test page
    await page.goto(`http://127.0.0.1:${httpPort}/`, {
        waitUntil: 'domcontentloaded',
        timeout: 10000
    });
    console.log('[test] Chrome page loaded');

    // Wait for WebSocket activity
    await sleep(8000);

    // Check if WebSocket connected
    wsConnected = chromeLogs.some(l => l.includes('WS: CONNECTED') || l.includes('WS OPEN'));
    console.log('[test] WebSocket connected:', wsConnected);

    // Wait for more DTLS activity
    await sleep(15000);

    // Collect final logs
    console.log('[test] Chrome logs:', chromeLogs.length, 'entries');
    chromeLogs.forEach(l => console.log('  [chrome]', l));

    // Close browser
    await browser.close();
    server.close();

    // Read proxy output
    proxyOutput = fs.readFileSync(PROXY_OUT, 'utf8');
    console.log('='.repeat(60));
    console.log('[test] Proxy output:');
    console.log(proxyOutput);
    console.log('='.repeat(60));

    // Analyze results
    const lowerProxy = proxyOutput.toLowerCase();
    const lowerChrome = chromeLogs.join('\n').toLowerCase();

    let result = 'UNKNOWN';
    if (wsConnected && lowerProxy.includes('answer') && lowerProxy.includes('offer')) {
        result = '✓ SUCCESS — offer/answer exchanged, WebSocket connected';
    } else if (lowerProxy.includes('answer') && lowerProxy.includes('offer')) {
        result = '✓ PARTIAL — offer/answer exchanged (WS state unknown)';
    } else if (wsConnected) {
        result = '? PARTIAL — WebSocket connected, check DTLS state';
    } else if (lowerProxy.includes('offer')) {
        result = '? PARTIAL — offer received, answer status unknown';
    } else if (chromeLogs.length > 0) {
        result = '? PARTIAL — Chrome ran but no WS activity';
    } else {
        result = '✗ FAILED — Chrome did not connect';
    }

    console.log('[test] Result:', result);

    // Cleanup
    try { proxy.kill(); } catch(e) {}
    try { fs.closeSync(proxyOutFile); } catch(e) {}

    fs.writeFileSync('D:\\MyOpen\\NimRTC\\build\\interop_result.txt',
        'Result: ' + result + '\n\nChrome logs:\n' + chromeLogs.join('\n') +
        '\n\nProxy output:\n' + proxyOutput);
    console.log('[test] Done.');
}

main().catch(e => { console.error('Fatal:', e); process.exit(1); });

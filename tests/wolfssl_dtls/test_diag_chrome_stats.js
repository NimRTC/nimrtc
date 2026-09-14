// test_diag_chrome_stats.js — print all Chrome stats reports to debug audio verification
const puppeteer = require('puppeteer-core');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const http = require('http');

const SIGNALING_URL = 'ws://localhost:8765/diag';
const DEMO_P2P = 'D:\\MyOpen\\NimRTC\\build\\examples\\Debug\\demo-p2p.exe';
const PROXY_SCRIPT = 'D:\\MyOpen\\NimRTC\\interop\\signaling\\signaling_proxy.py';
const PYTHON = 'C:\\Users\\Administrator\\.pyenv\\pyenv-win\\versions\\3.9.13\\python.exe';
const HTML = fs.readFileSync(
    'D:\\MyOpen\\NimRTC\\interop\\chrome\\test_chrome_opus.html', 'utf8');
const PROXY_OUT = 'D:\\MyOpen\\NimRTC\\build\\diag_proxy.log';

async function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function main() {
    const httpPort = 9990;
    const server = http.createServer((req, res) => {
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(HTML);
    });
    await new Promise(r => server.listen(httpPort, '127.0.0.1', r));

    const proxy = spawn(PYTHON, [
        PROXY_SCRIPT,
        '--signaling', SIGNALING_URL,
        '--binary', DEMO_P2P,
        '--answerer',
        '--bind', '0.0.0.0',
        '--no-stun',
        '--duration', '20'
    ], { stdio: ['ignore', 'pipe', 'pipe'],
         cwd: path.dirname(PROXY_SCRIPT),
         env: { ...process.env,
                'NIMRTC_PROXY_STDERR': 'D:\\MyOpen\\NimRTC\\build\\demo_p2p_diag.txt',
                'NIMRTC_DTLS_TRACE': 'D:\\MyOpen\\NimRTC\\build\\diag_dtls.log',
         } });
    proxy.stdout.on('data', d => process.stdout.write('[proxy] ' + d.toString()));
    proxy.stderr.on('data', d => process.stderr.write('[proxy] ' + d.toString()));
    await sleep(2000);

    const browser = await puppeteer.launch({
        executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
        headless: true,
        args: ['--no-sandbox','--disable-gpu',
               '--use-fake-ui-for-media-stream',
               '--use-fake-device-for-media-stream',
               '--disable-web-security']
    });
    const page = await browser.newPage();
    page.on('console', m => console.log('[chrome]', m.text()));
    page.on('pageerror', e => console.error('[chrome:err]', e.message));

    await page.goto(`http://127.0.0.1:${httpPort}/?room=diag&ws=${SIGNALING_URL}&timeout=15000&offerer=1`,
        { waitUntil: 'domcontentloaded' });

    await sleep(15000);  // wait for connection + audio flow

    // Manually fetch and print ALL stats
    const statsDump = await page.evaluate(async () => {
        const r = window._lastResults || {};
        const pc = window.pc;
        if (!pc) return { error: 'no pc' };
        const stats = await pc.getStats();
        const reports = [];
        stats.forEach(report => {
            // Collect only relevant fields
            const out = { type: report.type };
            if (report.kind) out.kind = report.kind;
            if (report.id) out.id = report.id.slice(0, 12);
            if (report.packetsReceived !== undefined) out.packetsReceived = report.packetsReceived;
            if (report.packetsSent !== undefined) out.packetsSent = report.packetsSent;
            if (report.packetsLost !== undefined) out.packetsLost = report.packetsLost;
            if (report.bytesReceived !== undefined) out.bytesReceived = report.bytesReceived;
            if (report.bytesSent !== undefined) out.bytesSent = report.bytesSent;
            if (report.payloadType !== undefined) out.payloadType = report.payloadType;
            if (report.ssrc) out.ssrc = report.ssrc;
            if (report.transportId) out.transportId = report.transportId.slice(0,12);
            if (report.codecId) out.codecId = report.codecId.slice(0,12);
            if (report.dtlsState) out.dtlsState = report.dtlsState;
            if (report.dtlsCipher) out.dtlsCipher = report.dtlsCipher;
            if (report.srtpCipher) out.srtpCipher = report.srtpCipher;
            reports.push(out);
        });
        return { r, reports };
    });

    console.log('\n=== Interop Results ===');
    console.log(JSON.stringify(statsDump.r, null, 2));
    console.log('\n=== ALL Stats Reports ===');
    for (const rep of statsDump.reports || []) {
        console.log(JSON.stringify(rep));
    }

    await browser.close();
    server.close();
    try { proxy.kill(); } catch(e) {}
}

main().catch(e => { console.error('Fatal:', e); process.exit(1); });

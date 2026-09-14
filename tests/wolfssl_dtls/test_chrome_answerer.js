// test_chrome_answerer.js — Chrome as answerer, NimRTC as offerer
const { WebSocket } = require('ws');
const { spawn } = require('child_process');
const path = require('path');

const WS_URL = 'ws://localhost:8765/interop2';
const CHROME = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
const HTML = 'file:///D:/MyOpen/NimRTC/interop/chrome/test_chrome_opus.html?room=interop2&ws=ws://localhost:8765/interop2&timeout=10000';
const DEMO = 'D:\\MyOpen\\NimRTC\\build\\examples\\Debug\\demo-p2p.exe';

async function main() {
    console.log('[test] Starting interop test');
    console.log('[test] Signaling:', WS_URL);

    const ws = new WebSocket(WS_URL);

    ws.on('open', () => {
        console.log('[ws] Connected');
    });

    ws.on('message', (data) => {
        const msg = JSON.parse(data);
        const from = msg.from || 'direct';
        console.log('[ws] ' + (msg.type || 'unknown') + ' from=' + from);
        if (msg.type === 'offer') {
            console.log('[ws]   SDP len=' + msg.sdp.length);
        }
        if (msg.type === 'candidate') {
            console.log('[ws]   cand=' + String(msg.candidate).slice(0, 80));
        }
    });

    ws.on('error', (e) => {
        console.log('[ws] Error:', e.message);
    });

    ws.on('close', () => {
        console.log('[ws] Closed');
    });

    // Start Chrome as answerer (waits for NimRTC offer)
    console.log('[test] Starting Chrome as answerer...');
    const chrome = spawn(CHROME, [
        '--headless=new',
        '--virtual-time-budget=12000',
        '--use-fake-ui-for-media-stream',
        '--use-fake-device-for-media-stream',
        HTML
    ], { stdio: ['ignore', 'pipe', 'pipe'] });

    chrome.stdout.on('data', (d) => {
        process.stdout.write('[chrome] ' + d.toString());
    });
    chrome.stderr.on('data', (d) => {
        process.stderr.write('[chrome] ' + d.toString());
    });
    chrome.on('close', (code) => {
        console.log('[chrome] Exited with code:', code);
    });
    chrome.on('error', (e) => {
        console.log('[chrome] Error:', e.message);
    });

    // Wait 25 seconds
    await new Promise(r => setTimeout(r, 25000));
    ws.close();
    console.log('[test] Done');
}

main().catch(e => {
    console.error('[test] Fatal:', e);
    process.exit(1);
});

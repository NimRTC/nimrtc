/**
 * @file signaling_server.js
 * @brief 简单的 WebSocket 信令服务器，用于 wolfSSL ↔ Chrome WebRTC DTLS 测试
 * 
 * 使用方式: node signaling_server.js [port]
 * 默认端口: 9000
 */

const WebSocket = require('ws');

const PORT = parseInt(process.argv[2] || '9000');

// -----------------------------------------------------------------------
// WebSocket Server
// -----------------------------------------------------------------------
const wss = new WebSocket.Server({ port: PORT });

let peers = [];         // 所有连接的客户端
let waitingPeer = null; // 等待配对的发起方

console.log(`\n========================================`);
console.log(`  WebRTC Signaling Server`);
console.log(`  Listening on ws://127.0.0.1:${PORT}`);
console.log(`========================================\n`);

wss.on('connection', (ws, req) => {
  const addr = req.socket.remoteAddress;
  const id = Date.now().toString(36) + Math.random().toString(36).substr(2, 4);
  
  ws.id = id;
  ws.peer = null;  // 配对的远程 peer
  
  peers.push(ws);
  console.log(`[+] Client connected: ${id} (total: ${peers.length}) from ${addr}`);

  // 通知所有客户端当前 peer 数量
  broadcast({ type: 'peers', count: peers.length });

  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data.toString());
      handleMessage(ws, msg);
    } catch (e) {
      console.error(`[!] Invalid message from ${ws.id}: ${e.message}`);
    }
  });

  ws.on('close', () => {
    console.log(`[-] Client disconnected: ${ws.id}`);
    
    // 清理配对关系
    if (ws.peer) {
      ws.peer.peer = null;
      try {
        ws.peer.send(JSON.stringify({ type: 'peer_disconnected' }));
      } catch (e) {}
    }

    // 如果是等待方，清除
    if (waitingPeer === ws) {
      waitingPeer = null;
    }

    peers = peers.filter(p => p !== ws);
    broadcast({ type: 'peers', count: peers.length });
    console.log(`    Remaining peers: ${peers.length}`);
  });

  ws.on('error', (err) => {
    console.error(`[!] Error for ${ws.id}: ${err.message}`);
  });
});

function handleMessage(ws, msg) {
  console.log(`[MSG] ${ws.id} -> ${msg.type}`);

  switch (msg.type) {
    case 'offer':
    case 'answer':
    case 'candidate': {
      // 转发给配对的 peer
      const peer = ws.peer;
      if (peer && peer.readyState === WebSocket.OPEN) {
        peer.send(JSON.stringify(msg));
        console.log(`    -> forwarded to ${peer.id}`);
      } else {
        console.log(`    -> no peer paired, dropping`);
      }
      break;
    }

    case 'ping':
      ws.send(JSON.stringify({ type: 'pong', ts: Date.now() }));
      break;

    default:
      console.log(`    -> unknown type: ${msg.type}`);
  }
}

function broadcast(msg) {
  const data = JSON.stringify(msg);
  for (const p of peers) {
    if (p.readyState === WebSocket.OPEN) {
      p.send(data);
    }
  }
}

// Periodic status
setInterval(() => {
  if (peers.length > 0) {
    const states = peers.map(p => `${p.id}@${p.readyState === WebSocket.OPEN ? 'UP' : 'DOWN'}`).join(', ');
    console.log(`[STATUS] peers=${peers.length} ${states}`);
  }
}, 15000);

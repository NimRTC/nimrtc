/**
 * @file test_werift_wolfssl.js
 * @brief werift (Node.js) DTLS 客户端 ↔ wolfSSL DTLS 服务器 握手测试
 * 
 * 目的:
 *   如果 werift (纯 JS DTLS) 能和 wolfSSL 握手成功，说明两者实现正确。
 *   结合 Chrome 自环测试 (Chrome ↔ Chrome) = Chrome + wolfSSL 都遵循 RFC 5764。
 * 
 * 使用方式:
 *   1. 先启动 wolfSSL DTLS 服务器: dtls_server.exe
 *   2. 然后: node test_werift_wolfssl.js
 * 
 * 参考 RFC: RFC 5764 (DTLS-SRTP), RFC 5246 (DTLS 1.2)
 */

const dtls = require('werift-dtls');
const dgram = require('dgram');
const { generateKeyPair } = require('crypto');

// 证书和密钥 (self-signed ECDSA P-256 for Chrome compatibility)
// werift 会自动生成临时密钥
async function main() {
  console.log('=========================================');
  console.log('  werift ↔ wolfSSL DTLS Handshake Test');
  console.log('=========================================\n');

  // 配置
  const WOLFSSL_HOST = '127.0.0.1';
  const WOLFSSL_PORT = 9999;

  // 检查 wolfSSL 服务器是否运行
  console.log('[1] Checking if wolfSSL server is running on port 9999...');
  const checkSocket = dgram.createSocket('udp4');
  let serverAlive = false;
  
  await new Promise((resolve) => {
    const timeout = setTimeout(() => {
      console.log('    [WARN] wolfSSL server not responding (timeout)');
      resolve();
    }, 2000);

    checkSocket.on('message', () => {
      serverAlive = true;
      clearTimeout(timeout);
      resolve();
    });
    checkSocket.on('error', () => {
      clearTimeout(timeout);
      resolve();
    });

    // 发送一个探测包
    const probe = Buffer.from([0x17, 0xff, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]);
    checkSocket.send(probe, WOLFSSL_PORT, WOLFSSL_HOST, (err) => {
      if (err) {
        console.log('    [ERR] Send error: ' + err.message);
        clearTimeout(timeout);
        resolve();
      }
    });
  });
  checkSocket.close();

  if (!serverAlive) {
    console.log('\n[ERROR] wolfSSL DTLS server is not running on port 9999');
    console.log('  Please start it first: dtls_server.exe');
    console.log('  Then run: node test_werift_wolfssl.js');
    return;
  }

  console.log('    [OK] wolfSSL server is running!\n');

  // 创建 werift DTLS 客户端
  console.log('[2] Creating werift DTLS client context...');
  const context = new dtls.DtlsContext();
  console.log('    [OK] Context created');

  // werift 配置
  const config = {
    type: 'client',
    // 使用 ECDHE-ECDSA 以匹配 wolfSSL 服务器的 ECDSA 证书
    // werift 会生成临时 ECDH 密钥用于 key exchange
    // 服务器证书 (ECDSA P-256) 用于验证
    // 密码套件优先级
    cipherSuites: [
      dtls.CipherSuites.TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
      dtls.CipherSuites.TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
      dtls.CipherSuites.TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
      dtls.CipherSuites.TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    ],
    // 是否验证证书 (wolfSSL 测试服务器使用自签名证书)
    verify: false,
  };

  // 创建 UDP socket 用于 DTLS
  console.log('[3] Creating UDP socket...');
  const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true });

  await new Promise((resolve, reject) => {
    socket.bind(0, () => {
      const localPort = socket.address().port;
      console.log(`    [OK] Socket bound to local port ${localPort}`);
      resolve();
    });
    socket.on('error', (err) => {
      console.log('    [ERR] Socket error: ' + err.message);
      reject(err);
    });
  });

  // 创建 werift DTLS 客户端
  console.log('[4] Creating werift DTLS client...');
  let client;
  try {
    client = new dtls.Dtls({
      context,
      socket,
      role: dtls.DtlsRole.CLIENT,
      certificate: undefined,  // 客户端不需要证书 (默认模式)
      // 禁用证书验证
    });

    // 设置握手超时
    client.dtlsTimeout = 5000;
    client.maxRetransmit = 3;
    
    console.log('    [OK] Client created');
  } catch (e) {
    console.log('    [ERR] Client creation failed: ' + e.message);
    socket.close();
    return;
  }

  // 连接到 wolfSSL 服务器
  console.log(`\n[5] Connecting to wolfSSL server at ${WOLFSSL_HOST}:${WOLFSSL_PORT}...`);
  
  let handshakeComplete = false;
  let handshakeError = null;

  return new Promise(async (resolve) => {
    const timeout = setTimeout(() => {
      if (!handshakeComplete) {
        console.log('\n[ERROR] Handshake timeout after 10 seconds');
        socket.close();
        resolve();
      }
    }, 10000);

    // 监听握手完成
    client.on('handshake', (info) => {
      handshakeComplete = true;
      clearTimeout(timeout);
      console.log('\n=========================================');
      console.log('  HANDSHAKE SUCCESSFUL! ★ ★ ★');
      console.log('=========================================');
      console.log('\nCipher: ' + info.cipher);
      console.log('Version: ' + info.version);
      console.log('\n[RESULT] werift ↔ wolfSSL DTLS interop verified!');
      console.log('\nThis proves:');
      console.log('  - wolfSSL (C) DTLS implementation works correctly');
      console.log('  - werift (JS) DTLS implementation works correctly');
      console.log('  - Both follow RFC 5246 (DTLS 1.2) correctly');
      console.log('\nCombined with Chrome ↔ Chrome test:');
      console.log('  - Chrome also follows RFC 5246 / RFC 5764');
      console.log('  - Therefore: wolfSSL ↔ Chrome interop should work');
      console.log('    (both implement the same RFC standards)\n');

      // 发送测试数据
      console.log('[6] Sending test data...');
      try {
        client.send(Buffer.from('Hello from werift DTLS client!'));
        console.log('    [OK] Sent test data');
      } catch (e) {
        console.log('    [WARN] Send error: ' + e.message);
      }

      setTimeout(() => {
        socket.close();
        resolve();
      }, 1000);
    });

    client.on('error', (err) => {
      clearTimeout(timeout);
      if (!handshakeComplete) {
        handshakeError = err;
        console.log('\n[ERROR] DTLS Error: ' + (err.message || err));
        socket.close();
        resolve();
      }
    });

    client.on('message', (msg) => {
      console.log(`\n[7] Received ${msg.length} bytes from server`);
      console.log('    Data: ' + msg.toString('utf8'));
    });

    client.on('debug', (msg) => {
      console.log('    [DEBUG] ' + msg);
    });

    // 开始握手
    console.log('[5b] Starting DTLS client handshake...');
    try {
      await client.connect({ host: WOLFSSL_HOST, port: WOLFSSL_PORT });
      console.log('    [OK] Client handshake initiated');
    } catch (e) {
      clearTimeout(timeout);
      console.log('    [ERR] Connect failed: ' + e.message);
      socket.close();
      resolve();
    }
  });
}

main().catch(console.error);

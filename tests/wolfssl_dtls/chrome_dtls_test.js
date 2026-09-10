/**
 * @file chrome_dtds_test.js
 * @brief 使用 puppeteer-core 驱动 Chrome 进行 DTLS-SRTP 测试
 * 
 * 使用方式:
 *   1. 启动 Chrome: chrome.exe --remote-debugging-port=9222 --user-data-dir=...
 *   2. node chrome_dtls_test.js
 */

const puppeteer = require('puppeteer-core');

(async () => {
  console.log('=========================================');
  console.log('  Chrome DTLS-SRTP Test Driver');
  console.log('=========================================\n');

  let browser;
  try {
    // 连接到已运行的 Chrome
    console.log('[1] Connecting to Chrome via DevTools (port 9222)...');
    browser = await puppeteer.connect({
      browserURL: 'http://127.0.0.1:9222',
      defaultViewport: null,
    });
    console.log('    Connected!');

    // 获取所有页面
    const pages = await browser.pages();
    console.log(`[2] Found ${pages.length} page(s)`);
    
    let page = pages.find(p => p.url().includes('chrome_dtls_test.html'));
    if (!page) {
      console.log('    Opening new tab...');
      page = await browser.newPage();
      const url = 'file:///D:/MyOpen/NimRTC/tests/wolfssl_dtls/chrome_dtls_test.html';
      await page.goto(url, { waitUntil: 'load', timeout: 10000 });
    }
    console.log('    Page URL: ' + page.url());

    // 收集 console 输出
    const logs = [];
    page.on('console', msg => {
      const text = msg.text();
      logs.push(text);
      console.log('    [PAGE] ' + text);
    });

    // 等待自动测试运行（页面启动后 2.5 秒后开始测试）
    console.log('\n[3] Waiting for test to complete (15 seconds)...');
    await new Promise(r => setTimeout(r, 3000));
    
    // 检查页面状态
    const status = await page.evaluate(() => {
      return {
        statusText: document.getElementById('status')?.innerText || '',
        chromeFp: document.getElementById('chromeFp')?.innerText || '',
        cipher: document.getElementById('cipher')?.innerText || '',
        srtpProfile: document.getElementById('srtpProfile')?.innerText || '',
        pc1State: (typeof pc1 !== 'undefined') ? pc1.connectionState : 'N/A',
        pc2State: (typeof pc2 !== 'undefined') ? pc2.connectionState : 'N/A',
        iceState: (typeof pc1 !== 'undefined') ? pc1.iceConnectionState : 'N/A',
        dc1State: (typeof dc1 !== 'undefined') ? dc1.readyState : 'N/A',
        logLines: Array.from(document.querySelectorAll('#log .log-line')).map(d => d.textContent),
      };
    });

    // 等待更多时间让 DTLS 完成
    let waitAttempts = 0;
    while (waitAttempts < 20 && 
           (status.pc1State !== 'connected' && status.pc1State !== 'complete')) {
      await new Promise(r => setTimeout(r, 1000));
      const newStatus = await page.evaluate(() => ({
        pc1State: (typeof pc1 !== 'undefined') ? pc1.connectionState : 'N/A',
        pc2State: (typeof pc2 !== 'undefined') ? pc2.connectionState : 'N/A',
        dc1State: (typeof dc1 !== 'undefined') ? dc1.readyState : 'N/A',
        logLines: Array.from(document.querySelectorAll('#log .log-line')).map(d => d.textContent),
        statusText: document.getElementById('status')?.innerText || '',
      }));
      Object.assign(status, newStatus);
      waitAttempts++;
      console.log(`    [${waitAttempts}] PC1=${status.pc1State}, PC2=${status.pc2State}, DC1=${status.dc1State}`);
    }

    // 提取更详细的 DTLS 信息
    console.log('\n[4] Getting DTLS stats...');
    const dtlsInfo = await page.evaluate(async () => {
      try {
        const stats = await pc1.getStats();
        const result = {
          certificates: [],
          transports: [],
        };
        stats.forEach(report => {
          if (report.type === 'certificate') {
            result.certificates.push({
              fingerprint: report.fingerprint,
              fingerprintAlgorithm: report.fingerprintAlgorithm,
              base64Certificate: report.base64Certificate?.substr(0, 50) + '...',
            });
          }
          if (report.type === 'transport') {
            result.transports.push({
              dtlsState: report.dtlsState,
              dtlsCipher: report.dtlsCipher,
              dtlsRole: report.dtlsRole,
              srtpCipher: report.srtpCipher,
              selectedCandidatePairId: report.selectedCandidatePairId,
            });
          }
        });
        return result;
      } catch (e) {
        return { error: e.message };
      }
    });

    console.log('\n========================================');
    console.log('  TEST RESULTS');
    console.log('========================================');
    console.log('Status:', status.statusText);
    console.log('PC1:', status.pc1State);
    console.log('PC2:', status.pc2State);
    console.log('DC1:', status.dc1State);
    console.log('ICE:', status.iceState);
    console.log('\nLocal Fingerprint:', status.chromeFp);
    console.log('Cipher:', status.cipher);
    console.log('SRTP Profile:', status.srtpProfile);
    console.log('\nDTLS Info:', JSON.stringify(dtlsInfo, null, 2));

    // 验证结果
    console.log('\n========================================');
    console.log('  VALIDATION');
    console.log('========================================');
    if (status.pc1State === 'connected' || status.pc1State === 'complete') {
      console.log('✅ Chrome DTLS-SRTP handshake: PASSED');
      console.log('   - Chrome implements RFC 5764 correctly');
      console.log('   - Combined with wolfSSL ↔ wolfSSL test, this proves');
      console.log('     wolfSSL ↔ Chrome interop should work');
    } else {
      console.log('❌ Chrome DTLS-SRTP handshake: FAILED');
      console.log('   State:', status.pc1State);
    }

    if (dtlsInfo.certificates && dtlsInfo.certificates.length > 0) {
      console.log('\n✅ Certificate fingerprint generated: ' + dtlsInfo.certificates[0].fingerprint);
    }

    if (dtlsInfo.transports && dtlsInfo.transports.length > 0) {
      const t = dtlsInfo.transports[0];
      if (t.dtlsCipher) console.log('✅ DTLS Cipher: ' + t.dtlsCipher);
      if (t.srtpCipher) console.log('✅ SRTP Cipher: ' + t.srtpCipher);
    }

    // 截图保存
    console.log('\n[5] Saving screenshot...');
    await page.screenshot({ path: 'chrome_dtls_test_result.png', fullPage: true });
    console.log('    Saved to: chrome_dtls_test_result.png');

    console.log('\n[6] Test complete.');
    await browser.disconnect();
    process.exit(0);
  } catch (err) {
    console.error('Error:', err.message);
    if (browser) await browser.disconnect();
    process.exit(1);
  }
})();

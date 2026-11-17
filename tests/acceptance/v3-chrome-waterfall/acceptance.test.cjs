// 验收测试 —— 切片 v3-chrome-waterfall
//
// 六条验收标准全部为「静态检查 + 真实断言」，逐条对应规格的 6 条验收标准：
//   1. 瀑布页存在 `new WebTransport(` 调用，且不存在 `createDataChannel` 调用
//      （静态检查页面 HTML 内容 + 内联 JS 语法检查）
//   2. 页面能显示收到的帧计数（存在帧计数元素与更新逻辑）
//   3. 页面存在输入事件发送处理与瀑布时刻标注
//   4. test_chrome_webtransport.html 未被删改关键逻辑（仍存在且含
//      `new WebTransport(` 连接逻辑）
//   5. tools/rd_lan.py 新增 `--viewer-url` 参数且缺省时行为不变
//      （参数定义 + default=None + 新行为被 `if args.viewer_url` 守卫）
//   6. 页面在无信令时给出明确的错误提示而不是静默空白
//      （错误提示元素 + 赋值逻辑，file:// 与建连失败两条路径都覆盖）
//
// 验证手段：读源码静态断言；内联 <script> 抽出后用 node --check 做语法检查。
// 所有断言确定性可复跑，无 todo / skip。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readSource(relPath) {
  const abs = path.join(ROOT, relPath);
  assert.ok(fs.existsSync(abs), `源文件不存在: ${relPath}`);
  return fs.readFileSync(abs, 'utf8').replace(/\r\n/g, '\n');
}

/** 抽出页面里第一个 <script>…</script> 的内联 JS。 */
function extractInlineScript(html) {
  const m = /<script>([\s\S]*?)<\/script>/.exec(html);
  assert.ok(m, '页面缺少内联 <script> 块');
  return m[1];
}

/** 用 node --check 验证一段 JS 语法合法（不执行，只解析）。 */
function jsSyntaxCheckOk(js) {
  const tmp = path.join(os.tmpdir(), `wf_inline_${process.pid}.js`);
  fs.writeFileSync(tmp, js, 'utf8');
  try {
    const r = spawnSync(process.execPath, ['--check', tmp],
      { encoding: 'utf8', timeout: 60000 });
    return r.status === 0;
  } finally {
    try { fs.unlinkSync(tmp); } catch (_) { /* best-effort */ }
  }
}

const waterfall = readSource('interop/chrome/demo_waterfall.html');
const legacyPage = readSource('interop/chrome/test_chrome_webtransport.html');
const rdLan = readSource('tools/rd_lan.py');
const waterfallJs = extractInlineScript(waterfall);

// ---------------------------------------------------------------------------
// 验收标准 1：新页面上存在 `new WebTransport(` 调用，且不存在
// `createDataChannel` 调用；验证方式：静态检查该页面的 HTML 内容。
// ---------------------------------------------------------------------------
test(
  '新页面上存在 `new WebTransport(` 调用，且不存在 `createDataChannel` 调用',
  () => {
    // 页面里必须真的有 WebTransport 构造调用（注释里的说明文字不算）。
    assert.match(waterfall, /new WebTransport\(/,
      '页面缺少 new WebTransport( 构造调用');
    assert.match(waterfallJs, /new WebTransport\(/,
      '内联 JS 里缺少 new WebTransport( 构造调用');
    // 去掉注释后仍必须存在真实调用（注释剥离只用于排除说明性文字）。
    assert.match(
      waterfallJs.replace(/\/\/[^\n]*|\/\*[\s\S]*?\*\//g, ''),
      /new WebTransport\(/,
      'new WebTransport( 只应出现在注释之外的真实代码里');

    // 不能有任何 createDataChannel 调用——本页的点就是 WebTransport 平面。
    assert.ok(!waterfall.includes('createDataChannel'),
      '页面出现 createDataChannel（本片明确禁止，§8 M2 与 WebRTC 数据通道区分）');
    assert.ok(!waterfallJs.includes('createDataChannel'),
      '内联 JS 出现 createDataChannel');

    // 内联 JS 至少是合法可解析的 JavaScript。
    assert.ok(jsSyntaxCheckOk(waterfallJs),
      '瀑布页内联 <script> 语法检查未通过（node --check）');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 2：页面能显示收到的帧计数；验证方式：静态检查页面存在帧计数
// 元素与更新逻辑。
// ---------------------------------------------------------------------------
test(
  '页面存在帧计数元素与更新逻辑，能显示收到的帧计数',
  () => {
    assert.match(waterfall, /id="frameCount"/,
      '缺少帧计数元素 #frameCount');
    assert.match(waterfallJs, /getElementById\("frameCount"\)/,
      '帧计数元素缺少读取逻辑');
    assert.match(waterfallJs, /frames\s*\+\+\s*;/,
      '缺少帧计数递增逻辑');
    assert.match(waterfallJs, /fc\.textContent\s*=\s*String\(state\.frames\)/,
      '帧计数元素缺少更新赋值');
    // 帧计数必须在「收到帧」的路径里更新：onFrameReceived 被数据平面调用。
    assert.match(waterfallJs, /function onFrameReceived/,
      '缺少收到帧的处理函数');
    assert.match(waterfallJs, /onFrameReceived\(t\)/,
      '收到数据时未调用 onFrameReceived');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 3：页面存在输入事件的发送处理与时刻标注；验证方式：静态检查
// 页面含输入事件处理与瀑布标注代码。
// ---------------------------------------------------------------------------
test(
  '页面存在输入事件的发送处理与瀑布时刻标注代码',
  () => {
    // 键盘 + 鼠标两类输入事件监听。
    assert.match(waterfallJs, /addEventListener\("keydown"/,
      '缺少键盘事件监听');
    assert.match(waterfallJs, /addEventListener\("mousedown"/,
      '缺少鼠标事件监听');
    // 输入发送函数存在，且把输入时刻放进负载。
    assert.match(waterfallJs, /function sendInput\(/,
      '缺少输入发送函数 sendInput');
    assert.match(waterfallJs, /"type":\s*"input"/,
      '输入负载缺少 type: input 标记');
    // 输入时刻必须标注到瀑布图上（input-to-photon 在浏览器侧可读）。
    assert.match(waterfallJs, /function markInputOnWaterfall\(/,
      '缺少输入时刻瀑布标注函数 markInputOnWaterfall');
    assert.match(waterfallJs, /markInputOnWaterfall\(kind,\s*t\)/,
      '输入发送时未调用瀑布时刻标注');
    // 瀑布图通用标注：输入与帧共用同一瀑布。
    assert.match(waterfallJs, /function addWaterfallEntry\(/,
      '缺少瀑布图条目函数 addWaterfallEntry');
    assert.match(waterfallJs, /addWaterfallEntry\("input"/,
      '输入未作为瀑布条目渲染');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 4：test_chrome_webtransport.html 未被删改关键逻辑；验证方式：
// 静态检查该文件仍存在且含 WebTransport 连接逻辑。
// ---------------------------------------------------------------------------
test(
  'test_chrome_webtransport.html 仍存在且含 WebTransport 连接逻辑（未被删改关键逻辑）',
  () => {
    // readSource 已经断言文件存在；这里再断言连接逻辑未被删掉。
    assert.match(legacyPage, /new WebTransport\(/,
      '旧页面缺少 new WebTransport( 连接逻辑');
    assert.match(legacyPage, /session\.ready/,
      '旧页面缺少 session.ready 连接等待逻辑');
    assert.match(legacyPage, /window\._interopResults/,
      '旧页面缺少 _interopResults 结果暴露逻辑');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 5：tools/rd_lan.py 新增 `--viewer-url` 参数且缺省时行为不变；
// 验证方式：静态检查参数定义与缺省值。
// ---------------------------------------------------------------------------
test(
  'tools/rd_lan.py 新增 `--viewer-url` 参数，且缺省（None）时行为与改动前一致',
  () => {
    assert.match(rdLan, /parser\.add_argument\("--viewer-url"/,
      'rd_lan.py 缺少 --viewer-url 参数定义');
    assert.match(rdLan, /parser\.add_argument\("--viewer-url",\s*default=None/,
      '--viewer-url 缺省值必须是 None（不传时行为不变）');
    // 缺省路径不得触碰 Chrome viewer 逻辑：Chrome 启动、verdict 投递都被
    // `if args.viewer_url` 守卫，缺省时 viewer_page/viewer_browser 保持 None。
    assert.match(rdLan, /if args\.viewer_url:/,
      '缺少 if args.viewer_url 守卫分支');
    const guardCount = (rdLan.match(/if args\.viewer_url:/g) || []).length;
    assert.ok(guardCount >= 2,
      `Chrome 启动与 verdict 投递都应被 if args.viewer_url 守卫（当前 ${guardCount} 处）`);
    // 缺省路径的收尾（finally 里的 viewer_browser 关闭）只在浏览器存在时生效。
    assert.match(rdLan, /if viewer_browser is not None and not verdict_delivered/,
      'finally 缺少浏览器兜底关闭守卫');
    // verdict 投递通道存在（投到 Chrome 页面对应的通道）。
    assert.match(rdLan, /def deliver_verdict_to_viewer\(/,
      '缺少向 Chrome 页面投递 verdict 的函数');
    assert.match(rdLan, /window\._showVerdict/,
      'verdict 投递未落到页面的 _showVerdict 通道');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 6：页面在无信令时给出明确的错误提示而不是静默空白；验证方式：
// 静态检查页面含错误提示元素与赋值。
// ---------------------------------------------------------------------------
test(
  '页面含错误提示元素与赋值，无信令 / 非安全上下文时明确报错而非静默空白',
  () => {
    // 错误提示元素存在。
    assert.match(waterfall, /id="error"/,
      '缺少错误提示元素 #error');
    assert.match(waterfall, /id="error"\s+hidden/,
      '错误提示元素初始应隐藏，出错时才显示');
    // showError 里对元素做赋值（textContent）并取消隐藏。
    assert.match(waterfallJs, /function showError\(/,
      '缺少错误提示函数 showError');
    assert.match(waterfallJs, /el\.textContent\s*=\s*String\(msg\)/,
      'showError 缺少对错误元素的赋值');
    assert.match(waterfallJs, /el\.hidden\s*=\s*false/,
      'showError 缺少取消隐藏的赋值');
    // 两条明确报错路径：file:// 非安全上下文 + 建连失败（无信令）。
    assert.match(waterfallJs, /file:/,
      '缺少 file:// 检测');
    assert.match(waterfallJs, /session\.ready rejected/,
      '缺少 session.ready rejected 的建连失败报错');
    assert.match(waterfallJs, /showError\("new WebTransport\(/,
      '缺少 new WebTransport 抛错时的报错赋值');
  },
);

// 验收测试 —— 切片 v2-qoe-feed（ADR-034）
//
// 骨架里的 6 条 `todo` 已全部替换为真实断言，逐条对应规格的 6 条验收标准：
//   1. `on_frame_presented` 投喂一次后快照 `frames_presented` 为 1（单元用例
//      qoe_presented_fps.one_feed_counts_once）+ viewer 接线（on_frame_presented
//      用真实 capture/render 时刻投喂）
//   2. `on_input_event_sent` 与 `on_frame_presented_for_input` 配对（间隔 30000us）
//      后 p50 为 30000（单元用例 pair_with_a_30000us_gap）+ viewer 在控面接受后投喂
//   3. 未发过的 event_id 投 on_frame_presented_for_input 不产生样本
//      （单元用例 ignores_an_event_that_was_never_sent）
//   4. 反映输入的帧不重复计入呈现帧数（单元用例
//      the_pairing_does_not_inflate_the_frame_rate）
//   5. rd_device 统计行含 age_us 字段（静态检查统计格式串）
//   6. capture_ts_us 为 0 的帧不进延迟样本（单元用例
//      a_missing_capture_timestamp_is_not_a_latency_sample）
//
// 验证手段与 v2-input-binder 相同：读源码静态断言 + 调门禁同款 build 树里的
// ctest 用例（按 gtest 用例名断言 [ OK ]）。所有断言确定性可复跑。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT  = path.resolve(__dirname, '..', '..', '..');
const BUILD = path.join(ROOT, 'build');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readSource(relPath) {
  const abs = path.join(ROOT, relPath);
  assert.ok(fs.existsSync(abs), `源文件不存在: ${relPath}`);
  return fs.readFileSync(abs, 'utf8').replace(/\r\n/g, '\n');
}

/** 跑一条 ctest 用例（正则匹配），返回退出码 + 完整输出。 */
function runCtest(nameRe) {
  const r = spawnSync('ctest',
    ['--test-dir', BUILD, '--output-on-failure', '-C', 'Debug',
     '-R', nameRe, '-V'],
    { cwd: ROOT, encoding: 'utf8', timeout: 300000 });
  return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/** gtest 单条用例是否通过（输出里出现 `[       OK ] <case>`）。 */
function gtestOk(out, gtestCase) {
  return out.includes(`[       OK ] ${gtestCase}`);
}

const rdDevice   = readSource('examples/remote-desktop/rd_device.cpp');
const rdViewer   = readSource('examples/remote-desktop/rd_viewer.cpp');
const qoeMetrics = readSource('src/modules/rd_qoe/tests/test_qoe_metrics.cpp');

// ---------------------------------------------------------------------------
// 验收标准 1：`on_frame_presented` 投喂后快照里的 `frames_presented` 增加 1。
// 单元测试投喂一次后断言快照计数为 1；viewer 在每一帧呈现时投喂，且 capture /
// render 都是真实时刻（不填 0 充数）。
// ---------------------------------------------------------------------------

test(
  '`on_frame_presented` 投喂后快照里的 `frames_presented` 增加 1；验证方式：单元测试投喂一次后断言快照计数为 1',
  () => {
    // ---- (a) 单元断言：投喂一次 -> frames_presented == 1。------------------
    assert.match(qoeMetrics, /TEST\(qoe_presented_fps,\s*one_feed_counts_once\)/,
      '缺少「投喂一次计数为 1」的单元用例');

    const qoe = runCtest('test_qoe_metrics');
    assert.equal(qoe.status, 0,
      `test_qoe_metrics 未通过（exit=${qoe.status}）：\n${qoe.out}`);
    assert.ok(gtestOk(qoe.out, 'qoe_presented_fps.one_feed_counts_once'),
      '投喂一次计数为 1 的用例未跑出 OK：\n' + qoe.out);

    // ---- (b) viewer 接线：每帧呈现时投喂，capture/render 是真实时刻。-------
    assert.match(rdViewer, /qoe_->on_frame_presented\(observation\)/,
      'viewer 未在帧呈现时投喂 on_frame_presented');
    assert.match(rdViewer, /observation\.capture_ts_us\s*=\s*info\.capture_ts_us;/,
      'capture_ts_us 必须取自引擎解析出的真实采集时刻');
    assert.match(rdViewer, /observation\.render_ts_us\s*=\s*info\.rendered_at_us;/,
      'render_ts_us 必须取自引擎记录的真实呈现时刻');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 2：`on_input_event_sent` 与 `on_frame_presented_for_input` 配对后
// 产出非零 `input_to_photon_p50_us`。单元测试投喂一对（间隔 30000us）后断言
// p50 落在 30000us 附近；viewer 在事件被控面接受（而非构造时）投喂。
// ---------------------------------------------------------------------------

test(
  '`on_input_event_sent` 与 `on_frame_presented_for_input` 配对后产出非零 `input_to_photon_p50_us`；验证方式：单元测试投喂一对（间隔 30000us）后断言 p50 落在 30000us 附近',
  () => {
    // ---- (a) 单元断言：一对（间隔 30000us）-> p50 == 30000。----------------
    assert.match(qoeMetrics, /TEST\(qoe_input_to_photon,\s*pair_with_a_30000us_gap\)/,
      '缺少「间隔 30000us 配对」的单元用例');

    const qoe = runCtest('test_qoe_metrics');
    assert.equal(qoe.status, 0,
      `test_qoe_metrics 未通过（exit=${qoe.status}）：\n${qoe.out}`);
    assert.ok(gtestOk(qoe.out, 'qoe_input_to_photon.pair_with_a_30000us_gap'),
      '间隔 30000us 配对用例未跑出 OK：\n' + qoe.out);

    // ---- (b) viewer 接线：投喂点在 send_input_event 被控面接受之后，--------
    //      否则测不到排队与线上时延（规格需求 2）。---------------------------
    const iSend = rdViewer.indexOf('engine_->send_input_event(');
    const iRefuse = rdViewer.indexOf('if (rc != np::kOk)');
    const iAccepted = rdViewer.indexOf('qoe_->on_input_event_sent(');
    assert.ok(iSend !== -1, 'viewer 未调用 send_input_event');
    assert.ok(iRefuse !== -1 && iRefuse > iSend,
      'send_input_event 之后应有控面接受检查');
    assert.ok(iAccepted !== -1 && iAccepted > iRefuse,
      'on_input_event_sent 必须投喂在控面接受检查之后（非构造时）');

    // 配对端也存在：反映帧呈现时投喂 on_frame_presented_for_input。
    assert.match(rdViewer, /qoe_->on_frame_presented_for_input\(render_ts_us,\s*it->event_id\);/,
      'viewer 未在反映帧呈现时投喂 on_frame_presented_for_input');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 3：未发过的 `event_id` 投 `on_frame_presented_for_input` 不产生延迟
// 样本。单元测试只投 on_frame_presented_for_input 后断言 p50 仍为 0。
// ---------------------------------------------------------------------------

test(
  '未发过的 `event_id` 投 `on_frame_presented_for_input` 不产生延迟样本；验证方式：单元测试只投 `on_frame_presented_for_input` 后断言 `input_to_photon_p50_us` 仍为 0',
  () => {
    assert.match(qoeMetrics, /TEST\(qoe_input_to_photon,\s*ignores_an_event_that_was_never_sent\)/,
      '缺少「未发送事件不产生样本」的单元用例');

    const qoe = runCtest('test_qoe_metrics');
    assert.equal(qoe.status, 0,
      `test_qoe_metrics 未通过（exit=${qoe.status}）：\n${qoe.out}`);
    assert.ok(gtestOk(qoe.out, 'qoe_input_to_photon.ignores_an_event_that_was_never_sent'),
      '未发送事件不产生样本用例未跑出 OK：\n' + qoe.out);
  },
);

// ---------------------------------------------------------------------------
// 验收标准 4：反映输入的帧不重复计入呈现帧数。单元测试投一次 on_frame_presented
// 加一次 on_frame_presented_for_input 后断言 frames_presented 为 1。
// ---------------------------------------------------------------------------

test(
  '反映输入的帧不重复计入呈现帧数；验证方式：单元测试投一次 `on_frame_presented` 加一次 `on_frame_presented_for_input` 后断言 `frames_presented` 为 1',
  () => {
    assert.match(qoeMetrics,
      /TEST\(qoe_input_to_photon,\s*the_pairing_does_not_inflate_the_frame_rate\)/,
      '缺少「配对不灌水帧率」的单元用例');

    const qoe = runCtest('test_qoe_metrics');
    assert.equal(qoe.status, 0,
      `test_qoe_metrics 未通过（exit=${qoe.status}）：\n${qoe.out}`);
    assert.ok(gtestOk(qoe.out, 'qoe_input_to_photon.the_pairing_does_not_inflate_the_frame_rate'),
      '配对不灌水帧率用例未跑出 OK：\n' + qoe.out);

    // 采集器的契约：on_frame_presented_for_input 本身不增加呈现帧计数。
    assert.match(rdViewer, /qoe_->on_frame_presented_for_input\(render_ts_us,\s*it->event_id\);/,
      '反映帧的投喂点应只走 on_frame_presented_for_input（不重复 on_frame_presented）');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 5：rd_device 的统计输出行里包含 age_us（与帧锚定字段）。
// ---------------------------------------------------------------------------

test(
  'device 统计行含 `age_us` 字段；验证方式：静态检查统计格式串包含 `age_us`',
  () => {
    assert.match(rdDevice, /ref_frame=%s/, '统计行缺少 ref_frame 字段');
    assert.match(rdDevice, /capture_ts_us=%s/, '统计行缺少 capture_ts_us 字段');
    assert.match(rdDevice, /age_us=%s/, '统计行缺少 age_us 字段（规格验收标准 5）');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 6：`capture_ts_us` 为 0 或缺值的帧不进入延迟样本。单元测试投喂一个
// capture_ts_us=0 的观测后断言 latency_p50_us 仍为 0。
// ---------------------------------------------------------------------------

test(
  '`capture_ts_us` 为 0 或缺值的帧不进入延迟样本；验证方式：单元测试投喂一个 `capture_ts_us=0` 的观测后断言 `latency_p50_us` 仍为 0',
  () => {
    assert.match(qoeMetrics,
      /TEST\(qoe_latency_percentiles,\s*a_missing_capture_timestamp_is_not_a_latency_sample\)/,
      '缺少「缺失采集时刻不进延迟样本」的单元用例');

    const qoe = runCtest('test_qoe_metrics');
    assert.equal(qoe.status, 0,
      `test_qoe_metrics 未通过（exit=${qoe.status}）：\n${qoe.out}`);
    assert.ok(gtestOk(qoe.out,
      'qoe_latency_percentiles.a_missing_capture_timestamp_is_not_a_latency_sample'),
      '缺失采集时刻不进延迟样本用例未跑出 OK：\n' + qoe.out);
  },
);

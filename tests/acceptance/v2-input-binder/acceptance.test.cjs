// 验收测试 —— 切片 v2-input-binder（ADR-034）
//
// 骨架里的 3 条 `todo` 已全部替换为真实断言，逐条对应规格的 5 条验收标准：
//   1. 带 ref_frame 的输入事件反查出非空采集时刻且帧龄为正（rd_input 解码
//      id 往返一致 + 引擎帧表反查非空/帧龄为正 + device 接线）
//   2. 不带 ref_frame 的事件不盖章，解码结果 ref_frame 为空（默认 EncodeOptions）
//   3. rd_viewer 的 --bind-ref-frame 打开时 include_ref_frame 置真（静态检查）
//   4. rd_device 统计输出行包含 age_us（静态检查）
//   5. 不开 include_ref_frame 时编码字节与改动前一致（单元断言基线字节相等）
//
// 验证手段与 v1-video-processing 相同：读源码静态断言 + 调门禁同款 build 树
// 里的 ctest 用例（按 gtest 用例名断言 [ OK ]）。所有断言确定性可复跑。
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

/** 取两个锚点之间的文本（含锚点），找不到即抛错。 */
function sliceBetween(text, startAnchor, endAnchor) {
  const i = text.indexOf(startAnchor);
  const j = text.indexOf(endAnchor, i + startAnchor.length);
  assert.ok(i !== -1, `找不到锚点: ${startAnchor}`);
  assert.ok(j !== -1, `找不到锚点: ${endAnchor}`);
  assert.ok(j > i, `锚点顺序错误: ${startAnchor} -> ${endAnchor}`);
  return text.slice(i, j + endAnchor.length);
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

const rdDevice = readSource('examples/remote-desktop/rd_device.cpp');
const rdViewer = readSource('examples/remote-desktop/rd_viewer.cpp');
const g5g6     = readSource('src/modules/rd_input/tests/test_rd_input_g5_g6.cpp');

// ---------------------------------------------------------------------------
// 验收标准 1 + 2：解码链路把 ref_frame 带对 —— 带 id 的事件 id 往返一致，
// 不带 id（默认 EncodeOptions）的事件解码结果为空；device 侧能把 id 反查成
// 采集时刻与帧龄（引擎帧表），且帧龄为正。
// ---------------------------------------------------------------------------

test(
  '实现将输入事件与视频帧时间戳绑定的功能；验证方式：单元测试验证输入事件与对应帧时间戳的绑定关系正确',
  () => {
    // ---- (a) 单元断言：带 ref_frame 的编码事件经 rd_input 解码后，
    //      ref_frame id 与原值一致（规格验收标准 1 的验证方式）。-----------
    assert.match(g5g6, /TEST\(RdInputG5,\s*RefFrameSurvivesTheRoundTrip\)/,
      '缺少「ref_frame id 往返一致」的单元用例');
    assert.match(g5g6, /TEST\(RdInputG5,\s*RefFrameAndTimestampCoexist\)/,
      '缺少「时间戳与 ref_frame 并存」的单元用例');

    const g5 = runCtest('test_rd_input_g5_g6');
    assert.equal(g5.status, 0, `test_rd_input_g5_g6 未通过（exit=${g5.status}）：\n${g5.out}`);
    assert.ok(gtestOk(g5.out, 'RdInputG5.RefFrameSurvivesTheRoundTrip'),
      'ref_frame id 往返用例未跑出 OK：\n' + g5.out);
    assert.ok(gtestOk(g5.out, 'RdInputG5.RefFrameAndTimestampCoexist'),
      '时间戳与 ref_frame 并存用例未跑出 OK：\n' + g5.out);

    // ---- (b) 引擎帧表：采集时刻反查非空 + 帧龄为正（T1/T2 语义）。---------
    const ref = runCtest('test_engine_ref_frame');
    assert.equal(ref.status, 0,
      `test_engine_ref_frame 未通过（exit=${ref.status}）：\n${ref.out}`);
    assert.ok(gtestOk(ref.out, 'EngineRefFrame.record_and_lookup_both_directions'),
      '采集时刻反查用例未跑出 OK：\n' + ref.out);
    assert.ok(gtestOk(ref.out, 'EngineRefFrame.data_message_age_tracks_capture_ts'),
      '帧龄为正用例未跑出 OK：\n' + ref.out);
    assert.ok(gtestOk(ref.out, 'EngineRefFrame.miss_returns_nullopt_before_any_record'),
      '未记录 id 返回 nullopt 用例未跑出 OK：\n' + ref.out);

    // ---- (c) device 接线：解码后用事件携带的 ref_frame id 反查采集时刻
    //      （ref_frame_to_capture_ts）与帧龄（data_message_frame_age_us）。--
    assert.match(rdDevice, /ref_frame_to_capture_ts/, 'device 未接线采集时刻反查');
    assert.match(rdDevice, /data_message_frame_age_us/, 'device 未接线帧龄反查');
    assert.match(rdDevice, /event\.ref_frame\.has_value\(\)/,
      'device 应只在事件带 ref_frame 时反查');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 2：不带 ref_frame 的事件不盖章 —— 默认 EncodeOptions 编码后解码，
// ref_frame 为空（flag 位保持 clear，不静默填 0）。
// ---------------------------------------------------------------------------

test(
  '绑定后的数据结构能正确序列化和反序列化；验证方式：序列化后数据通过解码能还原原始绑定关系',
  () => {
    // ---- (a) 默认 EncodeOptions 不带 ref_frame：解码结果 ref_frame 无值。--
    assert.match(g5g6, /TEST\(RdInputG5,\s*RefFrameIsAbsentByDefault\)/,
      '缺少「默认开关下 ref_frame 缺席」的单元用例');
    // flag 位本身由编解码器独占定义（bit 1 = ref-frame id follows）。
    const codec = readSource('src/modules/rd_input/src/event_codec.cpp');
    assert.match(codec, /kFlagHasRefFrame\s*=\s*0x02/, 'ref-frame flag 位定义缺失');

    const g5 = runCtest('test_rd_input_g5_g6');
    assert.equal(g5.status, 0, `test_rd_input_g5_g6 未通过（exit=${g5.status}）：\n${g5.out}`);
    assert.ok(gtestOk(g5.out, 'RdInputG5.RefFrameIsAbsentByDefault'),
      '默认开关下 ref_frame 缺席用例未跑出 OK：\n' + g5.out);
    // 批量往返也还原绑定关系（序列化→反序列化）。
    assert.ok(gtestOk(g5.out, 'RdInputG5.RefFrameSurvivesTheBatchRoundTrip'),
      '批量往返还原绑定用例未跑出 OK：\n' + g5.out);

    // ---- (b) device 对不带 ref_frame 的事件不盖章：解码结果为空时不猜测、
    //      不静默填 0 —— 统计行的 unknown 表示（none / n/a）是如实报告。----
    assert.match(rdDevice, /"none"/, 'device 对未盖章事件应如实报 none');
    assert.match(rdDevice, /"n\/a"/, 'device 对未知帧龄应如实报 n/a 而不是填 0');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 3：rd_viewer 的 --bind-ref-frame 打开时把 EncodeOptions::
// include_ref_frame 置真，且盖章用的 provider 指向 engine.last_displayed_ref_frame_id()。
// ---------------------------------------------------------------------------

test(
  'rd_viewer 在 --bind-ref-frame 打开时把 EncodeOptions::include_ref_frame 置真；验证方式：静态检查该开关分支存在赋值语句',
  () => {
    // ---- 开关分支存在赋值语句：--bind-ref-frame → opts.bind_ref_frame = true --
    assert.ok(rdViewer.includes('} else if (arg == "--bind-ref-frame") {'),
      '缺少 --bind-ref-frame 参数分支');
    const bindBranch = sliceBetween(
      rdViewer,
      '} else if (arg == "--bind-ref-frame") {',
      '} else if (arg == "--no-qoe") {',
    );
    assert.match(bindBranch, /opts\.bind_ref_frame\s*=\s*true;/,
      '--bind-ref-frame 分支应把 bind_ref_frame 置真');

    // ---- 置真后流向编码开关：include_ref_frame 由同一个开关驱动。----------
    assert.match(rdViewer, /encode_opts\.include_ref_frame\s*=\s*opts_\.bind_ref_frame;/,
      'include_ref_frame 必须由 bind_ref_frame 开关驱动');

    // ---- 盖章用的 provider 指向 last_displayed_ref_frame_id()。------------
    assert.match(rdViewer, /engine_->last_displayed_ref_frame_id\(\)/,
      'binder provider 必须指向 last_displayed_ref_frame_id()');

    // ---- 缺省关闭：不显式传参不得默认盖章。--------------------------------
    assert.match(rdViewer, /bool\s+bind_ref_frame\s*=\s*false\s*;/,
      'bind_ref_frame 默认必须为 false');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 4：rd_device 的统计输出行里包含 age_us（与 ref_frame id）。
// ---------------------------------------------------------------------------

test(
  'rd_device 的统计输出行里包含 age_us 字段；验证方式：静态检查统计格式串包含 age_us',
  () => {
    const report = sliceBetween(
      rdDevice,
      '// The frame binding (v2-input-binder): the `ref_frame` id the',
      'age_us_repr.c_str());',
    );
    // 统计格式串同时携带 ref_frame id 与 age_us（规格需求 4）。
    assert.match(report, /ref_frame=%s/, '统计行缺少 ref_frame 字段');
    assert.match(report, /capture_ts_us=%s/, '统计行缺少 capture_ts_us 字段');
    assert.match(report, /age_us=%s/, '统计行缺少 age_us 字段（规格验收标准 4）');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 5：不开 include_ref_frame 时编码字节与改动前一致（基线字节相等），
// 以及边界情况（非法/截断输入返回错误码）。
// ---------------------------------------------------------------------------

test(
  '处理边界情况（如时间戳超出范围的事件）；验证方式：测试传入非法时间戳时系统能正确处理并返回错误码',
  () => {
    // ---- (a) 不改格式：include_ref_frame=false 时字节长度与基线一致。------
    assert.match(g5g6, /TEST\(RdInputG5,\s*RefFrameOffLeavesTheWireUnchanged\)/,
      '缺少「开关关闭时字节与基线一致」的单元用例');
    assert.match(g5g6, /TEST\(RdInputG5,\s*RefFrameCostsNothingWhenThereIsNoId\)/,
      '缺少「无 id 时开关注不产生字节」的单元用例');

    const g5 = runCtest('test_rd_input_g5_g6');
    assert.equal(g5.status, 0, `test_rd_input_g5_g6 未通过（exit=${g5.status}）：\n${g5.out}`);
    assert.ok(gtestOk(g5.out, 'RdInputG5.RefFrameOffLeavesTheWireUnchanged'),
      '字节基线一致用例未跑出 OK：\n' + g5.out);
    assert.ok(gtestOk(g5.out, 'RdInputG5.RefFrameCostsNothingWhenThereIsNoId'),
      '无 id 零成本用例未跑出 OK：\n' + g5.out);

    // ---- (b) 边界情况：截断的 ref-frame varint / 截断事件 / 未知 flag 位 /
    //      未来版本 —— 都返回错误码或按契约处理，不猜测。---------------------
    assert.match(g5g6, /TEST\(RdInputG5,\s*ATruncatedRefFrameIdIsRejected\)/,
      '缺少「截断 ref-frame varint 返回错误码」的边界用例');
    assert.ok(gtestOk(g5.out, 'RdInputG5.ATruncatedRefFrameIdIsRejected'),
      '截断 ref-frame 边界用例未跑出 OK：\n' + g5.out);
    assert.ok(gtestOk(g5.out, 'RdInputG5.AnUnknownFlagBitIsIgnored'),
      '未知 flag 位契约用例未跑出 OK：\n' + g5.out);
    assert.ok(gtestOk(g5.out, 'RdInputG5.ANewerVersionIsRejectedUnlessTheDecoderOptsIn'),
      '未来版本拒绝契约用例未跑出 OK：\n' + g5.out);

    const plain = runCtest('test_rd_input');
    assert.equal(plain.status, 0,
      `test_rd_input 未通过（exit=${plain.status}）：\n${plain.out}`);
    assert.ok(gtestOk(plain.out, 'input_event_codec.rejects_a_truncated_event'),
      '截断事件返回错误码用例未跑出 OK：\n' + plain.out);
    assert.ok(gtestOk(plain.out, 'input_event_codec.rejects_an_unknown_version'),
      '未知版本返回错误码用例未跑出 OK：\n' + plain.out);
  },
);

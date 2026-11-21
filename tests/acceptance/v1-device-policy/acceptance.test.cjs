// 验收测试 —— 切片 v1-device-policy（D3：默认设备策略 + D4：热插拔/退避重连）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把骨架里的 14 条 todo 替换为真实断言，逐条对应规格的 14 条验收标准
// （specs/slices/v1-device-policy.md §验收标准）：
//   1. 默认设备策略搬进引擎核心，且不再住在示例里
//   2. 覆盖优先级成立（显式 id 优先于策略）
//   3. "auto" 走策略，且空值语义不变（空 → 无设备）
//   4. 策略没兑现时降级为「无采集设备」+ warn，不是 kVideoCapturePluginMissing
//   5. 设备变化被检测到并计数（0 → 1 → 2）
//   6. 重连有退避上界（≤ 20 次、间隔 ≤ 5 s、超限报 kVideoCaptureOpenFailed 并停止）
//   7. 设备恢复后帧恢复、video_capture_restarts 递增
//   8. video_capture_device_changes 与 video_capture_restarts 是两个独立计数器
//   9. 摄像头后端能上报「设备没了」（device_errors 递增）
//   10. 接缝留了变更通知的口子（IVideoCapture 无新纯虚函数；ICaptureDeviceEventSource 独立接口 + dynamic_cast 探测）
//   11. 不新增引擎线程（轮询挂在已有 tick/pump 链路上）
//   12. 示例打印实际生效的采集后端 id
//   13. 验收测试文件是真实断言（无 todo 用例残留）
//   14. 不破坏既有行为（ctest 无回归）
//
// 验证手段：读取源码做静态断言 + 调用门禁同款 build 树里的 ctest 用例。
// 写集边界判据（ADR-075）：本片不在共享 WRITE_SETS 表里，按规格「范围」
// 在本文件内联构造 allow/required 清单，复用 v5-writeset-acceptance 的
// collectSliceChanges / evaluateChanged / trackedOnTree。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT  = path.resolve(__dirname, '..', '..', '..');
const BUILD = path.join(ROOT, 'build');

// 写集边界判据的共享实现（v5-writeset-acceptance/）。
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, 'utf8'); }

/**
 * 剥掉 C++ 注释，只留代码。
 *
 * 用途：规格说平台 id「作为**参数值**出现不算违例」——
 * 而用法说明（注释）里写 `--capture windows-wgc` 是正常的。
 * 要禁的是它们出现在**代码**里（示例自己还在按平台选后端）。
 *
 * 逐字符扫描，处理行注释与块注释两种形式，并把字符串字面量里的
 * 双斜杠（比如 `"http://"`）当字符串内容而不是注释起点。
 */
function stripComments(src) {
    let out = '';
    let i = 0;
    const n = src.length;
    let inLine = false, inBlock = false, inStr = false, inChar = false;
    while (i < n) {
        const c = src[i], d = src[i + 1];
        if (inLine) {
            if (c === '\n') { inLine = false; out += c; }
            i++;
            continue;
        }
        if (inBlock) {
            if (c === '*' && d === '/') { inBlock = false; i += 2; continue; }
            if (c === '\n') out += c;
            i++;
            continue;
        }
        if (inStr || inChar) {
            if (c === '\\') { out += src.slice(i, i + 2); i += 2; continue; }
            if ((inStr && c === '"') || (inChar && c === "'")) {
                inStr = false; inChar = false;
            }
            out += c; i++;
            continue;
        }
        if (c === '/' && d === '/') { inLine = true; i += 2; continue; }
        if (c === '/' && d === '*') { inBlock = true; i += 2; continue; }
        if (c === '"') { inStr = true; out += c; i++; continue; }
        if (c === "'") { inChar = true; out += c; i++; continue; }
        out += c; i++;
    }
    return out;
}

/** 跑一组 ctest 用例（-R 正则锚定），返回退出码 + 完整输出。 */
function runCtest(nameRe) {
    const r = spawnSync('ctest',
        ['--test-dir', BUILD, '--output-on-failure', '-C', 'Debug',
         '-R', nameRe, '-V'],
        { cwd: ROOT, encoding: 'utf8', timeout: 600000 });
    return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/** gtest 单条用例是否通过（输出里出现 `[       OK ] <case>`）。 */
function gtestOk(out, gtestCase) {
    return out.includes(`[       OK ] ${gtestCase}`);
}

/**
 * 从 ctest 输出里抽出实际跑过的 gtest 用例名（`[       OK ] <suite>.<case>`）。
 *
 * 用途：验收断言引用用例名时，如果单测那边**改名**了，报错只说
 * 「未跑出 OK」并甩一整段输出 —— 人得自己从几百行里翻出真名。
 * 把实际名单列出来，改名一眼可见。
 */
function gtestCaseNames(out) {
    const names = [];
    const re = /^\s*\[\s*(?:OK|PASSED)\s*\]\s+(\S+)/gm;
    let m;
    while ((m = re.exec(out)) !== null) names.push(m[1]);
    return names;
}

/** gtestOk 的失败消息：带上实际跑过的用例名。 */
function gtestMissing(out, gtestCase) {
    const actual = gtestCaseNames(out);
    const hint = actual.length
        ? `\n实际跑过的用例名：\n  ${actual.join('\n  ')}`
        : '\n（输出里没解析出任何 gtest 用例名 —— 二进制可能没跑起来）';
    return `用例 "${gtestCase}" 未跑出 OK${hint}`;
}

/** 该 ctest 正则当前 build 树里匹配到的测试数（用于跳过平台专属二进制）。 */
function countCtest(nameRe) {
    const r = spawnSync('ctest',
        ['--test-dir', BUILD, '-N', '-R', nameRe],
        { cwd: ROOT, encoding: 'utf8', timeout: 60000 });
    const m = /Total Tests:\s*(\d+)/.exec(`${r.stdout || ''}${r.stderr || ''}`);
    return m ? Number(m[1]) : 0;
}

// 判据 2-8 共用同一次 ctest 运行：engine_device_policy_tests 是整二进制运行，
// 输出里含全部 gtest 用例的 `[       OK ]`。node:test 默认顺序执行，惰性记忆安全。
const policyRun = { status: -1, out: '' };
function runPolicySuite() {
    if (policyRun.status === -1) {
        Object.assign(policyRun, runCtest('^engine_device_policy_tests$'));
    }
    return policyRun;
}

// ---------------------------------------------------------------------------
// 验收标准 1：默认设备策略搬进引擎核心，且不再住在示例里
// ---------------------------------------------------------------------------
test('判据 1：默认设备策略搬进引擎核心，且不再住在示例里；验证方式：静态检查', () => {
    const policyCpp = readText(path.join(ROOT,
        'src/engine/src/engine_device_policy.cpp'));
    const rdDevice  = readText(path.join(ROOT,
        'examples/remote-desktop/rd_device.cpp'));

    // 平台分支表存在于引擎核心的策略实现里（Windows / macOS / Linux + 兜底）。
    assert.match(policyCpp, /#if\s+defined\(_WIN32\)/,
        '策略实现缺 Windows 分支');
    assert.match(policyCpp, /"windows-wgc"/,
        '策略实现缺 Windows 屏幕后端');
    assert.match(policyCpp, /"camera-mediafoundation"/,
        '策略实现缺 Windows 摄像头后端');
    assert.match(policyCpp, /#elif\s+defined\(__APPLE__\)/,
        '策略实现缺 macOS 分支');
    assert.match(policyCpp, /"macos-screencapturekit"/,
        '策略实现缺 macOS 屏幕后端');
    assert.match(policyCpp, /"camera-avfoundation"/,
        '策略实现缺 macOS 摄像头后端');
    assert.match(policyCpp, /#elif\s+defined\(__linux__\)/,
        '策略实现缺 Linux 分支');
    assert.match(policyCpp, /"linux-x11"/,
        '策略实现缺 Linux 屏幕后端');
    assert.match(policyCpp, /"camera-v4l2"/,
        '策略实现缺 Linux 摄像头后端');
    assert.match(policyCpp, /"synthetic-camera"/,
        '策略实现缺兜底（synthetic）摄像头后端');

    // 示例里不再有 #ifdef 平台分支表。
    //
    // 规格原文：「这些 id 作为**参数值**出现不算违例，但**不得**有 `#ifdef`
    // 平台分支表」。所以平台 id 只在**注释里**（用法说明）出现是允许的 ——
    // 禁止的是它们出现在**代码**里（那意味着示例自己还在选平台后端）。
    assert.doesNotMatch(rdDevice,
        /#\s*(ifdef|ifndef|if|elif|else)\b/,
        'rd_device.cpp 不得再有预处理条件（平台分支表已搬走）');

    // 剥掉注释后再查平台 id：代码里出现 = 示例还在自己选后端 = 违例。
    const rdDeviceCode = stripComments(rdDevice);
    for (const id of ['windows-wgc', 'camera-mediafoundation',
                      'macos-screencapturekit', 'camera-avfoundation',
                      'linux-x11', 'camera-v4l2']) {
        assert.ok(!rdDeviceCode.includes(id),
            `rd_device.cpp 的代码里不得含平台后端 id "${id}"` +
            `（作为注释/用法示例里的参数值出现不算违例）`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 2：覆盖优先级成立
// ---------------------------------------------------------------------------
test('判据 2：覆盖优先级成立；验证方式：单元测试（显式 id 优先于策略）', () => {
    const res = runPolicySuite();
    assert.equal(res.status, 0,
        `engine_device_policy_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'DevicePolicy.ExplicitIdWinsOverPolicy'), gtestMissing(res.out, 'DevicePolicy.ExplicitIdWinsOverPolicy'));
    assert.ok(gtestOk(res.out, 'EngineDevicePolicy.ExplicitCaptureIdWinsOverPolicy'), gtestMissing(res.out, 'EngineDevicePolicy.ExplicitCaptureIdWinsOverPolicy'));
});

// ---------------------------------------------------------------------------
// 验收标准 3："auto" 走策略，且空值语义不变
// ---------------------------------------------------------------------------
test('判据 3："auto" 走策略，且空值语义不变；验证方式：单元测试 + 静态检查', () => {
    const res = runPolicySuite();
    assert.equal(res.status, 0,
        `engine_device_policy_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineDevicePolicy.EmptyCaptureNameInstallsNoDevice'), gtestMissing(res.out, 'EngineDevicePolicy.EmptyCaptureNameInstallsNoDevice'));
    assert.ok(gtestOk(res.out, 'DevicePolicy.AutoResolvesThroughPolicy'), gtestMissing(res.out, 'DevicePolicy.AutoResolvesThroughPolicy'));
    assert.ok(gtestOk(res.out, 'EngineDevicePolicy.AutoResolvesToThePolicyScreenBackend'), gtestMissing(res.out, 'EngineDevicePolicy.AutoResolvesToThePolicyScreenBackend'));
    assert.ok(gtestOk(res.out, 'EngineDevicePolicy.AutoContentHintRoutesCameraDirection'), gtestMissing(res.out, 'EngineDevicePolicy.AutoContentHintRoutesCameraDirection'));

    const media = readText(path.join(ROOT, 'src/engine/src/engine_media.cpp'));
    assert.match(media, /resolved\s*==\s*"auto"/,
        '引擎必须识别 video_capture_name == "auto"');
    assert.match(media, /resolve_auto_capture_backend\(/,
        '引擎必须经策略解析 "auto"');
});

// ---------------------------------------------------------------------------
// 验收标准 4：策略没兑现时不是错误
// ---------------------------------------------------------------------------
test('判据 4：策略没兑现时不是错误；验证方式：单元测试 + 静态检查（降级为无设备 + warn）', () => {
    const res = runPolicySuite();
    assert.equal(res.status, 0,
        `engine_device_policy_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'DevicePolicy.PolicyBackendNotRegisteredDegradesToNoDevice'), gtestMissing(res.out, 'DevicePolicy.PolicyBackendNotRegisteredDegradesToNoDevice'));
    assert.ok(gtestOk(res.out, 'DevicePolicy.EmptyPolicyDegradesToNoDevice'), gtestMissing(res.out, 'DevicePolicy.EmptyPolicyDegradesToNoDevice'));

    // 静态：auto 解析失败 → warn + 无设备；kVideoCapturePluginMissing 只属于
    // 显式未知 id 的路径，且出现在 auto 解析块之后 —— 用切片排除它。
    const media = readText(path.join(ROOT, 'src/engine/src/engine_media.cpp'));
    const start = media.indexOf('if (resolved == "auto")');
    const end = media.indexOf('if (!resolved.empty())', start);
    assert.ok(start >= 0 && end > start,
        'engine_media.cpp 缺 auto 解析块（判据 3/4 的切片锚点失效）');
    const autoBlock = media.slice(start, end);
    assert.match(autoBlock, /continuing without a capture device/,
        'auto 解析失败必须 warn（降级为无设备）');
    assert.ok(!autoBlock.includes('kVideoCapturePluginMissing'),
        'auto 解析失败不得报 kVideoCapturePluginMissing（与显式未知 id 不同）');
});

// ---------------------------------------------------------------------------
// 验收标准 5：设备变化被检测到并计数
// ---------------------------------------------------------------------------
test('判据 5：设备变化被检测到并计数；验证方式：单元测试（移除 0→1、插回 1→2）+ 静态检查', () => {
    const res = runPolicySuite();
    assert.equal(res.status, 0,
        `engine_device_policy_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineDevicePolicy.DeviceRemovalRecoveryAndReaddAreCounted'), gtestMissing(res.out, 'EngineDevicePolicy.DeviceRemovalRecoveryAndReaddAreCounted'));
    assert.ok(gtestOk(res.out, 'EngineDevicePolicy.DeviceEventSourceReportsChangesAndBackoff'), gtestMissing(res.out, 'EngineDevicePolicy.DeviceEventSourceReportsChangesAndBackoff'));

    const pump = readText(path.join(ROOT, 'src/engine/src/engine_video_pump.cpp'));
    assert.match(pump, /poll_capture_device_changes\(\);/,
        '轮询必须挂在 pump_video_capture() 链路上');
    assert.match(pump, /\+\+vstate\.video_capture_device_changes/,
        '轮询必须递增 changes 计数器');
});

// ---------------------------------------------------------------------------
// 验收标准 6：重连有退避上界，不会疯狂重试
// ---------------------------------------------------------------------------
test('判据 6：重连有退避上界，不会疯狂重试；验证方式：单元测试 + 静态检查', () => {
    const res = runPolicySuite();
    assert.equal(res.status, 0,
        `engine_device_policy_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'CaptureReconnect.BackoffIsBoundedAndStopsAtTwentyAttempts'), gtestMissing(res.out, 'CaptureReconnect.BackoffIsBoundedAndStopsAtTwentyAttempts'));
    assert.ok(gtestOk(res.out, 'CaptureReconnect.LostProbesImmediatelyThenBacksOff'), gtestMissing(res.out, 'CaptureReconnect.LostProbesImmediatelyThenBacksOff'));
    assert.ok(gtestOk(res.out, 'CaptureReconnect.SecondLossDoesNotResetTheTimer'), gtestMissing(res.out, 'CaptureReconnect.SecondLossDoesNotResetTheTimer'));

    const reconnect = readText(path.join(ROOT,
        'src/engine/src/engine_capture_reconnect.hpp'));
    assert.match(reconnect, /initial_backoff_us\s*=\s*100'000/,
        '初始退避必须是 100 ms');
    assert.match(reconnect, /max_backoff_us\s*=\s*5'000'000/,
        '退避上界必须是 5 s');
    assert.match(reconnect, /max_attempts\s*=\s*20/,
        '重试上限必须是 20');

    const pump = readText(path.join(ROOT, 'src/engine/src/engine_video_pump.cpp'));
    assert.match(pump, /kVideoCaptureOpenFailed/,
        '重试超限后必须报 kVideoCaptureOpenFailed 并停止');
});

// ---------------------------------------------------------------------------
// 验收标准 7：设备恢复后帧恢复、video_capture_restarts 递增
// ---------------------------------------------------------------------------
test('判据 7：设备恢复后帧恢复、video_capture_restarts 递增；验证方式：单元测试 + 静态检查', () => {
    const res = runPolicySuite();
    assert.equal(res.status, 0,
        `engine_device_policy_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineDevicePolicy.DeviceRemovalRecoveryAndReaddAreCounted'), gtestMissing(res.out, 'EngineDevicePolicy.DeviceRemovalRecoveryAndReaddAreCounted'));
    assert.ok(gtestOk(res.out, 'CaptureReconnect.SuccessResetsToIdle'), gtestMissing(res.out, 'CaptureReconnect.SuccessResetsToIdle'));

    const pump = readText(path.join(ROOT, 'src/engine/src/engine_video_pump.cpp'));
    assert.match(pump, /\+\+vstate\.video_capture_restarts/,
        '重开成功必须递增 restarts 计数器');
});

// ---------------------------------------------------------------------------
// 验收标准 8：两个计数器相互独立
// ---------------------------------------------------------------------------
test('判据 8：两个计数器相互独立；验证方式：单元测试 + 静态检查（计数落点在 4 处）', () => {
    const res = runPolicySuite();
    assert.equal(res.status, 0,
        `engine_device_policy_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineDevicePolicy.DeviceRemovalRecoveryAndReaddAreCounted'), gtestMissing(res.out, 'EngineDevicePolicy.DeviceRemovalRecoveryAndReaddAreCounted'));

    // 计数器的 4 个落点：引擎内部状态 / 公开 Stats / X-macro 表 / stats() 汇总。
    const pump = readText(path.join(ROOT, 'src/engine/src/engine_video_pump.cpp'));
    assert.match(pump, /\+\+vstate\.video_capture_device_changes/,
        'video_state 递增落点缺失');
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.match(engineHpp,
        /std::uint64_t\s+video_capture_device_changes\s*=\s*0;/,
        '公开 Stats 缺该计数');
    const statsHpp = readText(path.join(ROOT,
        'src/engine/src/engine_stats_state.hpp'));
    assert.match(statsHpp, /video_capture_device_changes/,
        'X-macro 计数器表缺该计数');
    const media = readText(path.join(ROOT, 'src/engine/src/engine_media.cpp'));
    assert.match(media, /out\.video_capture_device_changes/,
        'stats() 缺该计数的汇出');
});

// ---------------------------------------------------------------------------
// 验收标准 9：摄像头后端能上报「设备没了」
// ---------------------------------------------------------------------------
test('判据 9：摄像头后端能上报「设备没了」；验证方式：静态检查 + 合成路径单测', () => {
    // MF / AVF / V4L2 三个后端共用 CameraCapture::capture_loop：next_frame()
    // 返回非 kOk 非 kErrNotReady → ++device_errors_ → 停止。引擎据此触发退避。
    const camera = readText(path.join(ROOT,
        'src/modules/video_capture/src/camera_capture.cpp'));
    assert.match(camera,
        /if\s*\(\s*status\s*!=\s*plugins::kOk\s*\)[\s\S]*?\+\+device_errors_/,
        '共享 capture_loop 失败路径必须递增 device_errors_');

    // 平台后端也有各自的上报点（Unix 核心 / Windows 后端）。
    const unixCore = readText(path.join(ROOT,
        'src/modules/video_capture/src/unix_capture_core.cpp'));
    assert.match(unixCore, /\+\+stats_\.device_errors/,
        'Unix 采集核心必须递增 device_errors');
    const winBackend = readText(path.join(ROOT,
        'src/modules/video_capture/src/platform/windows/windows_capture.cpp'));
    assert.match(winBackend, /\+\+stats_\.device_errors/,
        'Windows 采集后端必须递增 device_errors');

    // 跨平台可跑的合成路径单测全绿。
    for (const re of ['^camera_capture_tests$',
                      '^unix_capture_core_tests$',
                      '^video_capture_tests$']) {
        const r = runCtest(re);
        assert.equal(r.status, 0,
            `合成路径单测（${re}）未通过（exit=${r.status}）：\n${r.out}`);
        assert.ok(r.out.includes('[==========]'),
            `${re} 没有产出 gtest 运行输出：\n${r.out}`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 10：接缝留了变更通知的口子
// ---------------------------------------------------------------------------
test('判据 10：接缝留了变更通知的口子；验证方式：静态检查', () => {
    const events = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/capture_device_events.hpp'));
    assert.match(events,
        /class\s+NIMRTC_API_STABLE\s+ICaptureDeviceEventSource/,
        '必须新增独立的 ICaptureDeviceEventSource 接口');
    assert.match(events, /take_device_changes\(\)\s*noexcept\s*=\s*0/,
        '接口必须提供 take_device_changes 纯虚函数');
    assert.match(events, /kRemoved/,
        '事件枚举必须含「移除」');

    // IVideoCapture 没有被加纯虚函数：video_capture.hpp 不含事件接口的任何字面量
    //（该文件本片未改动，git status 快照里也没有它）。
    const vc = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/video_capture.hpp'));
    assert.ok(!vc.includes('take_device_changes'),
        'IVideoCapture 不得新增纯虚函数（外部实现者不受破坏）');
    assert.ok(!vc.includes('ICaptureDeviceEventSource'),
        'IVideoCapture 不得被并入事件接口');

    // 引擎用 dynamic_cast 探测（与 ice_transport.hpp 相同的接口扩展模式）。
    const pump = readText(path.join(ROOT, 'src/engine/src/engine_video_pump.cpp'));
    assert.match(pump, /dynamic_cast<plugins::ICaptureDeviceEventSource\*>/,
        '引擎必须经 dynamic_cast 探测事件源');
});

// ---------------------------------------------------------------------------
// 验收标准 11：不新增引擎线程
// ---------------------------------------------------------------------------
test('判据 11：不新增引擎线程；验证方式：静态检查（轮询挂在已有 tick/pump 链路上）', () => {
    const pump = readText(path.join(ROOT, 'src/engine/src/engine_video_pump.cpp'));
    // 轮询调用点紧跟 recover_capture_device()，是 pump_video_capture() 链路上的
    // 一环（pump_video_capture() 由 tick() 驱动，见 engine_transport.cpp）。
    assert.match(pump,
        /recover_capture_device\(\);\s*[\s\S]*?poll_capture_device_changes\(\);/,
        '轮询必须挂在 pump 链路（与重连同链）');
    assert.ok(!pump.includes('std::thread('),
        'engine_video_pump.cpp 不得新增 std::thread');
    assert.ok(!pump.includes('ThreadPool'),
        'engine_video_pump.cpp 不得新增线程池');

    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    assert.match(transport, /pump_video_capture\(\);/,
        'tick 路径必须驱动 pump_video_capture()');
});

// ---------------------------------------------------------------------------
// 验收标准 12：示例反映设备状态变化
// ---------------------------------------------------------------------------
test('判据 12：示例反映设备状态变化；验证方式：静态检查', () => {
    const rdDevice = readText(path.join(ROOT,
        'examples/remote-desktop/rd_device.cpp'));
    assert.match(rdDevice, /effective_backend/,
        '示例必须打印实际生效的采集后端 id');
    assert.match(rdDevice, /NimRTCEngine::default_device_policy\(\)/,
        '示例必须经引擎公开 API 查询默认策略');
    assert.match(rdDevice, /"auto"/,
        '示例默认 --capture 值必须为 "auto"');
});

// ---------------------------------------------------------------------------
// 验收标准 13：验收测试文件是真实断言
// ---------------------------------------------------------------------------
test('判据 13：验收测试文件是真实断言；验证方式：node --test 全部通过 + 无 todo 用例', () => {
    const own = readText(__filename);
    // 骨架生成的待办用例是 test 的待办重载。用拼接构造 needle，
    // 避免把待办标记本身写进本文件 —— 否则自检会误伤自己。
    const todoNeedle = ['test', 'todo('].join('.');
    assert.ok(!own.includes(todoNeedle),
        '验收文件不得残留 todo 用例（门禁②要求每条用例都是可执行断言）');
    // 本文件每条用例都是 test(...) 真断言；node --test 能跑到这里，即证明
    // 前面所有用例都已通过。
});

// ---------------------------------------------------------------------------
// 验收标准 14：不破坏既有行为
// ---------------------------------------------------------------------------
test('判据 14：不破坏既有行为；验证方式：ctest 无回归（既有引擎视频测试与视频采集模块测试）', () => {
    const regression = [
        { re: '^test_engine_video_capture$', note: '既有引擎视频采集路径' },
        { re: '^test_engine_capture_pump$',  note: '既有引擎采集 pump' },
        { re: '^cursor_compositor_tests$',   note: '光标合成' },
        { re: '^windows_capture_tests$',     note: 'Windows 采集后端（平台专属）' },
    ];
    for (const r of regression) {
        if (countCtest(r.re) === 0) continue; // 平台专属二进制未构建时跳过
        const res = runCtest(r.re);
        assert.equal(res.status, 0,
            `${r.note}（${r.re}）未通过（exit=${res.status}）：\n${res.out}`);
        assert.ok(res.out.includes('[==========]'),
            `${r.note}（${r.re}）没有产出 gtest 运行输出：\n${res.out}`);
    }
    // 视频采集模块测试（camera_capture / unix_capture_core / video_capture）
    // 已在判据 9 整跑，此处不重复。
});

// ---------------------------------------------------------------------------
// 写集边界断言（ADR-075）
// ---------------------------------------------------------------------------
test('写集边界：本片改动仅限写集范围；验证方式：改动清单不越界 + 产物在树上', () => {
    // 写集 = 规格「范围」；外加会话开始前已存在的 11 个子模块脏状态
    // （git status 快照，它们不是本片改出来的）。
    //
    // 注意：src/modules/audio_device 虽然在规格写集里，但规格明确「DO NOT
    //  modify」—— 它一旦被本片改动就应被门禁抓住，所以**故意不放**进 ALLOW。
    const ALLOW = [
        'src/engine',                                // 引擎编排（策略实现 + 重连状态机 + 轮询 + 测试）
        'src/plugins/include/nimrtc/plugins/capture_device_events.hpp',
        'src/plugins/include/nimrtc/plugins/video_capture.hpp',
        'src/modules/video_capture',                 // 摄像头后端错误上报（判据 9 范围内）
        'examples/remote-desktop',                   // rd_device.cpp 示例
        'docs/api/engine_api.md',
        'docs/api/public_headers.manifest',
        'tests/acceptance/v1-device-policy',
        'specs/slices/v1-device-policy.md',
        // —— 会话前已存在的子模块脏状态 ——
        'src/third_party/googletest',
        'src/third_party/libopus/src',
        'src/third_party/libsamplerate/src',
        'src/third_party/libsrtp/src',
        'src/third_party/libyuv/src',
        'src/third_party/msquic',
        'src/third_party/nlohmann_json',
        'src/third_party/openh264/src',
        'src/third_party/portaudio/src',
        'src/third_party/svt-av1/src',
        'src/third_party/wolfssl/src',
    ];

    const REQUIRED = [
        // D3：策略表（引擎私有实现）
        'src/engine/src/engine_device_policy.hpp',
        'src/engine/src/engine_device_policy.cpp',
        // D4：退避重连状态机 + 轮询
        'src/engine/src/engine_capture_reconnect.hpp',
        'src/engine/src/engine_video_pump.cpp',
        // D3/D4 的引擎接线与统计
        'src/engine/src/engine_media.cpp',
        'src/engine/src/engine_stats_state.hpp',
        'src/engine/src/engine_video_state.hpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        // 确定性单元测试（引擎私有头也被测到）
        'src/engine/tests/test_engine_device_policy.cpp',
        'src/engine/tests/CMakeLists.txt',
        // D4 接缝
        'src/plugins/include/nimrtc/plugins/capture_device_events.hpp',
        // 示例 + 文档
        'examples/remote-desktop/rd_device.cpp',
        'docs/api/engine_api.md',
        'docs/api/public_headers.manifest',
        // 本片验收 + 规格
        'tests/acceptance/v1-device-policy/acceptance.test.cjs',
        'specs/slices/v1-device-policy.md',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-device-policy' });
    // ② 产物检查（状态）：产物必须在树上（git ls-files），与合并策略无关。
    const onTree = ws.trackedOnTree(ROOT, REQUIRED);
    const { violations, missing } = ws.evaluateChanged({
        changed,
        allow: ALLOW,
        required: REQUIRED,
        present: onTree,
    });

    assert.deepEqual(violations, [],
        `改动超出本片写集范围：${violations.join(', ')}`);
    assert.deepEqual(missing, [],
        `本片产物缺失（产物必须在树上）：${missing.join(', ')}`);
    assert.ok(onTree.length >= 1,
        `本片产物必须在树上（已追踪文件）：${REQUIRED.join(', ')}`);
});

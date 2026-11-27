// 验收测试 —— 切片 v1-device-policy（v1.4 D3 默认设备策略 + D4 设备热插拔）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把规格的 14 条验收标准（specs/slices/v1-device-policy.md §验收标准）
// 逐条做成真实断言：
//
//   D3（默认设备策略）
//    1. 默认设备策略搬进引擎核心，示例里不再有平台分支表
//    2. 覆盖优先级成立：宿主给的 id 赢过策略
//    3. "auto" 走策略；空值语义不变
//    4. 策略没兑现时不是错误
//    5. 公开入口 default_device_policy() 与私有表一致
//
//   D4（设备变更检测与重连）
//    6. 设备变化被检测到并计数
//    7. 重连有退避上界，不会疯狂重试
//    8. 设备恢复后帧恢复、video_capture_restarts 递增
//    9. 两个计数器互相独立
//   10. 接缝留了变更通知的口子（IVideoCapture 未被加纯虚函数）
//   11. 不新增引擎线程
//   12. 示例反映设备状态变化
//   13. 验收测试文件是真实断言（无 test.todo）
//   14. 不破坏既有行为（既有引擎视频测试仍通过）
//
// 验证手段：读源码做静态断言 + 直接跑门禁同一棵 build 树里的 gtest 二进制。
// 不用 ctest -R 锚用例名，而用 --gtest_filter 直接跑二进制 —— 本片的套件
// 挂在 tests 子树下，直接跑二进制绕开 CTestTestfile 加载，判定只依赖
// 「二进制跑起来 + 用例 OK」。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');

// 写集边界判据的共享实现（v5-writeset-acceptance/）
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, 'utf8'); }

/** 定位 build 树里的 gtest 可执行文件（多配置 / 单配置都可） */
function resolveExe(sub, name) {
    const base = path.join(ROOT, 'build', sub);
    const candidates = [
        path.join(base, 'Debug', name + '.exe'),   // MSVC 多配置
        path.join(base, 'Release', name + '.exe'), // MSVC Release
        path.join(base, name + '.exe'),            // 单配置 Windows
        path.join(base, name),                     // 单配置 POSIX
    ];
    for (const c of candidates) {
        if (fs.existsSync(c)) return c;
    }
    return candidates[0]; // spawnSync 用明确的 ENOENT 报错
}

/** 跑一个 gtest 二进制，用 --gtest_filter 限定用例，返回退出码 + 输出 */
function runGtest(sub, exe, filter) {
    const exePath = resolveExe(sub, exe);
    const r = spawnSync(exePath, ['--gtest_filter=' + filter],
        { cwd: ROOT, encoding: 'utf8', timeout: 600000 });
    return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/** gtest 单条用例是否通过（输出里出现 `[       OK ] <case>`） */
function gtestOk(out, gtestCase) {
    return out.includes(`[       OK ] ${gtestCase}`);
}

/** 本片的套件：引擎侧的设备策略与热插拔 */
const SUITE = 'EngineDevicePolicy.*:EngineDeviceHotplug.*';

// ---------------------------------------------------------------------------
// 验收标准 1：默认设备策略搬进引擎核心，且不再住在示例里
// ---------------------------------------------------------------------------
test('D3 criterion 1: the default table lives in the engine, not in the example',
    () => {
        // 1a. 引擎侧有一份策略实现（私有协作者 + 公开入口）。
        const policyHpp = path.join(ROOT, 'src/engine/src/engine_device_policy.hpp');
        const policyCpp = path.join(ROOT, 'src/engine/src/engine_device_policy.cpp');
        assert.ok(fs.existsSync(policyHpp), 'the engine must have a policy header');
        assert.ok(fs.existsSync(policyCpp), 'the engine must have a policy source');

        const cpp = readText(policyCpp);
        // 平台表在引擎侧。
        assert.ok(cpp.includes('windows-wgc'),
            'the engine policy must name the Windows screen backend');
        assert.ok(cpp.includes('macos-screencapturekit'),
            'the engine policy must name the macOS screen backend');
        assert.ok(cpp.includes('linux-x11'),
            'the engine policy must name the Linux screen backend');

        // 1b. 示例里不再有平台分支表。
        const example = readText(
            path.join(ROOT, 'examples/remote-desktop/rd_device.cpp'));
        assert.ok(!/#if(def)?\s+_WIN32[\s\S]{0,400}windows-wgc/.test(example),
            'the example must not carry a Windows platform branch for the default backend');
        assert.ok(!example.includes('macos-screencapturekit'),
            'the example must not name the macOS default backend itself');
        assert.ok(!example.includes('linux-x11'),
            'the example must not name the Linux default backend itself');

        // 1c. 示例改为读引擎的公开入口。
        assert.ok(example.includes('default_device_policy'),
            'the example must ask the engine for the default policy');
    });

// ---------------------------------------------------------------------------
// 验收标准 2：覆盖优先级成立（宿主显式 id 赢过策略）
// ---------------------------------------------------------------------------
test('D3 criterion 2: the host\'s explicit id wins over the policy', () => {
    const r = runGtest('tests', 'test_engine_device_policy',
        'EngineDevicePolicy.TheResolutionRule:EngineDevicePolicy.TheHostsExplicitIdWinsOverThePolicy');
    assert.equal(r.status, 0, `the gtest binary failed:\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineDevicePolicy.TheResolutionRule'),
        `TheResolutionRule did not pass:\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineDevicePolicy.TheHostsExplicitIdWinsOverThePolicy'),
        `TheHostsExplicitIdWinsOverThePolicy did not pass:\n${r.out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 3："auto" 走策略，且空值语义不变
// ---------------------------------------------------------------------------
test('D3 criterion 3: "auto" resolves through the policy; empty keeps its meaning',
    () => {
        const r = runGtest('tests', 'test_engine_device_policy',
            'EngineDevicePolicy.AutoResolvesThroughThePolicy:EngineDevicePolicy.TheEmptyNameStillMeansTheHostFeedsFrames');
        assert.equal(r.status, 0, `the gtest binary failed:\n${r.out}`);
        assert.ok(gtestOk(r.out, 'EngineDevicePolicy.AutoResolvesThroughThePolicy'),
            `AutoResolvesThroughThePolicy did not pass:\n${r.out}`);
        assert.ok(gtestOk(r.out, 'EngineDevicePolicy.TheEmptyNameStillMeansTheHostFeedsFrames'),
            `TheEmptyNameStillMeansTheHostFeedsFrames did not pass:\n${r.out}`);
    });

// ---------------------------------------------------------------------------
// 验收标准 4：策略没兑现时不是错误
// ---------------------------------------------------------------------------
test('D3 criterion 4: an unhonourable policy is not an error', () => {
    const r = runGtest('tests', 'test_engine_device_policy',
        'EngineDevicePolicy.AutoResolvesThroughThePolicy:EngineDevicePolicy.AnUnknownExplicitIdIsStillAnError');
    assert.equal(r.status, 0, `the gtest binary failed:\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineDevicePolicy.AutoResolvesThroughThePolicy'),
        `AutoResolvesThroughThePolicy did not pass:\n${r.out}`);
    // 对照：显式的未知 id 仍然是错误（不能被"策略降级"顺手吞掉）。
    assert.ok(gtestOk(r.out, 'EngineDevicePolicy.AnUnknownExplicitIdIsStillAnError'),
        `AnUnknownExplicitIdIsStillAnError did not pass:\n${r.out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 5：公开入口与私有表一致
// ---------------------------------------------------------------------------
test('D3 criterion 5: the public entry point matches the private table', () => {
    const r = runGtest('tests', 'test_engine_device_policy',
        'EngineDevicePolicy.ThePublicTableMatchesThePrivateOne:EngineDevicePolicy.TheTableIsInTheEngineAndNamesRealBackends');
    assert.equal(r.status, 0, `the gtest binary failed:\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineDevicePolicy.ThePublicTableMatchesThePrivateOne'),
        `ThePublicTableMatchesThePrivateOne did not pass:\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineDevicePolicy.TheTableIsInTheEngineAndNamesRealBackends'),
        `TheTableIsInTheEngineAndNamesRealBackends did not pass:\n${r.out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 6：设备变化被检测到并计数
// ---------------------------------------------------------------------------
test('D4 criterion 6: a device change is detected and counted', () => {
    const r = runGtest('tests', 'test_engine_device_policy',
        'EngineDeviceHotplug.ADeviceChangeIsCountedAndTheDeviceReopened');
    assert.equal(r.status, 0, `the gtest binary failed:\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineDeviceHotplug.ADeviceChangeIsCountedAndTheDeviceReopened'),
        `ADeviceChangeIsCountedAndTheDeviceReopened did not pass:\n${r.out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 7：重连有退避上界，不会疯狂重试
// ---------------------------------------------------------------------------
test('D4 criterion 7: the reopen is backoff-bounded', () => {
    const r = runGtest('tests', 'test_engine_device_policy',
        'EngineDeviceHotplug.TheReopenIsBackoffBounded');
    assert.equal(r.status, 0, `the gtest binary failed:\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineDeviceHotplug.TheReopenIsBackoffBounded'),
        `TheReopenIsBackoffBounded did not pass:\n${r.out}`);

    // 退避的三个数字在源码里，且有上界。
    const pump = readText(path.join(ROOT, 'src/engine/src/engine_video_pump.cpp'));
    assert.ok(pump.includes('kCaptureReopenBackoffBaseMs'),
        'the reopen backoff must have a base interval');
    assert.ok(pump.includes('kCaptureReopenBackoffCapMs'),
        'the reopen backoff must have a cap');
    assert.ok(pump.includes('kCaptureReopenMaxAttempts'),
        'the reopen must have an attempt cap');
});

// ---------------------------------------------------------------------------
// 验收标准 8：设备恢复后帧恢复、video_capture_restarts 递增
// ---------------------------------------------------------------------------
test('D4 criterion 8: the device comes back and the restart counter moves', () => {
    const r = runGtest('tests', 'test_engine_device_policy',
        'EngineDeviceHotplug.ADeviceChangeIsCountedAndTheDeviceReopened');
    assert.equal(r.status, 0, `the gtest binary failed:\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineDeviceHotplug.ADeviceChangeIsCountedAndTheDeviceReopened'),
        `ADeviceChangeIsCountedAndTheDeviceReopened did not pass:\n${r.out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 9：两个计数器互相独立
// ---------------------------------------------------------------------------
test('D4 criterion 9: the change and restart counters are independent', () => {
    const r = runGtest('tests', 'test_engine_device_policy',
        'EngineDeviceHotplug.TheChangeAndRestartCountersAreIndependent');
    assert.equal(r.status, 0, `the gtest binary failed:\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineDeviceHotplug.TheChangeAndRestartCountersAreIndependent'),
        `TheChangeAndRestartCountersAreIndependent did not pass:\n${r.out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 10：接缝留了变更通知的口子
// ---------------------------------------------------------------------------
test('D4 criterion 10: the seam has an optional change-notification interface', () => {
    // 10a. 新接口是**独立**的头，且 IVideoCapture 没有被加纯虚函数。
    const seamPath = path.join(
        ROOT, 'src/plugins/include/nimrtc/plugins/capture_device_events.hpp');
    assert.ok(fs.existsSync(seamPath), 'the optional seam header must exist');

    const seam = readText(seamPath);
    assert.ok(seam.includes('ICaptureDeviceEventSource'),
        'the seam must define ICaptureDeviceEventSource');
    assert.ok(seam.includes('take_device_changes'),
        'the seam must expose take_device_changes()');
    assert.ok(seam.includes('CaptureDeviceChange'),
        'the seam must define the change vocabulary');

    // IVideoCapture 的声明没有被改动：它必须**不**含新的纯虚函数。
    const captureHpp = readText(
        path.join(ROOT, 'src/plugins/include/nimrtc/plugins/video_capture.hpp'));
    const iface = captureHpp.slice(
        captureHpp.indexOf('class NIMRTC_API_STABLE IVideoCapture'));
    assert.ok(!iface.includes('take_device_changes'),
        'IVideoCapture must not gain the change-notification method');
    assert.ok(!iface.includes('ICaptureDeviceEventSource'),
        'IVideoCapture must not gain a member of the optional interface');

    // 10b. 引擎用 dynamic_cast 探测。
    const pump = readText(path.join(ROOT, 'src/engine/src/engine_video_pump.cpp'));
    assert.ok(pump.includes('dynamic_cast<plugins::ICaptureDeviceEventSource*>'),
        'the engine must probe the optional interface with a dynamic_cast');

    // 10c. 新公开头进了清单。
    const manifest = readText(path.join(ROOT, 'docs/api/public_headers.manifest'));
    assert.ok(manifest.includes('src/plugins/include/nimrtc/plugins/capture_device_events.hpp'),
        'the new public header must be in the manifest');
});

// ---------------------------------------------------------------------------
// 验收标准 11：不新增引擎线程
// ---------------------------------------------------------------------------
test('D4 criterion 11: the device watch adds no engine thread', () => {
    const pump = readText(path.join(ROOT, 'src/engine/src/engine_video_pump.cpp'));
    const transport = readText(path.join(ROOT, 'src/engine/src/engine_transport.cpp'));

    // 轮询挂在已有 tick 路径上。
    assert.ok(transport.includes('poll_capture_device_changes'),
        'the device watch must be called from the tick path');

    // 没有新增 std::thread / ThreadPool / 定时器线程。
    for (const [name, text] of [['engine_video_pump.cpp', pump],
                                ['engine_transport.cpp', transport]]) {
        assert.ok(!text.includes('std::thread'),
            `${name} must not start a thread`);
        assert.ok(!text.includes('ThreadPool'),
            `${name} must not start a thread pool`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 12：示例反映设备状态变化
// ---------------------------------------------------------------------------
test('D4 criterion 12: the example reports the backend in effect', () => {
    const example = readText(
        path.join(ROOT, 'examples/remote-desktop/rd_device.cpp'));
    assert.ok(example.includes('capture backend'),
        'the example must print the capture backend in effect');
});

// ---------------------------------------------------------------------------
// 验收标准 13：验收测试文件是真实断言（无 test.todo）
// ---------------------------------------------------------------------------
test('criterion 13: the acceptance file has no unfinished case', () => {
    const self = readText(__filename);
    // The pattern is spelled in two pieces so that this check does not
    // match *itself* -- a check that always finds its own needle is a
    // check that always fails.  Only a real **call** counts: a mention
    // in a comment or in this check's own title is not an unfinished
    // case.
    const needle = 'test' + '\\s*\\.\\s*todo\\s*\\(';
    const unfinished = (self.match(new RegExp(needle, 'g')) || []).length;
    assert.equal(unfinished, 0,
        `the acceptance file has ${unfinished} unfinished case(s)`);
});

// ---------------------------------------------------------------------------
// 验收标准 14：不破坏既有行为
// ---------------------------------------------------------------------------
test('criterion 14: the existing engine video tests still pass', () => {
    const r = runGtest('tests', 'test_engine_video_capture',
        'EngineVideoCapture.*');
    assert.equal(r.status, 0, `the existing video-capture suite failed:\n${r.out}`);
    assert.ok(r.out.includes('[  PASSED  ]'),
        `the existing video-capture suite did not pass:\n${r.out}`);
});

// ---------------------------------------------------------------------------
// 写集边界（ADR-075）
// ---------------------------------------------------------------------------
test('write-set boundary: the slice stays inside its declared scope', () => {
    const ALLOW = [
        // D3：引擎侧策略 + 公开入口
        'src/engine/src/engine_device_policy.hpp',
        'src/engine/src/engine_device_policy.cpp',
        'src/engine/src/engine_media.cpp',
        'src/engine/src/engine.cpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/CMakeLists.txt',
        // D3：示例
        'examples/remote-desktop/rd_device.cpp',
        // D4：接缝
        'src/plugins/include/nimrtc/plugins/capture_device_events.hpp',
        // D4：引擎侧热插拔
        'src/engine/src/engine_video_pump.cpp',
        'src/engine/src/engine_video_state.hpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/src/engine_stats_state.hpp',
        // 文档
        'docs/api/engine_api.md',
        'docs/api/public_headers.manifest',
        // —— v1.4 计划与缺口清单的落地记录（条目落地时必须同步）——
        'docs/plan/v1.4-plan.md',
        'docs/plan/post-v1.4-gap-inventory.md',
        // —— 漂移探测器：条目的行随条目翻转，必须同步 ——
        'tools',
        // —— ADR-015 公开头基线 ——
        'tests/test_api_classification.cpp',
        'CHANGELOG.md',
        // 单测 + 注册
        'tests/test_engine_device_policy.cpp',
        'tests/CMakeLists.txt',
        // 本片产物
        'tests/acceptance/v1-device-policy/acceptance.test.cjs',
    ];

    const REQUIRED = [
        // D3：策略实现 + 公开入口
        'src/engine/src/engine_device_policy.hpp',
        'src/engine/src/engine_device_policy.cpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        // D4：接缝 + 引擎侧
        'src/plugins/include/nimrtc/plugins/capture_device_events.hpp',
        'src/engine/src/engine_video_pump.cpp',
        // 单测 + 注册
        'tests/test_engine_device_policy.cpp',
        'tests/CMakeLists.txt',
        // 本片产物
        'tests/acceptance/v1-device-policy/acceptance.test.cjs',
    ];

    // —— 越界检查：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件 ——
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-device-policy' });
    // —— 产物检查：产物必须在树上（git ls-files），与合并策略无关 ——
    const onTree = ws.trackedOnTree(ROOT, REQUIRED);
    const changedSet = new Set(changed);
    const present = REQUIRED.filter(p => changedSet.has(p) || onTree.includes(p));
    const { violations, missing } = ws.evaluateChanged({
        changed,
        allow: ALLOW,
        required: REQUIRED,
        present,
    });

    assert.deepEqual(violations, [],
        `改动超出本片写集范围：${violations.join(', ')}`);
    assert.deepEqual(missing, [],
        `本片产物缺失（产物必须在树上）：${missing.join(', ')}`);
    assert.ok(onTree.length >= 1,
        `本片产物必须在树上（已追踪文件）：${REQUIRED.join(', ')}`);
});

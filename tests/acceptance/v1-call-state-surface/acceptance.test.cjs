// 验收测试 —— 切片 v1-call-state-surface（D7：一通话的生命周期汇合面，ADR-035）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 32 条验收标准（specs/slices/v1-call-state-surface.md §验收标准）逐组
// 落成真实断言，验证手段与 v1-ice-recovery 同族：
//
//   ① 读源码做静态断言（公开符号、NIMRTC_API_STABLE、接线点、关闭顺序、文档）；
//   ② 直接跑门禁同款 build 树里的 gtest 二进制（test_engine_call_state）
//      —— 用 --gtest_filter 限定用例，判定只依赖「二进制跑起来 + 用例 OK」；
//   ③ 跑只读脚本（api_freeze_scan.js / check_api_manifest.py）。
//
// 本片测试二进制分两个套件：
//   - EngineCallState        机器级（纯状态，不 sleep）：翻译 + 不重复判据 +
//                             failure_from_reason 六个 N4 词汇 + fail/close/reset
//   - EngineCallStateLive    活引擎（有状态假 ICE / 假 DTLS）：open → kConnecting、
//                             ICE 连上 → kConnected、N3 重连 → kReconnecting、
//                             N3 终态 / DTLS 失败 → kFailed、stop/hangup → kClosed
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');

// 写集边界判据的共享实现（v5-writeset-acceptance/）。
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, 'utf8'); }

/** 定位 build 树里的 gtest 可执行文件（多配置 / 单配置都可）。 */
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
    return candidates[0]; // 让 spawnSync 用明确的 ENOENT 报错
}

/** 跑一个 gtest 二进制，按 --gtest_filter 限定用例，返回退出码 + 输出。 */
function runGtest(sub, exe, filter) {
    const exePath = resolveExe(sub, exe);
    const r = spawnSync(exePath, ['--gtest_filter=' + filter],
        { cwd: ROOT, encoding: 'utf8', timeout: 600000 });
    return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/** gtest 单条用例是否通过（输出里出现 `[       OK ] <case>`）。 */
function gtestOk(out, gtestCase) {
    return out.includes(`[       OK ] ${gtestCase}`);
}

/** 跑一组 gtest 用例并逐条断言 OK。 */
function runGtestCases(exe, cases, note) {
    const filter = cases.join(':');
    const r = runGtest('src/engine/tests', exe, filter);
    assert.equal(r.status, 0,
        `${note}（exit=${r.status}）：\n${r.out}`);
    for (const gcase of cases) {
        assert.ok(gtestOk(r.out, gcase), `${note} 用例未跑出 OK：${gcase}\n${r.out}`);
    }
}

/** 断言 haystack 里 a 在 b 之前出现（关闭顺序等相对次序判据）。 */
function before(haystack, a, b, note) {
    const ia = haystack.indexOf(a);
    const ib = haystack.indexOf(b);
    assert.ok(ia >= 0, `${note}：找不到 ${a}`);
    assert.ok(ib >= 0, `${note}：找不到 ${b}`);
    assert.ok(ia < ib, `${note}：${a} 必须在 ${b} 之前（${ia} vs ${ib}）`);
}

// ---------------------------------------------------------------------------
// 判据 1-6：公开面
// ---------------------------------------------------------------------------
test('判据 1-6：公开面；验证方式：engine.hpp 静态 + api_freeze_scan.js 跑绿', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));

    // (a) 判据 1：CallState 枚举带五个值。
    assert.match(engineHpp, /enum class NIMRTC_API_STABLE CallState : std::uint8_t/,
        'CallState 必须是带 NIMRTC_API_STABLE 的枚举');
    for (const value of ['kConnecting = 0', 'kConnected = 1', 'kReconnecting = 2',
                         'kFailed = 3', 'kClosed = 4']) {
        assert.ok(engineHpp.includes(value), `CallState 缺 ${value}`);
    }

    // (b) 判据 2：CallFailure 枚举带六个值。
    assert.match(engineHpp, /enum class NIMRTC_API_STABLE CallFailure : std::uint8_t/,
        'CallFailure 必须是带 NIMRTC_API_STABLE 的枚举');
    for (const value of ['kNone = 0', 'kIceFailed = 1', 'kDtlsFailed = 2',
                         'kTurnRefused = 3', 'kCredentialsRejected = 4',
                         'kMediaFailed = 5']) {
        assert.ok(engineHpp.includes(value), `CallFailure 缺 ${value}`);
    }

    // (c) 判据 3：CallStateReport 带九个字段。
    assert.match(engineHpp, /struct NIMRTC_API_STABLE CallStateReport/,
        'CallStateReport 必须是带 NIMRTC_API_STABLE 的结构体');
    for (const field of ['CallState state = ', 'CallFailure failure = ',
                         'std::string failure_reason', 'old_pair', 'new_pair',
                         'std::string path_change_reason',
                         'std::uint32_t restart_count = 0',
                         'std::uint32_t remaining_budget = 0', 'bool terminal = false']) {
        assert.ok(engineHpp.includes(field), `CallStateReport 缺字段 ${field}`);
    }

    // (d) 判据 4：set_on_call_state() 存在且类型是 EngineCallStateCallback。
    assert.match(engineHpp,
        /NIMRTC_API_STABLE void set_on_call_state\s*\(\s*EngineCallStateCallback\s+cb\s*\)\s*noexcept/,
        'set_on_call_state 必须是 NIMRTC_API_STABLE 且参数为 EngineCallStateCallback');
    assert.match(engineHpp,
        /using NIMRTC_API_STABLE EngineCallStateCallback\s*=\s*std::function<void\(const CallStateReport&\)>/,
        'EngineCallStateCallback 类型别名必须存在且带 NIMRTC_API_STABLE');

    // (e) 判据 6：set_on_control_message 签名不变（std::span<uint8_t> 回调）。
    assert.match(engineHpp,
        /set_on_control_message\(\s*std::function<void\(std::span<const std::uint8_t>\)>\s*cb\s*\)\s*noexcept/,
        'set_on_control_message 签名必须保持原样（span<uint8_t> 回调，无新参数）');
    assert.match(engineHpp, /signature and\s*semantics are unchanged/,
        'set_on_control_message 文档必须写明签名与语义不变');

    // (f) 判据 5：新公开符号带 NIMRTC_API_STABLE（只读扫描脚本）+ 静态复核。
    const f = spawnSync(process.execPath,
        ['scripts/api_freeze_scan.js', '--no-md'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(f.status, 0,
        `api_freeze_scan.js 未通过（exit=${f.status}）：\n` +
        `${f.stdout || ''}${f.stderr || ''}`);
    for (const sym of [
        'NIMRTC_API_STABLE int start(',
        'NIMRTC_API_STABLE void     stop()',
        'NIMRTC_API_STABLE bool     hangup(',
        'NIMRTC_API_STABLE void set_on_call_state(',
    ]) {
        assert.ok(engineHpp.includes(sym), `engine.hpp 缺稳定性标注：${sym}`);
    }
});

// ---------------------------------------------------------------------------
// 判据 7-14：状态迁移
// ---------------------------------------------------------------------------
test('判据 7-14：状态迁移；验证方式：tick 接线静态 + 机器/活引擎单测跑绿', () => {
    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));

    // (a) 静态：tick 驱动 call-state，且跑在 poll_ice_recovery() 之后
    //     （recovering 闸门才能在同一 tick 看到 N3 机的状态）。
    assert.ok(transport.includes('poll_call_state();'),
        'tick 必须调用 poll_call_state');
    const recoveryIdx = transport.indexOf('poll_ice_recovery();');
    const callStateIdx = transport.indexOf('poll_call_state();');
    assert.ok(recoveryIdx >= 0 && callStateIdx >= 0,
        'poll_ice_recovery / poll_call_state 必须在 tick 中同时存在');
    assert.ok(recoveryIdx < callStateIdx,
        'poll_call_state 必须排在 poll_ice_recovery 之后');

    // (b) 静态：reconnecting 由 N3 重连点进入；终态由 N3 终态进入 kIceFailed。
    assert.match(transport, /enter_reconnecting\(\)/,
        'execute_ice_recovery_restart 必须在重连时进入 kReconnecting');
    assert.match(transport, /fail_call_state\(CallFailure::kIceFailed/,
        'N3 终态必须 fail(kIceFailed, 当前 N4 reason)');

    // (c) 静态：每次报告附上 N3 / N8 细节（判据 13 / 14 的接线）。
    const fire = transport.slice(
        transport.indexOf('void NimRTCEngine::fire_call_state'),
        transport.indexOf('void NimRTCEngine::on_transport_recv'));
    for (const needle of ['restart_count', 'remaining_budget', 'terminal',
                          'path_change_reason', 'old_pair', 'new_pair']) {
        assert.ok(fire.includes(needle), `fire_call_state 必须附上 ${needle}`);
    }

    // (d) 机器级：判据 12 —— 同状态不重复报（连续两次同观察 → 第二次不发）。
    runGtestCases('test_engine_call_state', [
        'EngineCallState.FirstObservationAlwaysReportsConnecting',
        'EngineCallState.ObserveTranslatesTheSixIceStates',
        'EngineCallState.SameStateNotRepeated',
        'EngineCallState.ReconnectingRoundTrip',
    ], '状态机基础判据');

    // (e) 活引擎：判据 7 → kConnecting；判据 8 / 12 → 一次 kConnected 后静默。
    runGtestCases('test_engine_call_state', [
        'EngineCallStateLive.OpenReportsConnecting',
        'EngineCallStateLive.IceUpReportsConnectedThenStaysQuiet',
    ], 'kConnecting / kConnected 迁移');

    // (f) 活引擎：判据 9 / 13 / 14 —— N3 重连 → 一次 kReconnecting，
    //     带 restart_count=1 / remaining_budget=4 / path_change_reason。
    runGtestCases('test_engine_call_state', [
        'EngineCallStateLive.N3RestartReportsReconnectingWithDetails',
    ], 'N3 重连 kReconnecting');

    // (g) 活引擎：判据 10 / 15 —— N3 终态 → 一次 kFailed + kIceFailed + N4 reason。
    runGtestCases('test_engine_call_state', [
        'EngineCallStateLive.N3TerminalReportsFailedWithIceFailed',
    ], 'N3 终态 kFailed');
});

// ---------------------------------------------------------------------------
// 判据 15-21：失败原因
// ---------------------------------------------------------------------------
test('判据 15-21：失败原因；验证方式：failure_from_reason 单测 + 枚举静态 + 活引擎', () => {
    // (a) 判据 15-19：六个 N4 词汇的映射（含 "none" → kNone，不误报）。
    runGtestCases('test_engine_call_state', [
        'EngineCallState.FailureFromReasonMapsTheN4Vocabulary',
    ], 'N4 失败词汇 → 枚举映射');

    // (b) 判据 15 的「原样带进」：活引擎终态报告把 N4 字符串放进 failure_reason
    //     （N3TerminalReportsFailedWithIceFailed 断言 reason == "checks-failed"）。
    //     静态：fail_call_state 接受 (CallFailure, std::string_view) 两个参数，
    //     失败字符串由调用点原样传入。
    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    assert.match(transport,
        /void NimRTCEngine::fail_call_state\(CallFailure failure,\s*std::string_view reason\)/,
        'fail_call_state 必须带 (failure, reason) 两个参数');

    // (c) 判据 20：DTLS 失败 → kDtlsFailed（活引擎，且 kFailed 是吸收态）。
    runGtestCases('test_engine_call_state', [
        'EngineCallStateLive.DtlsFailureReportsFailedAndStaysFailed',
    ], 'DTLS 失败 kDtlsFailed');

    // (d) 判据 21：媒体失败 → kMediaFailed —— 机器级 fail() 接受枚举原样携带
    //     （ResetClearsEverything 用 kMediaFailed 进入失败态并读出），
    //     且 FailCarriesFailureAndReason / FailedIsAbsorbing 钉住承载与吸收。
    runGtestCases('test_engine_call_state', [
        'EngineCallState.FailCarriesFailureAndReason',
        'EngineCallState.FailedIsAbsorbing',
        'EngineCallState.ResetClearsEverything',
    ], 'kMediaFailed 承载与吸收态');
});

// ---------------------------------------------------------------------------
// 判据 22-25：生命周期
// ---------------------------------------------------------------------------
test('判据 22-25：生命周期；验证方式：engine.hpp/engine.cpp 静态 + 活引擎单测', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    const engineCpp = readText(path.join(ROOT, 'src/engine/src/engine.cpp'));

    // (a) 判据 22：start(config) 存在，一次调用完成「配置 → open」。
    assert.match(engineHpp,
        /NIMRTC_API_STABLE int start\(const EngineConfig& config\) noexcept/,
        'start() 必须存在且接受 EngineConfig');
    assert.match(engineCpp,
        /int NimRTCEngine::start\(const EngineConfig& config\) noexcept \{[\s\S]*?open\(\);/,
        'start() 内部必须调 open()');

    // (b) 判据 23：stop() 存在，关闭顺序 = close() 的顺序
    //     （QoE → ICE → 音频插件 → 编解码 → 视频插件 → BWE → TransportSession）。
    assert.match(engineHpp, /NIMRTC_API_STABLE void\s+stop\(\) noexcept/,
        'stop() 必须存在');
    assert.match(engineCpp, /void NimRTCEngine::stop\(\) noexcept \{[\s\S]*?close\(\);/,
        'stop() 必须是 close() 的生命周期名字');
    const closeBody = engineCpp.slice(
        engineCpp.indexOf('void NimRTCEngine::close() noexcept'),
        engineCpp.indexOf('int NimRTCEngine::start('));
    before(closeBody, 'qoe.close();', 'ice_t_->close();', '关闭顺序 QoE→ICE');
    before(closeBody, 'ice_t_->close();', 'audio3a_plugin_->close();', '关闭顺序 ICE→音频插件');
    before(closeBody, 'audio3a_plugin_->close();', 'codec_plugin_->close();', '关闭顺序 音频插件→编解码');
    before(closeBody, 'codec_plugin_->close();', 'shutdown_video_plugins();', '关闭顺序 编解码→视频插件');
    before(closeBody, 'shutdown_video_plugins();', 'shutdown_bwe_scheduler();', '关闭顺序 视频插件→BWE');
    before(closeBody, 'shutdown_bwe_scheduler();', 'impl_->session.close();', '关闭顺序 BWE→TransportSession');

    // (c) 判据 24：hangup(reason) 存在，带原因，且先通知对端再关。
    assert.match(engineHpp,
        /NIMRTC_API_STABLE bool\s+hangup\(std::string_view reason\) noexcept/,
        'hangup() 必须存在且带 reason');
    assert.match(engineCpp, /bool NimRTCEngine::hangup\(\s*std::string_view reason\)/,
        'hangup() 实现必须存在');
    const hangupBody = engineCpp.slice(
        engineCpp.indexOf('bool NimRTCEngine::hangup('),
        engineCpp.indexOf('// create_data_channel'));
    assert.ok(hangupBody.includes('send_control_message'),
        'hangup() 必须先通知对端（send_control_message）');
    assert.ok(hangupBody.includes('close();'),
        'hangup() 通知后必须按 close() 顺序关');
    before(hangupBody, 'send_control_message', 'close();',
        'hangup() 必须先通知再关');

    // (d) 判据 25：stop() / hangup() 之后 set_on_call_state 收到 kClosed。
    runGtestCases('test_engine_call_state', [
        'EngineCallStateLive.StartReportsConnectingAndStopReportsClosed',
        'EngineCallStateLive.HangupReportsClosedAndIsIdempotent',
    ], 'stop/hangup → kClosed');
});

// ---------------------------------------------------------------------------
// 判据 26-30：边界与回归
// ---------------------------------------------------------------------------
test('判据 26-30：边界与回归；验证方式：静态 + 本片单测 + 全套回归', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));

    // (a) 判据 26：回调在引擎 tick 线程上（不是宿主线程）。
    runGtestCases('test_engine_call_state', [
        'EngineCallStateLive.CallbackFiresOnTheTickThread',
    ], '回调线程');

    // (b) 判据 27：src/engine/ 不 include nimrtc/ice/ 的具体头（L4 接缝纪律）。
    for (const rel of ['src/engine/src/engine_call_state.hpp',
                       'src/engine/src/engine_call_state.cpp',
                       'src/engine/src/engine_transport.cpp',
                       'src/engine/src/engine.cpp',
                       'src/engine/src/engine_session_state.hpp',
                       'src/engine/src/engine_transport_stats.cpp']) {
        const src = readText(path.join(ROOT, rel));
        assert.ok(!src.includes('#include <nimrtc/ice/'),
            `${rel} 不得 include nimrtc/ice/ 的具体头（L4）`);
    }

    // (c) 判据 28：set_on_state_change / set_on_error / set_on_control_message
    //     签名不变。
    assert.match(engineHpp,
        /set_on_state_change\(EngineStateCallback cb\) noexcept/,
        'set_on_state_change 签名必须不变');
    assert.match(engineHpp,
        /set_on_error\(EngineErrorCallback cb\) noexcept/,
        'set_on_error 签名必须不变');
    assert.match(engineHpp,
        /set_on_control_message\(\s*std::function<void\(std::span<const std::uint8_t>\)>\s*cb\s*\)\s*noexcept/,
        'set_on_control_message 签名必须不变');

    // (d) 判据 30：plugins::IICETransport 接口不变 —— 关键虚方法仍在，
    //     且本片不触碰 src/plugins/（写集边界断言覆盖后者）。
    const ice = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp'));
    for (const m of ['class NIMRTC_API_STABLE IICETransport',
                     'virtual IceState state() const noexcept = 0;',
                     'virtual std::string local_ufrag()    const noexcept = 0;',
                     'virtual Status restart() noexcept',
                     'virtual std::uint32_t restart_count() const noexcept',
                     'virtual void set_on_ice_restart(OnIceRestart cb) noexcept']) {
        assert.ok(ice.includes(m), `plugins::IICETransport 缺 ${m}（接口必须不变）`);
    }

    // (e) 判据 29：test_engine_*.cpp 一整套回归全绿（含本片新增的二进制）。
    const regression = [
        { exe: 'test_engine_device_policy', note: 'D3/D4 设备策略与采集重连' },
        { exe: 'test_engine_audio_device',  note: 'D1 音频设备层' },
        { exe: 'test_engine_connectivity',  note: 'D6/N4 连通性诊断与 profile' },
        { exe: 'test_engine_ice_recovery',  note: 'N3 引擎重连策略' },
        { exe: 'test_engine_turn_credentials', note: 'N7 TURN 凭据接缝' },
        { exe: 'test_engine_path_change',   note: 'N8 路径变更报告' },
        { exe: 'test_engine_call_state',    note: '本片（v1-call-state-surface）' },
    ];
    for (const r of regression) {
        const res = runGtest('src/engine/tests', r.exe, '*');
        assert.equal(res.status, 0,
            `${r.note}（${r.exe}）未通过（exit=${res.status}）：\n${res.out}`);
        assert.ok(res.out.includes('[==========]'),
            `${r.note}（${r.exe}）没有产出 gtest 运行输出：\n${res.out}`);
    }
});

// ---------------------------------------------------------------------------
// 判据 31-32：文档与契约
// ---------------------------------------------------------------------------
test('判据 31-32：文档与契约；验证方式：engine_api.md 静态 + 两个只读脚本跑绿', () => {
    // (a) 判据 31：engine_api.md 记录新枚举、报告结构、生命周期三个方法、
    //     状态迁移图。
    const api = readText(path.join(ROOT, 'docs/api/engine_api.md'));
    for (const needle of ['CallState', 'CallFailure', 'CallStateReport',
                          'set_on_call_state', 'start(', 'stop(', 'hangup(']) {
        assert.ok(api.includes(needle), `engine_api.md 缺 ${needle}`);
    }
    // 迁移图：connecting → connected → reconnecting → failed / closed。
    for (const step of ['connecting', 'connected', 'reconnecting',
                        'failed', 'closed']) {
        assert.ok(api.includes(step), `engine_api.md 缺迁移图节点 ${step}`);
    }

    // (b) 判据 32：公开头清单与实际公开面一致（只读脚本，不改它；本片不新增头）。
    const m = spawnSync('python', ['scripts/check_api_manifest.py'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(m.status, 0,
        `check_api_manifest.py 未通过（exit=${m.status}）：\n` +
        `${m.stdout || ''}${m.stderr || ''}`);
    assert.match(`${m.stdout || ''}${m.stderr || ''}`, /OK/,
        'check_api_manifest.py 应输出 OK');
});

// ---------------------------------------------------------------------------
// 验收文件自身是真实断言（门禁②：无 todo 残留）
// ---------------------------------------------------------------------------
test('验收文件自身是真实断言；验证方式：node --test 全部通过 + 无 todo 用例', () => {
    const own = readText(__filename);
    const todoNeedle = ['test', 'todo('].join('.');
    assert.ok(!own.includes(todoNeedle),
        '验收文件不得残留 todo 用例（门禁②要求每条用例都是可执行断言）');
});

// ---------------------------------------------------------------------------
// 写集边界断言（ADR-075）
// ---------------------------------------------------------------------------
test('本片改动仅限写集范围；验证方式：改动清单不越界 + 产物在树上', () => {
    // 写集 = 规格「范围」（specs/slices/v1-call-state-surface.md §范围），
    // 与现状验收的允许清单一致。
    const ALLOW = [
        'src/engine/src/engine_call_state.hpp',
        'src/engine/src/engine_call_state.cpp',
        'src/engine/src/engine.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/src/engine_session_state.hpp',
        'src/engine/src/engine_transport_stats.cpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/CMakeLists.txt',
        'src/engine/tests/CMakeLists.txt',
        'src/engine/tests/test_engine_call_state.cpp',
        'tests/acceptance/v1-call-state-surface',
        'docs/adr/ADR-035-call-state-surface.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
        'specs/slices/v1-call-state-surface.md',
        // —— 会话开始前已存在的脏状态（不属于本片 agent 的改动）——
        'src/third_party/wolfssl/src',
    ];

    const REQUIRED = [
        'src/engine/src/engine_call_state.hpp',
        'src/engine/src/engine_call_state.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/src/engine.cpp',
        'src/engine/src/engine_session_state.hpp',
        'src/engine/src/engine_transport_stats.cpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/CMakeLists.txt',
        'src/engine/tests/CMakeLists.txt',
        'src/engine/tests/test_engine_call_state.cpp',
        'tests/acceptance/v1-call-state-surface/acceptance.test.cjs',
        'docs/adr/ADR-035-call-state-surface.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-call-state-surface' });
    // ② 产物检查（状态判据，ADR-075）：产物必须在树上，与合并策略无关。
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

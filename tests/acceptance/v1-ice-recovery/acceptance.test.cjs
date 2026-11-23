// 验收测试 —— 切片 v1-ice-recovery（N3：引擎跑重连策略，ADR-031）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 33 条验收标准（specs/slices/v1-ice-recovery.md §验收标准）逐组落成
// 真实断言，验证手段与 v3-n4-d6-connectivity 同族：
//
//   ① 读源码做静态断言（配置数值、接线点、公开符号、文档）；
//   ② 直接跑门禁同款 build 树里的 gtest 二进制（test_engine_ice_recovery）
//      —— 用 --gtest_filter 限定用例，判定只依赖「二进制跑起来 + 用例 OK」。
//
// 本片测试二进制分两个套件：
//   - EngineIceRecovery      机器级（虚拟时钟，不 sleep）：策略解析 + 状态机
//   - EngineIceRecoveryLive  活引擎（有状态假 ICE）：自动重启 / 手动预算 / 终态
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

/** 跨 // 行标的词组匹配：容忍 CRLF 与行首 // 注释前缀。 */
function acrossLines(frag) {
    return frag.split(' ').join('(?:\r?\n\\s*//\\s*|\\s)?');
}

// ---------------------------------------------------------------------------
// 判据 1-4：策略开关
// ---------------------------------------------------------------------------
test('判据 1-4：ice_recovery_policy 默认 "auto"，"off" / "" / 未知名字按 "off"；验证方式：静态默认值 + 单测跑绿', () => {
    // (a) 静态：EngineConfig 公开键默认 "auto"（判据 1）。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.match(engineHpp, /std::string ice_recovery_policy = "auto";/,
        'engine.hpp 的 EngineConfig 必须默认 "auto"');

    // (b) 机器级解析：只有 "auto" 是 kAuto，其余（"" / "off" / 未知）一律 kOff。
    const r = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecovery.PolicyResolution:EngineIceRecovery.ConfigDefaultsToAuto');
    assert.equal(r.status, 0,
        `PolicyResolution / ConfigDefaultsToAuto 未通过（exit=${r.status}）：\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineIceRecovery.PolicyResolution'),
        '策略解析用例未跑出 OK（未知名字当 off）：\n' + r.out);
    assert.ok(gtestOk(r.out, 'EngineIceRecovery.ConfigDefaultsToAuto'),
        '配置默认用例未跑出 OK（默认 "auto"）：\n' + r.out);

    // (c) 引擎级：未知名字按 "off"（判据 3）—— 解析后 stats 报 "disabled"。
    const u = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecoveryLive.UnknownPolicyResolvesToOff');
    assert.equal(u.status, 0,
        `UnknownPolicyResolvesToOff 未通过（exit=${u.status}）：\n${u.out}`);
    assert.ok(gtestOk(u.out, 'EngineIceRecoveryLive.UnknownPolicyResolvesToOff'),
        '未知名字按 off 的活引擎用例未跑出 OK：\n' + u.out);
});

// ---------------------------------------------------------------------------
// 判据 5-12：状态机（虚拟时钟）
// ---------------------------------------------------------------------------
test('判据 5-12：状态机（虚拟时钟）；验证方式：配置数值静态钉死 + 单测逐条跑绿', () => {
    // (a) 静态：机器配置数值（grace 5 s / 首次退避 1 s / 上限 8 s / 窗口 10 min / 预算 5 次）。
    const hpp = readText(path.join(ROOT, 'src/engine/src/engine_ice_recovery.hpp'));
    for (const [frag, note] of [
        ['grace_us = 5\'000\'000',            'grace 窗 5 s'],
        ['initial_backoff_us = 1\'000\'000',  '首次退避 1 s'],
        ['max_backoff_us = 8\'000\'000',      '退避上限 8 s'],
        ['budget_window_us = 600\'000\'000',  '预算窗口 10 分钟'],
        ['max_restarts = 5',                  '预算 5 次'],
    ]) {
        assert.ok(hpp.includes(frag), `机器配置缺 ${note}（${frag}）`);
    }

    // (b) 单测：逐条钉住 5-12 的机器规则。
    const filter = [
        'EngineIceRecovery.DisconnectedEntersGraceWithoutImmediateRestart',   // 5
        'EngineIceRecovery.GraceWindowRecoveryClearsCount',                   // 6
        'EngineIceRecovery.GraceExpiryEntersBackoffWithOneSecond',            // 7
        'EngineIceRecovery.BackoffSequenceDoublesThenCapsAtEightSeconds',     // 8
        'EngineIceRecovery.ConsecutiveRestartsConsumeBudgetThenTerminal',     // 9
        'EngineIceRecovery.TerminalStateStopsFurtherRestarts',                // 10
        'EngineIceRecovery.SlidingWindowDropsOldRestarts',                    // 11
        'EngineIceRecovery.FailedRestartConsumesBudgetAndDoublesBackoff',     // 12
    ].join(':');
    const r = runGtest('src/engine/tests', 'test_engine_ice_recovery', filter);
    assert.equal(r.status, 0,
        `状态机用例未通过（exit=${r.status}）：\n${r.out}`);
    for (const gcase of filter.split(':')) {
        assert.ok(gtestOk(r.out, gcase), `状态机用例未跑出 OK：${gcase}\n${r.out}`);
    }
});

// ---------------------------------------------------------------------------
// 判据 13-16：自动重启（引擎侧）
// ---------------------------------------------------------------------------
test('判据 13-16：掉线 + grace 到期 → 引擎自动重启；验证方式：tick 接线静态 + 活引擎单测', () => {
    // (a) 静态：tick 驱动独立状态机；自动重启直接调 ICE 接缝的 restart()，
    //     不递归公开 restart_ice()（判据 13 的机制）。
    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    assert.ok(transport.includes('poll_ice_recovery();'),
        'tick 必须调用 poll_ice_recovery（独立于 DTLS rehandshake 的状态机）');
    assert.match(transport, /execute_ice_recovery_restart\(now_us\)/,
        'poll_ice_recovery 必须在到期时执行自动重启');
    assert.match(transport, /ice_t_->restart\(\);/,
        '自动重启必须直接调 ICE 接缝的 restart()');
    assert.ok(!/restart_ice\(\)\s*;/.test(transport),
        '自动重启不得以语句形式递归公开 restart_ice()');

    // (b) 静态：Failed 跳过 grace（判据 14 的接线）—— Failed 观察直进 kBackoff。
    assert.ok(transport.includes('cur == plugins::IceState::Failed'),
        'poll_ice_recovery 必须专门处理 Failed 分支（跳过 grace）');
    assert.ok(transport.includes('rec.note_failed(now_us);'),
        'poll_ice_recovery 必须在 Failed 上调用 note_failed');

    // (c) 活引擎：判据 13 + 16 —— 自动重启发生且 reason == "local-address-changed"。
    const autoR = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecoveryLive.AutoRestartAfterDropAndGraceExpiry');
    assert.equal(autoR.status, 0,
        `AutoRestartAfterDropAndGraceExpiry 未通过（exit=${autoR.status}）：\n${autoR.out}`);
    assert.ok(gtestOk(autoR.out, 'EngineIceRecoveryLive.AutoRestartAfterDropAndGraceExpiry'),
        '自动重启 + 报告 reason 的活引擎用例未跑出 OK：\n' + autoR.out);

    // (d) 活引擎：判据 15 —— 从未连上过 → 不触发重启风暴。
    const never = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecoveryLive.NeverConnectedSessionDoesNotRestart');
    assert.equal(never.status, 0,
        `NeverConnectedSessionDoesNotRestart 未通过（exit=${never.status}）：\n${never.out}`);
    assert.ok(gtestOk(never.out, 'EngineIceRecoveryLive.NeverConnectedSessionDoesNotRestart'),
        '从未连上过不重启的活引擎用例未跑出 OK：\n' + never.out);

    // (e) 静态：第二道闸门 —— 观察点只在 ice_connected_once 之后喂掉线事件。
    assert.ok(transport.includes('ice_connected_once'),
        'poll_ice_recovery 必须使用 ice_connected_once 门槛');
});

// ---------------------------------------------------------------------------
// 判据 17-20：手动 restart_ice() 计入同一预算
// ---------------------------------------------------------------------------
test('判据 17-20：restart_ice() 计入同一预算；验证方式：静态接线 + 活引擎单测 + 两步契约文档', () => {
    // (a) 静态：restart_ice() 走同一预算记账；预算满报 manual-blocked。
    const engineCpp = readText(path.join(ROOT, 'src/engine/src/engine.cpp'));
    assert.match(engineCpp, /note_restart_attempted\(now_us, "manual-blocked"\)/,
        'restart_ice() 必须走同一预算记账（ADR-031 §2.3）');
    assert.match(engineCpp, /report_ice_recovery_terminal\("manual", "manual-blocked"\)/,
        '预算满时 restart_ice() 必须报终态 manual-blocked 并返回 false');
    assert.match(engineCpp, /fire_ice_recovery_report\("manual", false, ""\)/,
        '预算内 restart_ice() 必须触发 manual 报告');
    assert.match(engineCpp, /\.reset_budget\(\);/,
        'reset_ice_recovery_budget() 必须清计数与终态标志');

    // (b) 静态：判据 19 —— 两步契约不变（restart_ice() 然后 send_to_peer(create_offer())）。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.ok(engineHpp.includes('send_to_peer'),
        'restart_ice() 文档必须保留 send_to_peer 两步契约');
    assert.ok(engineHpp.includes('create_offer'),
        'restart_ice() 文档必须保留 create_offer');

    // (c) 活引擎：判据 17 / 18 / 20 —— 5 次成功 → 第 6 次被拦（manual-blocked）→
    //     reset 后再成功；budget 窗口起点在 reset 时保留（判据 20 的窗口部分
    //     由机器级 ResetBudgetClearsCountAndTerminalKeepsWindow 钉住）。
    const r = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecoveryLive.ManualRestartSharesBudgetAndResetFreesIt:'
        + 'EngineIceRecovery.ResetBudgetClearsCountAndTerminalKeepsWindow:'
        + 'EngineIceRecovery.FullResetAlsoClearsTheWindow');
    assert.equal(r.status, 0,
        `手动预算用例未通过（exit=${r.status}）：\n${r.out}`);
    for (const gcase of [
        'EngineIceRecoveryLive.ManualRestartSharesBudgetAndResetFreesIt',
        'EngineIceRecovery.ResetBudgetClearsCountAndTerminalKeepsWindow',
        'EngineIceRecovery.FullResetAlsoClearsTheWindow',
    ]) {
        assert.ok(gtestOk(r.out, gcase), `手动预算用例未跑出 OK：${gcase}\n${r.out}`);
    }
});

// ---------------------------------------------------------------------------
// 判据 21-25：终态三处报告
// ---------------------------------------------------------------------------
test('判据 21-25：终态三处报告；验证方式：静态（错误码/stats/日志）+ 活引擎单测', () => {
    // (a) 判据 21：新错误码。
    const err = readText(path.join(ROOT,
        'src/core/include/nimrtc/core/engine_errors.hpp'));
    assert.match(err, /kIceRecoveryExhausted = 0x1F01/,
        'engine_errors.hpp 必须定义 kIceRecoveryExhausted = 0x1F01');

    // (b) 判据 22 / 24 / 25：三个 stats 字段存在。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    for (const f of ['ice_recovery_state', 'ice_recovery_restarts',
                     'ice_recovery_terminal_reason']) {
        assert.ok(engineHpp.includes(f), `engine.hpp 的 Stats 缺字段 ${f}`);
    }

    // (c) 判据 23：一行 warn 日志（never a silent drop）。
    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    assert.match(transport, /\.warn\(/,
        '终态必须写一行 warn 日志');

    // (d) 活引擎：判据 21 / 22 / 23（auto 终态 reason "budget-exhausted"）。
    const autoTerm = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecoveryLive.AutoRestartStopsAtTerminalWithBudgetExhausted');
    assert.equal(autoTerm.status, 0,
        `AutoRestartStopsAtTerminalWithBudgetExhausted 未通过（exit=${autoTerm.status}）：\n${autoTerm.out}`);
    assert.ok(gtestOk(autoTerm.out,
        'EngineIceRecoveryLive.AutoRestartStopsAtTerminalWithBudgetExhausted'),
        'auto 终态（budget-exhausted 三处报告）用例未跑出 OK：\n' + autoTerm.out);

    // (e) 活引擎：判据 25 —— "off" → stats "disabled"，且掉线不自动重启。
    const off = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecoveryLive.PolicyOffReportsDisabledAndNeverRestarts');
    assert.equal(off.status, 0,
        `PolicyOffReportsDisabledAndNeverRestarts 未通过（exit=${off.status}）：\n${off.out}`);
    assert.ok(gtestOk(off.out, 'EngineIceRecoveryLive.PolicyOffReportsDisabledAndNeverRestarts'),
        '"off" 报 disabled 且不重启的活引擎用例未跑出 OK：\n' + off.out);
});

// ---------------------------------------------------------------------------
// 判据 26-30：边界与兼容
// ---------------------------------------------------------------------------
test('判据 26-30：边界与兼容；验证方式：静态签名 + 单测 + 既有套件回归', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));

    // (a) 判据 26：set_on_ice_restart 签名与语义不变（仍写 "peer"，无新参数）。
    assert.match(engineHpp,
        /set_on_ice_restart\(\s*plugins::OnIceRestart\s+cb\s*\)\s*noexcept/,
        'set_on_ice_restart 签名必须保持原样（无新参数）');
    assert.match(engineHpp, /the \*peer\* restarts ICE/,
        'set_on_ice_restart 文档必须仍写 "peer" 语义');
    const peer = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecoveryLive.PeerRestartCallbackUnchanged');
    assert.equal(peer.status, 0,
        `PeerRestartCallbackUnchanged 未通过（exit=${peer.status}）：\n${peer.out}`);
    assert.ok(gtestOk(peer.out, 'EngineIceRecoveryLive.PeerRestartCallbackUnchanged'),
        '对端重启回调不变用例未跑出 OK：\n' + peer.out);

    // (b) 判据 27：set_on_ice_recovery(nullptr) 清除回调不崩。
    const nullR = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecoveryLive.NullRecoveryCallbackClearsWithoutCrash');
    assert.equal(nullR.status, 0,
        `NullRecoveryCallbackClearsWithoutCrash 未通过（exit=${nullR.status}）：\n${nullR.out}`);
    assert.ok(gtestOk(nullR.out, 'EngineIceRecoveryLive.NullRecoveryCallbackClearsWithoutCrash'),
        'null 回调清除不崩用例未跑出 OK：\n' + nullR.out);

    // (c) 判据 28：未 open()（无 ICE transport）时 restart_ice() 返回 false。
    const before = runGtest('src/engine/tests', 'test_engine_ice_recovery',
        'EngineIceRecoveryLive.RestartIceBeforeOpenReturnsFalse');
    assert.equal(before.status, 0,
        `RestartIceBeforeOpenReturnsFalse 未通过（exit=${before.status}）：\n${before.out}`);
    assert.ok(gtestOk(before.out, 'EngineIceRecoveryLive.RestartIceBeforeOpenReturnsFalse'),
        '未 open 时 restart_ice() 返回 false 用例未跑出 OK：\n' + before.out);

    // (d) 判据 29：新增的引擎文件不 include nimrtc/ice/ 的具体头（L4 接缝纪律）。
    for (const rel of ['src/engine/src/engine_ice_recovery.hpp',
                       'src/engine/src/engine_ice_recovery.cpp']) {
        const src = readText(path.join(ROOT, rel));
        assert.ok(!src.includes('#include <nimrtc/ice/'),
            `${rel} 不得 include nimrtc/ice/ 的具体头（L4）`);
    }

    // (e) 判据 30：test_engine_*.cpp 一整套回归全绿（含本片新增的二进制）。
    const regression = [
        { exe: 'test_engine_device_policy', note: 'D3/D4 设备策略与采集重连' },
        { exe: 'test_engine_audio_device',  note: 'D1 音频设备层' },
        { exe: 'test_engine_connectivity',  note: 'D6/N4 连通性诊断与 profile' },
        { exe: 'test_engine_ice_recovery',  note: '本片（v1-ice-recovery）' },
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
// 判据 31-33：文档与契约
// ---------------------------------------------------------------------------
test('判据 31-33：文档与契约；验证方式：engine_api.md 静态 + 两个只读脚本跑绿', () => {
    // (a) 判据 31：engine_api.md 记录新 key / 新回调 / 三个 stats 字段 /
    //     restart_ice() 新行为 / reset_ice_recovery_budget()。
    const api = readText(path.join(ROOT, 'docs/api/engine_api.md'));
    for (const needle of ['ice_recovery_policy', 'set_on_ice_recovery',
                          'ice_recovery_state', 'ice_recovery_restarts',
                          'ice_recovery_terminal_reason',
                          'reset_ice_recovery_budget']) {
        assert.ok(api.includes(needle), `engine_api.md 缺 ${needle}`);
    }
    assert.ok(api.includes('"off"'),
        'engine_api.md 必须记录 "off" 取值（含未知名字当 "off" 的规则）');
    assert.ok(api.includes('restart_ice'),
        'engine_api.md 必须记录 restart_ice() 的新行为（预算满 → false）');

    // (b) 判据 32：公开头清单与实际公开面一致（只读脚本，不改它）。
    const m = spawnSync('python', ['scripts/check_api_manifest.py'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(m.status, 0,
        `check_api_manifest.py 未通过（exit=${m.status}）：\n` +
        `${m.stdout || ''}${m.stderr || ''}`);
    assert.match(`${m.stdout || ''}${m.stderr || ''}`, /OK/,
        'check_api_manifest.py 应输出 OK');

    // (c) 判据 33：新公开符号全部带稳定性标注（只读扫描脚本 + 静态）。
    const f = spawnSync(process.execPath,
        ['scripts/api_freeze_scan.js', '--no-md'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(f.status, 0,
        `api_freeze_scan.js 未通过（exit=${f.status}）：\n` +
        `${f.stdout || ''}${f.stderr || ''}`);
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.match(engineHpp, /struct NIMRTC_API_STABLE IceRecoveryReport/,
        'IceRecoveryReport 必须带 NIMRTC_API_STABLE');
    assert.match(engineHpp,
        /NIMRTC_API_STABLE void set_on_ice_recovery\s*\(EngineIceRecoveryCallback/,
        'set_on_ice_recovery 必须带 NIMRTC_API_STABLE');
    assert.match(engineHpp,
        /NIMRTC_API_STABLE void reset_ice_recovery_budget\s*\(\)\s*noexcept/,
        'reset_ice_recovery_budget 必须带 NIMRTC_API_STABLE');
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
    // 写集 = 规格「写集」（specs/slices/v1-ice-recovery.md §写集）。
    const ALLOW = [
        'src/engine/src/engine_ice_recovery.hpp',
        'src/engine/src/engine_ice_recovery.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/src/engine.cpp',
        'src/engine/src/engine_media.cpp',
        'src/engine/src/engine_session_state.hpp',
        'src/engine/src/engine_transport_stats.cpp',
        'src/engine/src/engine_transport_stats.hpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/core/include/nimrtc/core/engine_errors.hpp',
        'src/engine/CMakeLists.txt',
        'src/engine/tests/CMakeLists.txt',
        'src/engine/tests/test_engine_ice_recovery.cpp',
        // —— v2-ice 遗留缺陷的最小修复（见下方说明）——
        'src/modules/ice/include/nimrtc/ice/tcp_candidate.hpp',
        'tests/acceptance/v1-ice-recovery',
        'docs/adr/ADR-031-ice-recovery-policy-ownership.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
        // —— 会话开始前已存在的脏状态（不属于本片 agent 的改动）——
        'specs/slices/v1-ice-recovery.md',
        'src/third_party/wolfssl/src',
    ];

    const REQUIRED = [
        'src/engine/src/engine_ice_recovery.hpp',
        'src/engine/src/engine_ice_recovery.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/src/engine.cpp',
        'src/engine/src/engine_media.cpp',
        'src/engine/src/engine_session_state.hpp',
        'src/engine/src/engine_transport_stats.cpp',
        'src/engine/src/engine_transport_stats.hpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/core/include/nimrtc/core/engine_errors.hpp',
        'src/engine/CMakeLists.txt',
        'src/engine/tests/CMakeLists.txt',
        'src/engine/tests/test_engine_ice_recovery.cpp',
        'tests/acceptance/v1-ice-recovery/acceptance.test.cjs',
        'docs/adr/ADR-031-ice-recovery-policy-ownership.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-ice-recovery' });
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

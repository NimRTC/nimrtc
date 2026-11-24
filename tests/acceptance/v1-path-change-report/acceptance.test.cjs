// 验收测试 —— 切片 v1-path-change-report（N8：路径变更上报，ADR-033）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 25 条验收标准（specs/slices/v1-path-change-report.md §验收标准）逐组落成
// 真实断言，验证手段与 v1-ice-recovery 同族：
//
//   ① 读源码做静态断言（公开面、接线点、词汇、文档）；
//   ② 直接跑门禁同款 build 树里的 gtest 二进制（test_engine_path_change）
//      —— 用 --gtest_filter 限定用例，判定只依赖「二进制跑起来 + 用例 OK」。
//
// 本片测试二进制分两个套件：
//   - PathChangeTracker        机器级（纯函数）：classify 五种情形 + same_path +
//                              note_before/before/reset 记账往返
//   - EnginePathChangeLive     活引擎（有状态假 ICE，selected_pair 可设）：N3 自动重启 /
//                              手动重启 / 对端重启三条接线
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

/** 跑一串 gtest 用例并断言每条都 OK。 */
function runAndCheck(sub, exe, filter, note) {
    const cases = filter.split(':');
    const r = runGtest(sub, exe, filter);
    assert.equal(r.status, 0,
        `${note} 未通过（exit=${r.status}）：\n${r.out}`);
    for (const gcase of cases) {
        assert.ok(gtestOk(r.out, gcase), `${note} 用例未跑出 OK：${gcase}\n${r.out}`);
    }
}

// ---------------------------------------------------------------------------
// 判据 1-7：公开面
// ---------------------------------------------------------------------------
test('判据 1-7：公开面；验证方式：engine.hpp 静态 + 单测跑绿', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));

    // (a) 判据 1：IceRecoveryReport 新增 old_pair / new_pair（std::optional<IceCandidatePairInfo>）。
    assert.match(engineHpp, /std::optional<IceCandidatePairInfo> old_pair;/,
        'IceRecoveryReport 必须新增 old_pair');
    assert.match(engineHpp, /std::optional<IceCandidatePairInfo> new_pair;/,
        'IceRecoveryReport 必须新增 new_pair');

    // (b) 判据 2：既有字段语义不变 —— 五个字段仍在（本片只加不改）。
    for (const frag of ['std::string reason;',
                        'std::uint32_t restart_count = 0;',
                        'std::uint32_t remaining_budget = 0;',
                        'bool terminal = false;',
                        'std::string terminal_reason;']) {
        assert.ok(engineHpp.includes(frag),
            `IceRecoveryReport 既有字段被改动了：${frag}`);
    }

    // (c) 判据 3：IceCandidatePairInfo 形状不变（两端各带 address/port/protocol/type）。
    assert.match(engineHpp, /struct NIMRTC_API_STABLE IceCandidatePairInfo \{/,
        'IceCandidatePairInfo 必须保留原形状');
    assert.match(engineHpp, /IceCandidateInfo local;/,
        'IceCandidatePairInfo.local 必须保留');
    assert.match(engineHpp, /IceCandidateInfo remote;/,
        'IceCandidatePairInfo.remote 必须保留');

    // (d) 判据 4 / 5：set_on_peer_restart + PeerRestartReport 三字段。
    assert.match(engineHpp,
        /struct NIMRTC_API_STABLE PeerRestartReport \{/,
        'PeerRestartReport 必须存在并带 NIMRTC_API_STABLE');
    assert.match(engineHpp,
        /using NIMRTC_API_STABLE EnginePeerRestartCallback\s*=\s*std::function<void\(const PeerRestartReport&\)>/,
        'EnginePeerRestartCallback 必须是 std::function<void(const PeerRestartReport&)>');
    assert.match(engineHpp,
        /NIMRTC_API_STABLE void set_on_peer_restart\s*\(EnginePeerRestartCallback\s+cb\s*\)\s*noexcept/,
        'set_on_peer_restart 必须存在并带 NIMRTC_API_STABLE');

    // (e) 判据 6：set_on_ice_restart 签名与语义不变（ABI 稳定，ADR-015）。
    assert.match(engineHpp,
        /set_on_ice_restart\(\s*plugins::OnIceRestart\s+cb\s*\)\s*noexcept/,
        'set_on_ice_restart 签名必须保持原样（无新参数）');

    // (f) 判据 7：新公开符号全部带 NIMRTC_API_STABLE —— 只读脚本 + 静态复查。
    const f = spawnSync(process.execPath,
        ['scripts/api_freeze_scan.js', '--no-md'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(f.status, 0,
        `api_freeze_scan.js 未通过（exit=${f.status}）：\n` +
        `${f.stdout || ''}${f.stderr || ''}`);
    assert.match(engineHpp, /struct NIMRTC_API_STABLE IceRecoveryReport \{/,
        'IceRecoveryReport 必须带 NIMRTC_API_STABLE');
    assert.match(engineHpp, /struct NIMRTC_API_STABLE PeerRestartReport \{/,
        'PeerRestartReport 必须带 NIMRTC_API_STABLE');
    assert.match(engineHpp, /using NIMRTC_API_STABLE EnginePeerRestartCallback/,
        'EnginePeerRestartCallback 必须带 NIMRTC_API_STABLE');
    assert.match(engineHpp, /NIMRTC_API_STABLE void set_on_peer_restart/,
        'set_on_peer_restart 必须带 NIMRTC_API_STABLE');
});

// ---------------------------------------------------------------------------
// 判据 8-12：报告什么
// ---------------------------------------------------------------------------
test('判据 8-12：报告什么；验证方式：接线静态 + 活引擎单测', () => {
    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    const engineCpp = readText(path.join(ROOT, 'src/engine/src/engine.cpp'));

    // (a) 判据 8/9 的接线：N3 重启前记账、重启后取新 pair、报告里填 old/new。
    assert.match(transport, /path_change\.note_before\(current_pair\(\)\)/,
        'execute_ice_recovery_restart 必须在重启前记账当前 pair');
    assert.match(transport, /report\.old_pair = before;/,
        'fire_ice_recovery_report 必须把记账的 pair 填进 old_pair');
    assert.match(transport, /report\.new_pair = now_pair;/,
        'fire_ice_recovery_report 必须把当下的 pair 填进 new_pair');

    // (b) 判据 10 的接线：手动重启同样记账，reason "manual"。
    assert.match(engineCpp, /path_change\.note_before\(current_pair\(\)\)/,
        'restart_ice() 必须在重启前记账当前 pair');
    assert.match(engineCpp, /fire_ice_recovery_report\("manual", false, ""\)/,
        'restart_ice() 必须触发 manual 报告');

    // (c) 判据 11 的接线：对端重启转发处报 PeerRestartReport，reason "peer-restart"，
    //     且旧的 set_on_ice_restart 照常触发。
    assert.match(transport, /if \(on_ice_restart_\) on_ice_restart_\(\);/,
        '对端重启必须照常触发旧的 set_on_ice_restart');
    assert.match(transport, /report\.reason = "peer-restart";/,
        '对端重启报告 reason 必须为 "peer-restart"');
    assert.match(transport, /on_peer_restart_\(report\);/,
        '对端重启必须触发 set_on_peer_restart');

    // (d) 判据 8/9：活引擎 —— N3 自动重启带 old（重启前）/ new（重启后）。
    runAndCheck('src/engine/tests', 'test_engine_path_change',
        'EnginePathChangeLive.AutoRestartReportsOldAndNewPairs',
        '判据 8/9（N3 自动重启带 old/new pair）');

    // (e) 判据 10：活引擎 —— 手动重启 reason "manual"，带 pair。
    runAndCheck('src/engine/tests', 'test_engine_path_change',
        'EnginePathChangeLive.ManualRestartReportsManualWithPairs',
        '判据 10（手动重启 manual + pair）');

    // (f) 判据 11：活引擎 —— 对端重启触发新回调、旧回调仍在。
    runAndCheck('src/engine/tests', 'test_engine_path_change',
        'EnginePathChangeLive.PeerRestartFiresNewCallbackAndOldStillFires',
        '判据 11（对端重启 peer-restart）');

    // (g) 判据 12：报告带 N3 的 restart_count / remaining_budget / terminal ——
    //     字段已由判据 2 静态钉住；活引擎侧由 AutoRestart 用例断言
    //     restart_count == 1（budget 5 用 1），N3 回归套件（判据 22）兜底。
    const r = runGtest('src/engine/tests', 'test_engine_path_change',
        'EnginePathChangeLive.AutoRestartReportsOldAndNewPairs');
    assert.ok(gtestOk(r.out,
        'EnginePathChangeLive.AutoRestartReportsOldAndNewPairs'),
        'AutoRestart 用例未跑出 OK：\n' + r.out);
});

// ---------------------------------------------------------------------------
// 判据 13-16：reason 词汇
// ---------------------------------------------------------------------------
test('判据 13-16：reason 词汇；验证方式：机器级 classify 单测 + 接线级 relay 细化', () => {
    // (a) 判据 13：闭合词汇 —— classify 的输出只在五个值里（单测逐条钉住五情形）。
    const filter = [
        'PathChangeTracker.ClassifyBothEmptyIsEmpty',    // 无 pair → ""
        'PathChangeTracker.ClassifyFirstSelection',      // 首次选中 → local-address-changed
        'PathChangeTracker.ClassifyPairDropped',         // pair 掉了 → local-address-changed
        'PathChangeTracker.ClassifyRelayFlip',           // 中继位翻转 → relay-fallback
        'PathChangeTracker.ClassifySameRelayBit',        // 同中继位 → local-address-changed
    ].join(':');
    runAndCheck('src/engine/tests', 'test_engine_path_change', filter,
        '判据 13-16（classify 五情形）');

    // (b) 判据 14 的接线：直连 → 中继，即使由 N3 触发，reason 也被细化成 relay-fallback。
    runAndCheck('src/engine/tests', 'test_engine_path_change',
        'EnginePathChangeLive.RelayFlipRefinesTheReasonToRelayFallback',
        '判据 14（接线级 relay-fallback）');

    // (c) 判据 15/16：classify 是纯函数 —— 同样输入同样输出（C3）。
    runAndCheck('src/engine/tests', 'test_engine_path_change',
        'PathChangeTracker.ClassifyIsPure', '判据 15（classify 纯函数）');
});

// ---------------------------------------------------------------------------
// 判据 17-23：边界
// ---------------------------------------------------------------------------
test('判据 17-23：边界；验证方式：静态（L4/失败词汇/接缝）+ 活引擎单测', () => {
    const statsCpp = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));

    // (a) 判据 17：无旧 pair、无新 pair → 不崩（机器级 + 活引擎）。
    runAndCheck('src/engine/tests', 'test_engine_path_change',
        'PathChangeTracker.ClassifyBothEmptyIsEmpty:'
        + 'EnginePathChangeLive.ManualRestartWithNoPairSelectedDoesNotCrash',
        '判据 17（无 pair 不崩）');

    // (b) 判据 18：新 pair 还没选出来 → new_pair 空（宿主看得到「还没选好」）。
    runAndCheck('src/engine/tests', 'test_engine_path_change',
        'EnginePathChangeLive.NewPairEmptyWhileStillGathering',
        '判据 18（new_pair 空）');

    // (c) 判据 19：路径变更不是失败 —— append_ice_recovery_stats（投影
    //     last_path_change）不得触碰 ice_failure_reason；失败词汇只由投影器算。
    const idx = statsCpp.indexOf('void NimRTCEngine::append_ice_recovery_stats');
    assert.ok(idx >= 0, 'engine_transport_stats.cpp 缺 append_ice_recovery_stats');
    const appendStats = statsCpp.slice(idx);
    assert.ok(!appendStats.includes('ice_failure_reason'),
        'append_ice_recovery_stats 不得触碰 ice_failure_reason（路径变更不是失败）');
    assert.match(statsCpp,
        /out\.ice_failure_reason\s*=\s*detail::TransportStatsProjector::ice_failure_reason\(fi\);/,
        'ice_failure_reason 只能由投影器按失败输入计算');

    // (d) 判据 20：新增的引擎文件不 include nimrtc/ice/ 的具体头（L4 接缝纪律）。
    for (const rel of ['src/engine/src/engine_path_change.hpp',
                       'src/engine/src/engine_path_change.cpp']) {
        const src = readText(path.join(ROOT, rel));
        assert.ok(!src.includes('#include <nimrtc/ice/'),
            `${rel} 不得 include nimrtc/ice/ 的具体头（L4）`);
    }

    // (e) 判据 21：回调在引擎 tick（驱动）线程上 —— 单测钉住线程 id。
    runAndCheck('src/engine/tests', 'test_engine_path_change',
        'EnginePathChangeLive.PeerRestartCallbackFiresOnDrivingThread',
        '判据 21（回调线程）');

    // (f) 判据 22：test_engine_*.cpp 一整套回归全绿（含本片新增的二进制）。
    const regression = [
        { exe: 'test_engine_device_policy',  note: 'D3/D4 设备策略与采集重连' },
        { exe: 'test_engine_audio_device',   note: 'D1 音频设备层' },
        { exe: 'test_engine_connectivity',   note: 'D6/N4 连通性诊断与 profile' },
        { exe: 'test_engine_ice_recovery',   note: 'N3（v1-ice-recovery）回归' },
        { exe: 'test_engine_turn_credentials', note: 'N7（v1-turn-credentials）回归' },
        { exe: 'test_engine_path_change',    note: '本片（v1-path-change-report）' },
    ];
    for (const r of regression) {
        const res = runGtest('src/engine/tests', r.exe, '*');
        assert.equal(res.status, 0,
            `${r.note}（${r.exe}）未通过（exit=${res.status}）：\n${res.out}`);
        assert.ok(res.out.includes('[==========]'),
            `${r.note}（${r.exe}）没有产出 gtest 运行输出：\n${res.out}`);
    }

    // (g) 判据 23：plugins::IICETransport 接口不变 —— 接缝回调类型原样。
    const pluginsIce = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp'));
    assert.match(pluginsIce,
        /using NIMRTC_API_STABLE OnIceRestart = std::function<void\(\)>;/,
        'plugins::OnIceRestart 必须保持无载荷（本片不改成带参数）');
});

// ---------------------------------------------------------------------------
// 判据 24-25：文档与契约
// ---------------------------------------------------------------------------
test('判据 24-25：文档与契约；验证方式：engine_api.md 静态 + 只读脚本跑绿', () => {
    // (a) 判据 24：engine_api.md 记录新回调与用法、两个新 Stats 字段、reason 的闭合词汇。
    const api = readText(path.join(ROOT, 'docs/api/engine_api.md'));
    for (const needle of ['set_on_peer_restart', 'PeerRestartReport',
                          'last_path_change_reason', 'last_path_change_new_pair']) {
        assert.ok(api.includes(needle), `engine_api.md 缺 ${needle}`);
    }
    for (const value of ['"local-address-changed"', '"peer-restart"',
                         '"relay-fallback"', '"manual"', '"ice-failed"']) {
        assert.ok(api.includes(value), `engine_api.md 缺 reason 取值 ${value}`);
    }

    // (b) 判据 25：公开头清单与实际公开面一致（只读脚本，不改它；本片不新增头文件，
    //     所以清单应当不变 —— 脚本跑绿即证明）。
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
    // 写集 = 规格「范围」（specs/slices/v1-path-change-report.md §范围）。
    const ALLOW = [
        'src/engine/src/engine_path_change.hpp',
        'src/engine/src/engine_path_change.cpp',
        'src/engine/src/engine.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/src/engine_session_state.hpp',
        'src/engine/src/engine_transport_stats.cpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/CMakeLists.txt',
        'src/engine/tests/CMakeLists.txt',
        'src/engine/tests/test_engine_path_change.cpp',
        'tests/acceptance/v1-path-change-report',
        'docs/adr/ADR-033-path-change-reporting.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
        'specs/slices/v1-path-change-report.md',
    ];

    const REQUIRED = [
        'src/engine/src/engine_path_change.hpp',
        'src/engine/src/engine_path_change.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/src/engine.cpp',
        'src/engine/src/engine_session_state.hpp',
        'src/engine/src/engine_transport_stats.cpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/CMakeLists.txt',
        'src/engine/tests/CMakeLists.txt',
        'src/engine/tests/test_engine_path_change.cpp',
        'tests/acceptance/v1-path-change-report/acceptance.test.cjs',
        'docs/adr/ADR-033-path-change-reporting.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
        'specs/slices/v1-path-change-report.md',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-path-change-report' });
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

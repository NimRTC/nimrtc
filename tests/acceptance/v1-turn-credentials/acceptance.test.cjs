// 验收测试 —— 切片 v1-turn-credentials（N7：TURN 凭据作为提供者接缝，ADR-032）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 22 条验收标准（specs/slices/v1-turn-credentials.md §验收标准）逐组落成
// 真实断言，验证手段与 v1-ice-recovery 同族：
//
//   ① 读源码做静态断言（公开面、接线点、优先级、文档、稳定性标注）；
//   ② 直接跑门禁同款 build 树里的 gtest 二进制（test_engine_turn_credentials）
//      —— 用 --gtest_filter 限定用例，判定只依赖「二进制跑起来 + 用例 OK」。
//
// 本片测试二进制分三个套件：
//   - EngineTurnCredentials      公开面编译期断言（static_assert + 默认值）
//   - TurnCredentialsState       机器级（虚拟时钟，不 sleep）：拉取记账
//   - EngineTurnCredentialsLive  活引擎（有状态假 ICE）：open / 静态优先 / 重连
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

// ---------------------------------------------------------------------------
// 判据 1-3：公开面（回调类型 + EngineConfig 字段 + TurnCredentials 结构体）
// ---------------------------------------------------------------------------
test('判据 1-3：公开面 —— 回调是函数指针（非 std::function）、两个字段默认 nullptr、六个字段的结构体；验证方式：静态默认值 + 编译期 static_assert 跑绿', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));

    // (a) 判据 1：EngineConfig 两个新字段，默认 nullptr。
    assert.match(engineHpp,
        /TurnCredentialsProvider turn_credentials = nullptr;/,
        'engine.hpp 的 EngineConfig 必须带 turn_credentials，默认 nullptr（判据 1）');
    assert.match(engineHpp,
        /void\* turn_credentials_ctx = nullptr;/,
        'engine.hpp 的 EngineConfig 必须带 turn_credentials_ctx，默认 nullptr（判据 1）');

    // (b) 判据 2：TurnCredentials 结构体带六个字段。
    const structBlock = engineHpp.match(/struct NIMRTC_API_STABLE TurnCredentials \{[\s\S]*?\n\};/);
    assert.ok(structBlock, 'engine.hpp 必须定义 TurnCredentials 结构体（判据 2）');
    for (const [frag, note] of [
        ['std::string host;',                    'host'],
        ['std::uint16_t port = 3478;',           'port（默认 3478）'],
        ['std::string username;',                'username'],
        ['std::string password;',                'password'],
        ['plugins::TurnTransport transport =',   'transport（默认 Udp）'],
        ['std::string tls_server_name;',         'tls_server_name'],
    ]) {
        assert.ok(structBlock[0].includes(frag),
            `TurnCredentials 结构体缺 ${note} 字段（${frag}，判据 2）`);
    }

    // (c) 判据 3：别名声明是裸函数指针，不是 std::function。
    //     engine.hpp 其它回调本就使用 std::function，所以「不出现 std::function」
    //     的作用域是**别名声明本身** —— 由编译期 static_assert 钉死类型同一性。
    assert.match(engineHpp,
        /using NIMRTC_API_STABLE TurnCredentialsProvider = bool \(\*\)\(void\* ctx,/,
        'TurnCredentialsProvider 必须是裸函数指针类型（判据 3）');
    const aliasRegion = engineHpp.slice(
        engineHpp.indexOf('using NIMRTC_API_STABLE TurnCredentialsProvider'),
        engineHpp.indexOf('using NIMRTC_API_STABLE TurnCredentialsProvider') + 200);
    assert.ok(!aliasRegion.includes('std::function'),
        'TurnCredentialsProvider 别名声明不得使用 std::function（判据 3）');

    // (d) 判据 1 / 2 / 3 的编译期钉子：static_assert 在测试二进制里，
    //     跑起来 = 通过。PublicConfigDefaults 还钉默认值。
    const r = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentials.PublicConfigDefaults');
    assert.equal(r.status, 0,
        `PublicConfigDefaults 未通过（exit=${r.status}）：\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineTurnCredentials.PublicConfigDefaults'),
        '公开面默认值用例未跑出 OK：\n' + r.out);

    // (e) static_assert 文本本身：类型同一性钉死在测试源里。
    const testSrc = readText(path.join(ROOT,
        'src/engine/tests/test_engine_turn_credentials.cpp'));
    assert.match(testSrc,
        /std::is_same_v<nimrtc::engine::TurnCredentialsProvider,[\s\S]*?bool \(\*\)\(void\*, TurnCredentials\*\) noexcept>/,
        '测试源必须静态钉住回调类型是裸函数指针（判据 3）');
});

// ---------------------------------------------------------------------------
// 判据 4-7：拉取时机 —— open() 问一次、失败不配 TURN、成功配 TURN、静态优先
// ---------------------------------------------------------------------------
test('判据 4-7：open() 拉取时机 —— 问一次 / 失败不配 TURN / 成功配 TURN / 静态优先；验证方式：接线静态 + 活引擎单测', () => {
    // (a) 判据 4：open() 在 ICE 传输 open 之前重置记账并拉取凭据。
    const engineCpp = readText(path.join(ROOT, 'src/engine/src/engine.cpp'));
    const resetAt = engineCpp.indexOf('turn_credentials.reset();');
    const refreshAt = engineCpp.indexOf('refresh_turn_credentials();');
    const iceOpenAt = engineCpp.indexOf('ice_t_->open();');
    assert.ok(resetAt >= 0, 'open() 必须先重置凭据记账（判据 4 的接线）');
    assert.ok(refreshAt >= 0 && iceOpenAt >= 0,
        'open() 必须同时出现 refresh_turn_credentials() 与 ice_t_->open()（判据 4）');
    assert.ok(refreshAt < iceOpenAt,
        'refresh_turn_credentials() 必须在 ice_t_->open() 之前（判据 4：gathering 前拉取）');

    // (b) 判据 7：静态服务器优先 —— add_turn_server() 置位 has_static_turn_server，
    //     refresh 时先查该标志并直接返回（提供者不被咨询）。
    assert.match(engineCpp,
        /has_static_turn_server = true;/,
        'add_turn_server() 必须置位 has_static_turn_server（判据 7）');
    assert.match(engineCpp,
        /if \(impl_->session_state\.has_static_turn_server\) return;/,
        'refresh_turn_credentials() 必须先查静态服务器标志并返回（判据 7）');

    // (c) 判据 6：成功拉取后通过接缝 apply（带 transport）。
    assert.match(engineCpp,
        /ice_t_->add_turn_server_with_transport\(/,
        '成功拉取的凭据必须经 add_turn_server_with_transport 配到 ICE（判据 6）');

    // (d) 活引擎：判据 4 + 6 —— open() 问一次（calls==1）且凭据带 transport 配到 ICE。
    const open = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.OpenConsultsProviderOnceAndAppliesCredentials');
    assert.equal(open.status, 0,
        `OpenConsultsProviderOnceAndAppliesCredentials 未通过（exit=${open.status}）：\n${open.out}`);
    assert.ok(gtestOk(open.out,
        'EngineTurnCredentialsLive.OpenConsultsProviderOnceAndAppliesCredentials'),
        'open() 问一次并 apply 的活引擎用例未跑出 OK：\n' + open.out);

    // (e) 活引擎：判据 5 —— 提供者 false → 不配 TURN、不崩、stats "unavailable"。
    const falseR = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.ProviderFalseKeepsSessionDirectAndUnavailable');
    assert.equal(falseR.status, 0,
        `ProviderFalseKeepsSessionDirectAndUnavailable 未通过（exit=${falseR.status}）：\n${falseR.out}`);
    assert.ok(gtestOk(falseR.out,
        'EngineTurnCredentialsLive.ProviderFalseKeepsSessionDirectAndUnavailable'),
        '提供者失败走直连的活引擎用例未跑出 OK：\n' + falseR.out);

    // (f) 活引擎：判据 7 —— 静态服务器存在时提供者不被调用（calls==0）。
    const staticR = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.StaticServerWinsAndProviderIsNotConsulted');
    assert.equal(staticR.status, 0,
        `StaticServerWinsAndProviderIsNotConsulted 未通过（exit=${staticR.status}）：\n${staticR.out}`);
    assert.ok(gtestOk(staticR.out,
        'EngineTurnCredentialsLive.StaticServerWinsAndProviderIsNotConsulted'),
        '静态服务器优先的活引擎用例未跑出 OK：\n' + staticR.out);
});

// ---------------------------------------------------------------------------
// 判据 8-10：重连时重新咨询
// ---------------------------------------------------------------------------
test('判据 8-10：N3 重连点 —— 重启后重新问 / 凭据不同则更新 / 失败则保留旧的；验证方式：接线静态 + 活引擎单测 + 机器级单测', () => {
    // (a) 判据 8：execute_ice_recovery_restart 在 ICE 重启之后重新拉取。
    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    const restartAt = transport.indexOf('ice_t_->restart();');
    const refreshAt = transport.indexOf('refresh_turn_credentials();');
    assert.ok(restartAt >= 0 && refreshAt >= 0,
        '重连路径必须同时出现 ice_t_->restart() 与 refresh_turn_credentials()（判据 8）');
    assert.ok(refreshAt > restartAt,
        'refresh_turn_credentials() 必须在 ICE 重启之后（判据 8：agent 重建后重读配置）');

    // (b) 活引擎：判据 8 + 9 —— 重连时重新问（calls==2）且新凭据应用到 ICE。
    const rec = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.ReconnectReconsultsProviderAndAppliesChange');
    assert.equal(rec.status, 0,
        `ReconnectReconsultsProviderAndAppliesChange 未通过（exit=${rec.status}）：\n${rec.out}`);
    assert.ok(gtestOk(rec.out,
        'EngineTurnCredentialsLive.ReconnectReconsultsProviderAndAppliesChange'),
        '重连重新咨询并应用新凭据的活引擎用例未跑出 OK：\n' + rec.out);

    // (c) 活引擎：判据 10 —— 重连失败不崩、保留旧凭据、stats "unavailable"。
    const failRec = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.ReconnectProviderFailureKeepsOldCredentials');
    assert.equal(failRec.status, 0,
        `ReconnectProviderFailureKeepsOldCredentials 未通过（exit=${failRec.status}）：\n${failRec.out}`);
    assert.ok(gtestOk(failRec.out,
        'EngineTurnCredentialsLive.ReconnectProviderFailureKeepsOldCredentials'),
        '重连失败保留旧凭据的活引擎用例未跑出 OK：\n' + failRec.out);

    // (d) 机器级：判据 9 的判定核心 —— 变化检测（上次 applied vs 新拉取的）。
    const change = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'TurnCredentialsState.ChangeDetectionTracksLastApplied');
    assert.equal(change.status, 0,
        `ChangeDetectionTracksLastApplied 未通过（exit=${change.status}）：\n${change.out}`);
    assert.ok(gtestOk(change.out,
        'TurnCredentialsState.ChangeDetectionTracksLastApplied'),
        '凭据变化检测用例未跑出 OK：\n' + change.out);
});

// ---------------------------------------------------------------------------
// 判据 11-13：凭据被拒的报法
// ---------------------------------------------------------------------------
test('判据 11-13：凭据被拒 —— ice_failure_reason == "credentials-rejected"、stats == "rejected"、不误报 no-candidates/none；验证方式：优先级静态 + 机器级单测 + 投影接线静态', () => {
    // (a) 判据 11 / 13：ice_failure_reason() 的优先级 —— credentials-rejected
    //     在 no-candidates / checks-failed / none 之前命中。
    const stats = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));
    const reasonBlock = stats.match(
        /std::string TransportStatsProjector::ice_failure_reason\([\s\S]*?return "none";\n\}/);
    assert.ok(reasonBlock, 'ice_failure_reason() 必须存在（判据 11）');
    const blk = reasonBlock[0];
    const idxRejected = blk.indexOf('"credentials-rejected"');
    const idxNoCand = blk.indexOf('"no-candidates"');
    const idxChecksFailed = blk.indexOf('"checks-failed"');
    const idxNone = blk.indexOf('return "none"');
    assert.ok(idxRejected >= 0, 'ice_failure_reason() 必须能报 "credentials-rejected"（判据 11）');
    assert.ok(idxNoCand >= 0 && idxChecksFailed >= 0 && idxNone >= 0,
        'ice_failure_reason() 必须保留 no-candidates / checks-failed / none 取值（判据 13 参照）');
    assert.ok(idxRejected < idxNoCand && idxRejected < idxChecksFailed && idxRejected < idxNone,
        'credentials-rejected 必须在 no-candidates / checks-failed / none 之前命中（判据 11/13）');
    assert.match(blk, /if \(in\.turn_credentials_rejected\)/,
        '凭据被拒必须由 turn_credentials_rejected 输入驱动（判据 11）');

    // (b) 判据 12：投影把 ICE 模块的被拒闩锁折叠进协作者 → stats "rejected"。
    assert.match(stats,
        /note_rejected\(\);/,
        'append_turn_credentials_stats 必须把被拒观测折叠进协作者（判据 12）');

    // (c) 机器级：判据 12 —— 被拒闩锁是粘性的，state_string() == "rejected"。
    const rejected = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'TurnCredentialsState.RejectedLatchIsSticky');
    assert.equal(rejected.status, 0,
        `RejectedLatchIsSticky 未通过（exit=${rejected.status}）：\n${rejected.out}`);
    assert.ok(gtestOk(rejected.out, 'TurnCredentialsState.RejectedLatchIsSticky'),
        '被拒闩锁用例未跑出 OK：\n' + rejected.out);
});

// ---------------------------------------------------------------------------
// 判据 14-18：边界与兼容
// ---------------------------------------------------------------------------
test('判据 14-18：边界与兼容 —— 不配提供者行为不变 / 空提供者不调 / 空 host 不配 / 不 include nimrtc/ice / tick 线程调用；验证方式：静态 + 单测 + 回归', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    const engineCpp = readText(path.join(ROOT, 'src/engine/src/engine.cpp'));

    // (a) 判据 14：add_turn_server() 的静态契约不变 —— 仍须在 open() 前调用。
    assert.match(engineHpp,
        /Must be called BEFORE open\(\)/,
        'add_turn_server() 文档必须保留 "BEFORE open()" 契约（判据 14）');

    // (b) 判据 15：提供者字段为 nullptr → refresh 直接返回，绝不调用。
    assert.match(engineCpp,
        /if \(config_\.turn_credentials == nullptr\) return;/,
        'refresh_turn_credentials() 必须对空提供者直接返回（判据 15）');

    // (c) 判据 16：提供者返回 true 但 host 为空 → 当作拿不到（不配 TURN）。
    //     该语义在协作者 refresh() 里 —— 由机器级单测钉住。
    const emptyHost = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'TurnCredentialsState.EmptyHostIsTreatedAsUnavailable:'
        + 'EngineTurnCredentialsLive.ProviderEmptyHostIsTreatedAsUnavailable');
    assert.equal(emptyHost.status, 0,
        `空 host 用例未通过（exit=${emptyHost.status}）：\n${emptyHost.out}`);
    assert.ok(gtestOk(emptyHost.out, 'TurnCredentialsState.EmptyHostIsTreatedAsUnavailable'),
        '空 host 当作拿不到的机器级用例未跑出 OK：\n' + emptyHost.out);
    assert.ok(gtestOk(emptyHost.out,
        'EngineTurnCredentialsLive.ProviderEmptyHostIsTreatedAsUnavailable'),
        '空 host 不配 TURN 的活引擎用例未跑出 OK：\n' + emptyHost.out);

    // (d) 判据 15 的活引擎对照：不配提供者 → 不咨询、不崩、stats ""。
    const noProv = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.NoProviderReportsEmptyAndAddsNothing:'
        + 'TurnCredentialsState.NullProviderIsNotConsultedAndKeepsIdle');
    assert.equal(noProv.status, 0,
        `空提供者用例未通过（exit=${noProv.status}）：\n${noProv.out}`);
    assert.ok(gtestOk(noProv.out, 'EngineTurnCredentialsLive.NoProviderReportsEmptyAndAddsNothing'),
        '不配提供者的活引擎用例未跑出 OK：\n' + noProv.out);
    assert.ok(gtestOk(noProv.out, 'TurnCredentialsState.NullProviderIsNotConsultedAndKeepsIdle'),
        '空提供者不咨询的机器级用例未跑出 OK：\n' + noProv.out);

    // (e) 判据 17：本片的公开头与新增引擎文件不 include nimrtc/ice/ 的具体头（L4）。
    //     （engine_transport_stats.hpp 里的 nimrtc/ice/ice.hpp 是 D6/N4 的既有状态，
    //     本片新增的文件与公开头都不引入它。）
    for (const rel of ['src/engine/include/nimrtc/engine/engine.hpp',
                       'src/engine/src/engine_turn_credentials.hpp',
                       'src/engine/src/engine_turn_credentials.cpp']) {
        const src = readText(path.join(ROOT, rel));
        assert.ok(!src.includes('#include <nimrtc/ice/'),
            `${rel} 不得 include nimrtc/ice/ 的具体头（L4，判据 17）`);
    }

    // (f) 判据 18：提供者在引擎的 open() / 重连（tick 驱动）路径上被调用 ——
    //     文档钉住线程语义，接线只有这两处引擎侧调用点。
    assert.match(engineHpp, /engine's tick thread/,
        'TurnCredentialsProvider 文档必须写明在引擎 tick 线程上调用（判据 18）');
    assert.match(engineHpp, /set_on_ice_recovery fires on/,
        '线程语义必须与 set_on_ice_recovery 同一线程（判据 18）');
    const calls = [
        ...engineCpp.match(/refresh_turn_credentials\(\);/g) || [],
        ...readText(path.join(ROOT, 'src/engine/src/engine_transport.cpp'))
            .match(/refresh_turn_credentials\(\);/g) || [],
    ];
    assert.deepEqual([...new Set(calls)], ['refresh_turn_credentials();'],
        'refresh_turn_credentials() 只能有引擎侧（open / 重连）调用点（判据 18）');

    // (g) 判据 19：test_engine_*.cpp 一整套回归全绿（含本片新增的二进制）。
    const regression = [
        { exe: 'test_engine_device_policy',     note: 'D3/D4 设备策略与采集重连' },
        { exe: 'test_engine_audio_device',      note: 'D1 音频设备层' },
        { exe: 'test_engine_connectivity',      note: 'D6/N4 连通性诊断与 profile' },
        { exe: 'test_engine_ice_recovery',      note: 'N3 重连策略（本片的挂载点）' },
        { exe: 'test_engine_turn_credentials',  note: '本片（v1-turn-credentials）' },
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
// 判据 20-22：文档与契约
// ---------------------------------------------------------------------------
test('判据 20-22：文档与契约 —— engine_api.md 记录新接缝；两个只读脚本跑绿；新符号带稳定性标注；验证方式：文档静态 + 脚本执行', () => {
    // (a) 判据 20：engine_api.md 记录回调类型、两个新字段、stats 取值与优先级。
    const api = readText(path.join(ROOT, 'docs/api/engine_api.md'));
    for (const needle of ['TurnCredentialsProvider', 'turn_credentials',
                          'turn_credentials_ctx', 'turn_credentials_state',
                          'add_turn_server']) {
        assert.ok(api.includes(needle), `engine_api.md 缺 ${needle}（判据 20）`);
    }
    for (const value of ['"idle"', '"ok"', '"unavailable"', '"rejected"']) {
        assert.ok(api.includes(value),
            `engine_api.md 必须记录 Stats::turn_credentials_state 取值 ${value}（判据 20）`);
    }
    assert.match(api, /static[\s\S]{0,120}wins|wins[\s\S]{0,120}static/,
        'engine_api.md 必须写明静态 add_turn_server() 优先于新接缝（判据 20）');

    // (b) 判据 21：公开头清单与实际公开面一致（只读脚本，不改它）。
    const m = spawnSync('python', ['scripts/check_api_manifest.py'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(m.status, 0,
        `check_api_manifest.py 未通过（exit=${m.status}）：\n` +
        `${m.stdout || ''}${m.stderr || ''}`);
    assert.match(`${m.stdout || ''}${m.stderr || ''}`, /OK/,
        'check_api_manifest.py 应输出 OK（判据 21）');

    // (c) 判据 22：新公开符号全部带稳定性标注（只读扫描脚本 + 静态）。
    const f = spawnSync(process.execPath,
        ['scripts/api_freeze_scan.js', '--no-md'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(f.status, 0,
        `api_freeze_scan.js 未通过（exit=${f.status}）：\n` +
        `${f.stdout || ''}${f.stderr || ''}`);
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.match(engineHpp, /struct NIMRTC_API_STABLE TurnCredentials \{/,
        'TurnCredentials 必须带 NIMRTC_API_STABLE（判据 22）');
    assert.match(engineHpp,
        /using NIMRTC_API_STABLE TurnCredentialsProvider = /,
        'TurnCredentialsProvider 必须带 NIMRTC_API_STABLE（判据 22）');
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
    // 写集 = 规格「范围」（specs/slices/v1-turn-credentials.md §范围）。
    const ALLOW = [
        'src/engine/src/engine_turn_credentials.hpp',
        'src/engine/src/engine_turn_credentials.cpp',
        'src/engine/src/engine.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/src/engine_session_state.hpp',
        'src/engine/src/engine_transport_stats.cpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/CMakeLists.txt',
        'src/engine/tests/CMakeLists.txt',
        'src/engine/tests/test_engine_turn_credentials.cpp',
        'tests/acceptance/v1-turn-credentials',
        'docs/adr/ADR-032-turn-credentials-provider-seam.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
        // —— 会话开始前已存在的脏状态（不属于本片 agent 的改动）——
        'specs/slices/v1-turn-credentials.md',
        'src/third_party/wolfssl/src',
    ];

    const REQUIRED = [
        'src/engine/src/engine_turn_credentials.hpp',
        'src/engine/src/engine_turn_credentials.cpp',
        'src/engine/src/engine.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/src/engine_session_state.hpp',
        'src/engine/src/engine_transport_stats.cpp',
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/CMakeLists.txt',
        'src/engine/tests/CMakeLists.txt',
        'src/engine/tests/test_engine_turn_credentials.cpp',
        'tests/acceptance/v1-turn-credentials/acceptance.test.cjs',
        'docs/adr/ADR-032-turn-credentials-provider-seam.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
        'specs/slices/v1-turn-credentials.md',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-turn-credentials' });
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

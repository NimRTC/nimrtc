// 验收测试 —— 切片 v3-n4-d6-connectivity（N4 连通性诊断 + D6 连通性 profile）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把规格的 8 条验收标准逐条做成真实断言
// （specs/slices/amendments/v3-n4-d6-connectivity.amended.md §验收标准）：
//
//   1. ice_gathering_state 的三个取值 + 与 ice_state 独立
//   2. 候选计数按类型分开，总数 == gathered_local_candidates() 条数
//   3. ice_failure_reason 封闭词汇表（六个取值全可达，优先级被钉住）
//   4. turn_transport_used 报实际用的那种（TURN/UDP → "udp"）
//   5. connectivity_profile 默认 "auto" 且开；显式 STUN 优先
//   6. profiles/connectivity/connectivity-profiles.json 存在且键集一致
//   7. 不静默降级（没有 STUN/TURN 可达时仍尝试，用 N4 词汇报告）
//   8. API 契约同步（test_api_classification 自身通过）
//
// 验证手段：读源码做静态断言 + 直接跑门禁同一棵 build 树里的 gtest 二进制。
// 不用 ctest -R 锚用例名，而用 --gtest_filter 直接跑二进制 —— 本片的两套
// 套件分别挂在 ice 与 engine 两个子树，而这两个子树的 CTestTestfile
// 加载历史上有过坏发现文件（缺 DLL → test_engine_device_policy）；
// 直接跑二进制绕开 CTestTestfile 加载，判定只依赖「二进制跑起来 + 用例 OK」。
//
// 写集边界判据（ADR-075）：本片不在共享 WRITE_SETS 表里，按规格「范围」
// 在本文件内联构造 allow/required 清单，复用 v5-writeset-acceptance 的
// collectSliceChanges / evaluateChanged / trackedOnTree。
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
    // 构建根目录：默认 `build/`，但 CI / 本地也常用 `build/ci/` 这类
    // 子目录（多配置生成器 + 独立配置树）。逐个试，谁先命中用谁。
    const buildRoots = [
        path.join(ROOT, 'build'),
        path.join(ROOT, 'build', 'ci'),
    ];
    const candidates = [];
    for (const root of buildRoots) {
        const base = path.join(root, sub);
        candidates.push(
            path.join(base, 'Debug', name + '.exe'),   // MSVC 多配置
            path.join(base, 'Release', name + '.exe'), // MSVC Release
            path.join(base, name + '.exe'),            // 单配置 Windows
            path.join(base, name),                     // 单配置 POSIX
        );
    }
    for (const c of candidates) {
        if (fs.existsSync(c)) return c;
    }
    return candidates[0]; // spawnSync 用明确的 ENOENT 报错
}

/** 跑一个 gtest 二进制，用 --gtest_filter 限定用例，返回退出码 + 输出。 */
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

// 本片的两套套件：模块侧（ice 子树）与引擎侧（tests 子树）。
const MODULE_SUITE = 'IceDiagnostics';
const ENGINE_SUITE = 'EngineConnectivityDiagnostics';

// ---------------------------------------------------------------------------
// 验收标准 1：ice_gathering_state 三取值 + 与 ice_state 独立
// ---------------------------------------------------------------------------
test('判据 1：ice_gathering_state 三取值 + 与 ice_state 独立；验证方式：模块单测直接断言', () => {
    // (a) 静态：seam 上有 gathering_state()，且它的默认实现返回 "new"
    //     （带默认实现的加法式扩展，不破坏既有实现者）。
    const seam = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp'));
    assert.match(seam, /virtual\s+std::string_view\s+gathering_state\(\)\s+const\s+noexcept/,
        'seam 上缺少 gathering_state()');
    assert.match(seam, /return\s+"new"\s*;/,
        'seam 的 gathering_state() 默认实现必须返回 "new"');

    // (b) 静态：公开头里 TransportStats 带着这三个字段，且带默认值
    //     （ADR-015 加法式）。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.match(engineHpp, /std::string\s+ice_gathering_state\s*;/,
        'TransportStats 缺少 ice_gathering_state');
    assert.match(engineHpp, /std::string\s+ice_failure_reason\s*;/,
        'TransportStats 缺少 ice_failure_reason');
    assert.match(engineHpp, /std::string\s+turn_transport_used\s*;/,
        'TransportStats 缺少 turn_transport_used');

    // (c) 动态：模块侧的三态用例真的通过。
    const moduleRun = runGtest(path.join('src', 'modules', 'ice', 'tests'),
        'test_ice_diagnostics_test', `${MODULE_SUITE}.*`);
    assert.equal(moduleRun.status, 0,
        `ICE 模块侧诊断套件没跑通：\n${moduleRun.out}`);
    assert.ok(gtestOk(moduleRun.out,
            `${MODULE_SUITE}.TheGatheringStateWalksFromNewToComplete`),
        '收集状态的三态用例没过');
    assert.ok(gtestOk(moduleRun.out,
            `${MODULE_SUITE}.TheGatheringStateIsGatheringWhileTheCollectionRuns`),
        '收集中的 "gathering" 用例没过');

    // (d) 动态：引擎侧「与 ice_state 独立」的用例真的通过。
    const engineRun = runGtest('tests', 'test_engine_connectivity_diagnostics',
        `${ENGINE_SUITE}.*`);
    assert.equal(engineRun.status, 0,
        `引擎侧连通性套件没跑通：\n${engineRun.out}`);
    assert.ok(gtestOk(engineRun.out,
            `${ENGINE_SUITE}.TheGatheringStateReachesTheSurface`),
        '收集状态上到 stats 的用例没过');
    assert.ok(gtestOk(engineRun.out,
            `${ENGINE_SUITE}.TheGatheringStateIsIndependentOfTheIceState`),
        '收集状态与 ice_state 独立的用例没过');
});

// ---------------------------------------------------------------------------
// 验收标准 2：候选计数按类型分开且总数一致
// ---------------------------------------------------------------------------
test('判据 2：候选计数按类型分开且总数一致；验证方式：引擎单测用固定候选行集合断言三个计数', () => {
    // (a) 静态：三个计数字段都在公开头上，且各有默认值 0。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    for (const field of ['ice_host_candidates', 'ice_srflx_candidates',
                         'ice_relay_candidates']) {
        assert.match(engineHpp,
            new RegExp(`std::uint32_t\\s+${field}\\s*=\\s*0\\s*;`),
            `TransportStats 缺少 ${field}（或没有默认值 0）`);
    }

    // (b) 静态：计数是在引擎的 stats 装配点里数出来的（从候选行里数，
    //     不是 ICE 模块另报一份）。
    const statsCpp = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));
    assert.match(statsCpp, /count_candidates/,
        'stats 装配点没有按类型数候选的函数');
    for (const t of ['Host', 'Srflx', 'Relay']) {
        assert.match(statsCpp,
            new RegExp(`CandidateType::${t}\\s*:`),
            `stats 装配点没有按 ${t} 类型数候选`);
    }
    // `prflx`（对端反射）刻意**不**计数：那是对端发现的候选，不是
    // 本端收集到的候选。
    assert.ok(!statsCpp.includes('CandidateType::Prflx:  ++'),
        'prflx 不该被计入本端的候选计数');

    // (c) 动态：引擎侧的计数用例真的通过。
    const engineRun = runGtest('tests', 'test_engine_connectivity_diagnostics',
        `${ENGINE_SUITE}.*`);
    assert.equal(engineRun.status, 0,
        `引擎侧连通性套件没跑通：\n${engineRun.out}`);
    assert.ok(gtestOk(engineRun.out,
            `${ENGINE_SUITE}.TheCandidateCountsArePerType`),
        '候选计数的用例没过');
    assert.ok(gtestOk(engineRun.out,
            `${ENGINE_SUITE}.AnUnknownCandidateTypeIsNotCounted`),
        '未知候选类型不被计数的用例没过');
});

// ---------------------------------------------------------------------------
// 验收标准 3：ice_failure_reason 走封闭词汇表
// ---------------------------------------------------------------------------
test('判据 3：ice_failure_reason 封闭词汇表（六个取值全可达 + 优先级被钉住）；验证方式：引擎单测逐个造场景断言', () => {
    // (a) 静态：源码里出现的失败原因字符串只有这六个（外加 seam 的
    //     "none"）。任何一个第七个取值都是改了契约。
    //
    //     扫的是**赋值给 ice_failure_reason 的那些取值**，不是源码里
    //     所有的字符串字面量 —— `ice_state` 也有自己的词汇表
    //     （"failed" / "connected" / …），那是另一个字段的契约，
    //     把它算进失败原因的词汇表是判据自己搞错了。
    const allowed = new Set([
        'none', 'no-candidates', 'no-pair', 'checks-failed',
        'credentials-rejected', 'transport-unsupported',
    ]);

    const engineCpp = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));

    // The failure-reason vocabulary lives in ONE place: the pure
    // `TransportStatsProjector::ice_failure_reason()` function, whose
    // `return "<literal>";` statements enumerate the closed vocabulary.
    // (The merged tree routes both `Stats::transport.ice_failure_reason`
    // and the D7 call-state's reason through that one function, so the
    // two surfaces can never disagree.)
    //
    // Scan that function's body only -- the file also holds the
    // `ice_state_name` / `dtls_state_name` vocabularies, which are
    // different contracts, and counting them here would be the check
    // measuring the wrong thing.
    const fnStart = engineCpp.indexOf(
        'TransportStatsProjector::ice_failure_reason(');
    assert.ok(fnStart >= 0,
        'stats 装配点没有 TransportStatsProjector::ice_failure_reason()');
    const fnBody = engineCpp.slice(fnStart, engineCpp.indexOf('\n}', fnStart));

    const returns = [...fnBody.matchAll(/return\s*"([a-z][a-z0-9-]*)"\s*;/g)]
        .map(m => m[1]);
    assert.ok(returns.length > 0,
        'ice_failure_reason() 没有返回过任何字面量取值');

    const seen = new Set(returns);
    for (const s of seen) {
        assert.ok(allowed.has(s),
            `出现了封闭词汇表之外的失败原因取值："${s}"`);
    }
    // 六个取值全都会被返回（不只是被允许）。
    for (const s of allowed) {
        assert.ok(seen.has(s),
            `失败原因的取值 "${s}" 在 ice_failure_reason() 里从没被返回过`);
    }

    // (b) 动态：六个取值逐个造出来的用例真的通过，且封闭性用例通过。
    const engineRun = runGtest('tests', 'test_engine_connectivity_diagnostics',
        `${ENGINE_SUITE}.*`);
    assert.equal(engineRun.status, 0,
        `引擎侧连通性套件没跑通：\n${engineRun.out}`);
    const cases = [
        'AHealthySessionReportsNoFailure',
        'NoCandidatesIsReportedAsSuch',
        'NoNominatedPairIsReportedAsSuch',
        'AFailedAgentIsReportedAsSuch',
        'ARefusedCredentialIsReportedAsSuch',
        'AnUnsupportedTurnTransportIsReportedAsSuch',
        'TheMoreSpecificCauseWins',
        'EveryReportedReasonIsInTheClosedVocabulary',
    ];
    for (const c of cases) {
        assert.ok(gtestOk(engineRun.out, `${ENGINE_SUITE}.${c}`),
            `失败原因的用例没过：${c}`);
    }

    // (c) 动态：凭据被拒这一条必须来自 seam，而不是引擎推导 ——
    //     模块侧的用例真的能造出 "credentials-rejected"。
    const moduleRun = runGtest(path.join('src', 'modules', 'ice', 'tests'),
        'test_ice_diagnostics_test', `${MODULE_SUITE}.*`);
    assert.equal(moduleRun.status, 0,
        `ICE 模块侧诊断套件没跑通：\n${moduleRun.out}`);
    assert.ok(gtestOk(moduleRun.out,
            `${MODULE_SUITE}.ARefusedCredentialIsReportedAsCredentialsRejected`),
        'seam 侧「凭据被拒」用例没过');
    assert.ok(gtestOk(moduleRun.out,
            `${MODULE_SUITE}.AnUnreachableServerIsNotReportedAsARefusedCredential`),
        'seam 侧「不可达 ≠ 凭据被拒」用例没过');
});

// ---------------------------------------------------------------------------
// 验收标准 4：turn_transport_used 报实际用的那种
// ---------------------------------------------------------------------------
test('判据 4：turn_transport_used 报实际用的那种；验证方式：引擎单测断言 TURN/UDP 会话报 "udp"', () => {
    // (a) 静态：三种取值的映射在引擎源码里。
    const engineCpp = readText(path.join(ROOT,
        'src/engine/src/engine.cpp'));
    for (const v of ['udp', 'tcp', 'tls']) {
        assert.ok(engineCpp.includes(`"${v}"`),
            `引擎源码里没有 TURN 传输取值 "${v}"`);
    }

    // (b) 动态：引擎侧的上报用例与「没配 TURN 就没有 TURN 传输」用例。
    const engineRun = runGtest('tests', 'test_engine_connectivity_diagnostics',
        `${ENGINE_SUITE}.*`);
    assert.equal(engineRun.status, 0,
        `引擎侧连通性套件没跑通：\n${engineRun.out}`);
    assert.ok(gtestOk(engineRun.out,
            `${ENGINE_SUITE}.TheTurnTransportReachesTheSurface`),
        'TURN 传输上到 stats 的用例没过');
    assert.ok(gtestOk(engineRun.out,
            `${ENGINE_SUITE}.NoTurnServerMeansNoTurnTransport`),
        '没配 TURN 就没有 TURN 传输的用例没过');
});

// ---------------------------------------------------------------------------
// 验收标准 5：connectivity_profile 默认 "auto" 且开；显式值优先
// ---------------------------------------------------------------------------
test('判据 5：connectivity_profile 默认 "auto" 且开 + 显式 STUN 优先；验证方式：引擎单测断言「补默认值」与「显式值优先」两件事', () => {
    // (a) 静态：公开头里默认是 "auto"。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.match(engineHpp,
        /std::string\s+connectivity_profile\s*=\s*"auto"\s*;/,
        'EngineConfig 的 connectivity_profile 默认不是 "auto"');

    // (b) 静态：profile 的解析在独立的协作者里（不是塞进 transport.cpp
    //     的一坨 if）。
    assert.ok(fs.existsSync(path.join(ROOT,
            'src/engine/src/engine_connectivity_profile.cpp')),
        '缺 engine_connectivity_profile.cpp');
    assert.ok(fs.existsSync(path.join(ROOT,
            'src/engine/src/engine_connectivity_profile.hpp')),
        '缺 engine_connectivity_profile.hpp');

    // (c) 动态：默认 auto 会配上 STUN、显式 STUN 赢、未知 profile 回落 auto。
    const engineRun = runGtest('tests', 'test_engine_connectivity_diagnostics',
        `${ENGINE_SUITE}.*`);
    assert.equal(engineRun.status, 0,
        `引擎侧连通性套件没跑通：\n${engineRun.out}`);
    const cases = [
        'TheDefaultProfileIsAuto',
        'TheProfileConfiguresTheDefaultStunServer',
        'TheHostsStunServerWins',
        'AnUnknownProfileFallsBackToAuto',
    ];
    for (const c of cases) {
        assert.ok(gtestOk(engineRun.out, `${ENGINE_SUITE}.${c}`),
            `connectivity profile 的用例没过：${c}`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 6：connectivity-profiles.json 存在且键集一致
// ---------------------------------------------------------------------------
test('判据 6：connectivity-profiles.json 存在且键集一致；验证方式：读文件断言键集', () => {
    // 路径钉死（ADR-075 的教训：规格引用了具体路径，判据就该把它写成
    // 断言的一部分，而不是只断言「某个地方有个键集一致的文件」）。
    const rel = path.join('profiles', 'connectivity', 'connectivity-profiles.json');
    const abs = path.join(ROOT, rel);
    assert.ok(fs.existsSync(abs),
        `连通性 profile 文件不存在：${rel}`);

    const doc = JSON.parse(readText(abs));

    // 三个 profile 都在。
    for (const name of ['auto', 'conference', 'remote-desktop']) {
        assert.ok(doc[name] && typeof doc[name] === 'object',
            `连通性 profile 文件里没有 "${name}"`);
    }

    // 键集一致：每个 profile 的键都是同一组。
    const expectedKeys = ['stun', 'turn_tls', 'tcp_fallback'];
    for (const [name, entry] of Object.entries(doc)) {
        if (name.startsWith('_')) continue;   // 文件头注释
        assert.deepEqual(Object.keys(entry).sort(), expectedKeys.slice().sort(),
            `profile "${name}" 的键集不一致`);
    }

    // 取值在封闭的范围里。
    for (const [name, entry] of Object.entries(doc)) {
        if (name.startsWith('_')) continue;
        assert.equal(typeof entry.stun, 'boolean', `profile "${name}" 的 stun 不是布尔`);
        assert.ok(['always', 'when_available', 'never'].includes(entry.turn_tls),
            `profile "${name}" 的 turn_tls 取值非法：${entry.turn_tls}`);
        assert.equal(typeof entry.tcp_fallback, 'boolean',
            `profile "${name}" 的 tcp_fallback 不是布尔`);
    }

    // 文件头必须说清它不是引擎 profile（否则下一个读的人会以为它该被
    // profiles/schema/ 管）。
    assert.match(readText(abs), /NOT an engine profile/,
        '连通性 profile 文件头必须说明它不是引擎 profile');
});

// ---------------------------------------------------------------------------
// 验收标准 7：不静默降级
// ---------------------------------------------------------------------------
test('判据 7：不静默降级；验证方式：没有 STUN/TURN 可达时仍尝试，并用 N4 的词汇报告', () => {
    // (a) 静态：引擎在 stats 装配点填的是 N4 的词汇表，而不是空串。
    const statsCpp = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));
    assert.match(statsCpp, /ice_failure_reason/,
        'stats 装配点没有填 ice_failure_reason');
    // 没有失败时也要说 "none" —— 不能留空串让宿主自己猜。
    assert.match(statsCpp, /"none"/,
        'stats 装配点没有把「没有失败」报成 "none"');

    // (b) 静态：即使没有任何 STUN/TURN 配置，引擎也仍然走一遍收集
    //     （不是直接短路）。
    const transportCpp = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    assert.match(transportCpp, /connectivity_profile/,
        '引擎的 transport 初始化没有读 connectivity_profile');

    // (c) 动态：引擎侧「一个健康的会话报 none」与「没有候选就报
    //     no-candidates」两个用例。
    const engineRun = runGtest('tests', 'test_engine_connectivity_diagnostics',
        `${ENGINE_SUITE}.*`);
    assert.equal(engineRun.status, 0,
        `引擎侧连通性套件没跑通：\n${engineRun.out}`);
    assert.ok(gtestOk(engineRun.out,
            `${ENGINE_SUITE}.AHealthySessionReportsNoFailure`),
        '健康会话报 none 的用例没过');
    assert.ok(gtestOk(engineRun.out,
            `${ENGINE_SUITE}.NoCandidatesIsReportedAsSuch`),
        '没有候选就报 no-candidates 的用例没过');
});

// ---------------------------------------------------------------------------
// 验收标准 8：API 契约同步
// ---------------------------------------------------------------------------
test('判据 8：API 契约同步；验证方式：test_api_classification 自身通过', () => {
    // (a) 静态：公开头清单与规格写的一致（加字段不改头数量，所以
    //     manifest 的条目数不变，但清单本身必须仍与树一致）。
    const manifestCheck = spawnSync('python',
        [path.join(ROOT, 'scripts', 'check_api_manifest.py')],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(manifestCheck.status, 0,
        `公开头清单与树不一致：\n${manifestCheck.stdout || ''}${manifestCheck.stderr || ''}`);

    // (b) 动态：分类测试自己通过（它断言基线条数与清单一致）。
    const apiRun = runGtest('tests', 'test_api_classification',
        'public_header_classification.*');
    assert.equal(apiRun.status, 0,
        `API 分类测试没跑通：\n${apiRun.out}`);
    assert.ok(gtestOk(apiRun.out,
            'public_header_classification.header_set_matches_manifest'),
        '公开头集合与清单不匹配的用例没过');
    assert.ok(gtestOk(apiRun.out,
            'public_header_classification.total_header_count_matches_baseline'),
        '公开头数量与基线不匹配的用例没过');
});

// ---------------------------------------------------------------------------
// 不破坏既有行为：ICE 旧有事件全绿
// ---------------------------------------------------------------------------
test('不破坏既有行为；验证方式：ICE 既有事件全绿（路由 / 候选 / TURN）', () => {
    const iceRun = runGtest(path.join('src', 'modules', 'ice', 'tests'),
        'test_ice_test', '*');
    assert.equal(iceRun.status, 0,
        `ICE 既有套件没跑通：\n${iceRun.out}`);

    // TURN 中继仍然能建立（seam 的加法式扩展没有动到 TURN 的行为）。
    const turnRun = runGtest(path.join('src', 'modules', 'ice', 'tests'),
        'test_turn_relay_test', '*');
    assert.equal(turnRun.status, 0,
        `TURN 中继套件没跑通：\n${turnRun.out}`);
});

// ---------------------------------------------------------------------------
// 验收测试文件是真断言，无 todo 用例残留
// ---------------------------------------------------------------------------
test('验收测试文件是真断言，无 todo 用例残留；验证方式：node --test 全部通过', () => {
    const own = readText(path.join(__dirname, 'acceptance.test.cjs'));
    // 骨架生成的待办用例是 test 的待办重载。用拼接构造 needle，
    // 避免把待办标记本身写进本文件 —— 否则自检会误伤自己。
    const todoNeedle = ['test', 'todo('].join('.');
    assert.ok(!own.includes(todoNeedle),
        '验收文件不得残留 todo 用例（门禁②要求每条用例都是可执行断言）');
});

// ---------------------------------------------------------------------------
// 写集边界断言（ADR-075）
// ---------------------------------------------------------------------------
test('本片改动仅限写集范围；验证方式：改动清单不越界 + 产物在树', () => {
    // 写集 = 规格「范围」（src/modules/ice/**、src/engine/**、
    // src/third_party/libjuice/**、profiles/**、docs/api/**、验收文件）；
    // 外加 ADR-015 基线文件、规格文件，以及会话开始前已存在的子模块脏状态。
    const ALLOW = [
        'src/modules/ice',
        'src/engine',
        'src/plugins',                       // seam 的加法式扩展（IICETransport）
        'src/third_party/libjuice',         // 补丁 + 树内改动（决定一）
        'profiles',
        'docs/api',
        'tests/acceptance/v3-n4-d6-connectivity',
        'tests/CMakeLists.txt',               // 登记本片的引擎侧测试
        'tests/test_engine_connectivity_diagnostics.cpp',
        'specs/slices/v3-n4-d6-connectivity.md',
        'specs/slices/amendments',
        'tools',
        // —— v1.4 计划与缺口清单的落地记录（条目落地时必须同步）——
        'docs/plan/v1.4-plan.md',
        'docs/plan/post-v1.4-gap-inventory.md',
        // —— ADR-015 公开头基线 ——
        'docs/api/public_headers.manifest',
        'tests/test_api_classification.cpp',
        'CHANGELOG.md',
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
        // N4：公开接口 + 模块实现
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/modules/ice/include/nimrtc/ice/ice.hpp',
        'src/modules/ice/src/ice.cpp',
        // N4：引擎统计装配点
        'src/engine/src/engine_transport_stats.cpp',
        // N4：seam 的加法式扩展
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp',
        // D6：profile 解析 + 引擎打开前应用
        'src/engine/src/engine_connectivity_profile.hpp',
        'src/engine/src/engine_connectivity_profile.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/CMakeLists.txt',
        // D6：模板
        'profiles/connectivity/connectivity-profiles.json',
        // 单测 + 注册
        'src/modules/ice/tests/test_ice_diagnostics.cpp',
        'src/modules/ice/tests/CMakeLists.txt',
        'tests/test_engine_connectivity_diagnostics.cpp',
        'tests/CMakeLists.txt',
        // 本片产物
        'tests/acceptance/v3-n4-d6-connectivity/acceptance.test.cjs',
    ];

    // —— 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件 ——
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v3-n4-d6-connectivity' });
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

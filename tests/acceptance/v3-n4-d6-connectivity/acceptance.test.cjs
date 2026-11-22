// 验收测试 —— 切片 v3-n4-d6-connectivity（N4 连通性诊断 + D6 连通性 profile）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把骨架里的 8 条 todo 替换为真实断言，逐条对应规格的 8 条验收标准
// （specs/slices/v3-n4-d6-connectivity.md §验收标准，行 146-187）：
//   1. ice_gathering_state 的三个取值 + 与 ice_state 独立
//   2. 候选计数按类型分开，总数 == gathered_local_candidates() 条数
//   3. ice_failure_reason 封闭词汇表（六个取值全可达，优先级被钉住）
//   4. turn_transport_used 报实际用的那种（TURN/UDP → "udp"）
//   5. connectivity_profile 默认 "auto" 且开；显式 STUN 优先
//   6. profiles/connectivity/connectivity-profiles.json 存在且键集一致
//   7. 不静默降级（没有 STUN/TURN 可达时仍尝试，用 N4 词汇报告）
//   8. API 契约同步（test_api_classification 自身通过）
//
// 验证手段：读源码做静态断言 + 直接跑门禁同款 build 树里的 gtest 二进制。
// 不用 ctest -R 锚用例名，而用 --gtest_filter 直接跑二进制 —— 本片的两条
// gtest 套件分别挂在 ice 与 engine 两个子树，而这两个子树的 CTestTestfile
// 加载历史上有过坏发现文件（缺 DLL 的 test_engine_device_policy）；直接跑
// 二进制绕开 CTestTestfile 加载，判定只依赖「二进制跑起来 + 用例 OK」。
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

// 两条本片套件：模块侧（ice 子树）与引擎侧（engine 子树）。
const MODULE_SUITE = 'ConnectivityDiagnostics';
const ENGINE_SUITE = 'EngineConnectivity';

// ---------------------------------------------------------------------------
// 验收标准 1：ice_gathering_state 取值正确 + 与 ice_state 独立
// ---------------------------------------------------------------------------
test('判据 1：ice_gathering_state 三取值 + 与 ice_state 独立；验证方式：模块单测直接断言', () => {
    // (a) 静态：公开头里有 GatheringState 三态枚举（独立于 IceState 的注释写明）。
    const iceHpp = readText(path.join(ROOT,
        'src/modules/ice/include/nimrtc/ice/ice.hpp'));
    assert.match(iceHpp, /enum class NIMRTC_API_STABLE GatheringState : std::uint8_t \{/,
        'ice.hpp 必须声明 GatheringState 枚举');
    assert.match(iceHpp, /independent of `IceState`/,
        'GatheringState 的注释必须写明它与 agent 状态机独立');

    // (b) 静态：引擎统计装配点把 gathering 状态投影成 "new"/"gathering"/"complete"。
    const stats = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));
    assert.match(stats, /ice_gathering_state_name\(/,
        '引擎必须把模块的 GatheringState 投影成字符串');

    // (c) 单测：三取值 + 独立（Connected 同时 Gathering）整条套件跑绿。
    const r = runGtest('src/modules/ice/tests',
        'test_connectivity_diagnostics_test', `${MODULE_SUITE}.*`);
    assert.equal(r.status, 0,
        `ConnectivityDiagnostics 未通过（exit=${r.status}）：\n${r.out}`);
    for (const gcase of [
        'ConnectivityDiagnostics.GatheringStateNewBeforeOpen',
        'ConnectivityDiagnostics.GatheringStateGatheringWhileStunPending',
        'ConnectivityDiagnostics.GatheringStateCompleteAfterDone',
        'ConnectivityDiagnostics.ConnectedWhileStillGathering',
    ]) {
        assert.ok(gtestOk(r.out, gcase),
            `判据 1 用例未跑出 OK：${gcase}\n${r.out}`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 2：候选计数按类型分开，总数 == gathered 条数
// ---------------------------------------------------------------------------
test('判据 2：候选计数按类型分开且总数一致；验证方式：固定候选行断言三个计数', () => {
    // (a) 静态：count_candidates 走模块自己的 parse_candidate 按类型计数。
    const stats = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));
    assert.match(stats, /ice::parse_candidate\(line\)/,
        '引擎必须用模块自己的候选语法按类型计数');
    assert.match(stats, /case ice::CandidateType::Host:\s+\+\+out\.host/,
        'host 计数必须存在');

    // (b) 静态：装配点把计数喂给三个公开字段。
    const assembly = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));
    assert.match(assembly, /out\.ice_host_candidates\s*=\s*counts\.host/,
        'engine.hpp 的 ice_host_candidates 必须由计数填充');

    // (c) 单测：固定候选行集合断言 host/srflx/relay 三个计数（含 TCP host）。
    const r = runGtest('src/engine/tests',
        'test_engine_connectivity', `${ENGINE_SUITE}.CountCandidates*`);
    assert.equal(r.status, 0,
        `CountCandidates 未通过（exit=${r.status}）：\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineConnectivity.CountCandidatesByType'),
        '按类型计数用例未跑出 OK：\n' + r.out);
    assert.ok(gtestOk(r.out, 'EngineConnectivity.CountCandidatesSkipsUnparseableLines'),
        '不可解析行跳过用例未跑出 OK：\n' + r.out);

    // (d) 单测：真实收集完成后，计数总和 == gathered_local_candidates() 条数
    //     （GatheringStateCompleteAfterDone 内部断言）。
    const m = runGtest('src/modules/ice/tests',
        'test_connectivity_diagnostics_test',
        'ConnectivityDiagnostics.GatheringStateCompleteAfterDone');
    assert.equal(m.status, 0,
        `GatheringStateCompleteAfterDone 未通过（exit=${m.status}）：\n${m.out}`);
    assert.ok(gtestOk(m.out, 'ConnectivityDiagnostics.GatheringStateCompleteAfterDone'),
        '真实收集的计数总和用例未跑出 OK：\n' + m.out);
});

// ---------------------------------------------------------------------------
// 验收标准 3：ice_failure_reason 封闭词汇表 + 优先级
// ---------------------------------------------------------------------------
test('判据 3：ice_failure_reason 封闭词汇表；验证方式：六个取值逐一造场景断言 + 优先级', () => {
    // (a) 静态：装配点把六值词汇写进公开字段。
    const stats = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));
    for (const w of [
        '"transport-unsupported"', '"credentials-rejected"', '"no-candidates"',
        '"no-pair"', '"checks-failed"', '"none"',
    ]) {
        assert.ok(stats.includes(w),
            `engine_transport_stats.cpp 缺词汇表取值 ${w}`);
    }

    // (b) 静态：优先级顺序写在实现里（first match reports first）。
    assert.match(stats, /The spec's priority order: first match reports first/,
        '实现必须注释写明先命中先报的优先级');

    // (c) 单测：六值全可达 + 封闭性（穷举输入组合断言输出仍在六值内）+ 优先级。
    const r = runGtest('src/engine/tests',
        'test_engine_connectivity', `${ENGINE_SUITE}.FailureReason*`);
    assert.equal(r.status, 0,
        `FailureReason 未通过（exit=${r.status}）：\n${r.out}`);
    for (const gcase of [
        'EngineConnectivity.FailureReasonTransportUnsupported',
        'EngineConnectivity.FailureReasonCredentialsRejected',
        'EngineConnectivity.FailureReasonNoCandidates',
        'EngineConnectivity.FailureReasonNoPairWhenCandidatesButNoPair',
        'EngineConnectivity.FailureReasonChecksFailed',
        'EngineConnectivity.FailureReasonNoneWhenHealthy',
        'EngineConnectivity.FailureReasonTransportUnsupportedBeatsNoCandidates',
        'EngineConnectivity.FailureReasonVocabularyIsClosed',
    ]) {
        assert.ok(gtestOk(r.out, gcase),
            `判据 3 用例未跑出 OK：${gcase}\n${r.out}`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 4：turn_transport_used 报实际用的那种
// ---------------------------------------------------------------------------
test('判据 4：turn_transport_used 报实际用的那种；验证方式：TURN/UDP 真实分配断言 "udp"', () => {
    // (a) 静态：模块把中继候选的来源传输记下来（按配置的 server 顺序归属）。
    const ice = readText(path.join(ROOT, 'src/modules/ice/src/ice.cpp'));
    assert.match(ice, /relay_candidates_seen_/,
        'ice.cpp 必须按中继候选到达顺序记录来源 TURN 服务器');
    assert.match(ice, /turn_transport_used_/,
        'ice.cpp 必须记住实际使用的 TURN 传输');

    // (b) 静态：引擎把模块的 TurnTransport 投影成 "udp"/"tcp"/"tls"。
    const stats = readText(path.join(ROOT,
        'src/engine/src/engine_transport_stats.cpp'));
    assert.match(stats, /case ice::TurnTransport::Udp: return "udp"/,
        '投影必须把 Udp 拼成 "udp"');
    assert.match(stats, /case ice::TurnTransport::Tls: return "tls"/,
        '投影必须把 Tls 拼成 "tls"');

    // (c) 单测：走 TURN/UDP 的会话报 TurnTransport::Udp（引擎面报 "udp"）。
    const r = runGtest('src/modules/ice/tests',
        'test_connectivity_diagnostics_test', `${MODULE_SUITE}.TurnTransportUsed*`);
    assert.equal(r.status, 0,
        `TurnTransportUsed 未通过（exit=${r.status}）：\n${r.out}`);
    assert.ok(gtestOk(r.out, 'ConnectivityDiagnostics.TurnTransportUsedUdpRelay'),
        'TURN/UDP 分配必须报 Udp：\n' + r.out);
    assert.ok(gtestOk(r.out, 'ConnectivityDiagnostics.TurnTransportUsedTcpRelay'),
        'TURN/TCP 分配必须报 Tcp（传输忠实归属的同一代码路径）：\n' + r.out);

    // (d) 单测：名字投影把 Tls 拼成 "tls"（树内没有可用的 TLS TURN 服务器，
    //     所以 "tls" 腿由纯投影覆盖，归属代码路径由 UDP/TCP 真实分配证明）。
    const e = runGtest('src/engine/tests',
        'test_engine_connectivity', `${ENGINE_SUITE}.TurnTransportNames`);
    assert.equal(e.status, 0,
        `TurnTransportNames 未通过（exit=${e.status}）：\n${e.out}`);
    assert.ok(gtestOk(e.out, 'EngineConnectivity.TurnTransportNames'),
        '"tls" 投影用例未跑出 OK：\n' + e.out);
});

// ---------------------------------------------------------------------------
// 验收标准 5：connectivity_profile 默认 "auto" 且开；显式 STUN 优先
// ---------------------------------------------------------------------------
test('判据 5：connectivity_profile 默认 "auto" 且开；验证方式：补默认值 + 显式值优先', () => {
    // (a) 静态：EngineConfig 公开键默认 "auto"。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.match(engineHpp,
        /std::string connectivity_profile = "auto";/,
        'engine.hpp 的 EngineConfig 必须有默认 "auto" 的 connectivity_profile');

    // (b) 静态：引擎打开前用 profile 补 STUN 默认值，且显式 host 优先。
    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    assert.match(transport, /detail::resolve_stun_server\(/,
        '引擎打开前必须调用 profile 的 STUN 解析');
    assert.match(transport, /ice_t_->set_stun_server\(stun\.host, stun\.port\);/,
        '解析出的 STUN 服务器必须配置到 ICE 传输上');
    // 注释跨了 // 行标：词与词之间要么是同行的单个空格，要么是
    // 「换行 + 行首 // 注释前缀」（文件是 CRLF，容忍可选的 \r）。
    const acrossLines = (frag) => frag.split(' ').join('(?:\r?\n\\s*//\\s*|\\s)?');
    assert.ok(
        new RegExp(acrossLines('An explicit host STUN server always wins')).test(transport)
            && new RegExp(acrossLines(
                'the profile carries policy defaults, not seam values.')).test(transport),
        '实现注释必须写明显式 host 优先于 profile（注释被改掉要报）');

    // (c) 单测：默认补默认 STUN + 显式值优先 + 默认配置值。
    //     注意：本树 vendored 的 gtest 不支持 {a,b} 花括号过滤，
    //     显式列出用例名（冒号分隔）。
    const r = runGtest('src/engine/tests',
        'test_engine_connectivity',
        `${ENGINE_SUITE}.AutoProfileDefaults:${ENGINE_SUITE}.StunFillsDefaultWhenHostSetNothing:`
        + `${ENGINE_SUITE}.ExplicitStunWinsOverTheProfile:${ENGINE_SUITE}.ExplicitStunWinsInEveryProfile:`
        + `${ENGINE_SUITE}.ExplicitHostWithZeroPortKeepsHost:${ENGINE_SUITE}.ConnectivityProfileDefaultsToAuto`);
    assert.equal(r.status, 0,
        `profile 相关用例未通过（exit=${r.status}）：\n${r.out}`);
    for (const gcase of [
        'EngineConnectivity.ConnectivityProfileDefaultsToAuto',
        'EngineConnectivity.AutoProfileDefaults',
        'EngineConnectivity.StunFillsDefaultWhenHostSetNothing',
        'EngineConnectivity.ExplicitStunWinsOverTheProfile',
        'EngineConnectivity.ExplicitStunWinsInEveryProfile',
        'EngineConnectivity.ExplicitHostWithZeroPortKeepsHost',
    ]) {
        assert.ok(gtestOk(r.out, gcase),
            `判据 5 用例未跑出 OK：${gcase}\n${r.out}`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 6：profiles/connectivity/connectivity-profiles.json 存在且键集一致
// ---------------------------------------------------------------------------
test('判据 6：connectivity-profiles.json 存在且键集一致；验证方式：读文件断言键集', () => {
    const jsonPath = path.join(ROOT, 'profiles/connectivity/connectivity-profiles.json');
    assert.ok(fs.existsSync(jsonPath),
        'profiles/connectivity/connectivity-profiles.json 必须存在');

    const parsed = JSON.parse(readText(jsonPath));

    // 三个 profile，键集完全一致。
    for (const name of ['auto', 'conference', 'remote-desktop']) {
        assert.ok(parsed[name], `缺少 profile: ${name}`);
        const keys = Object.keys(parsed[name]).sort();
        assert.deepEqual(keys, ['stun', 'tcp_fallback', 'turn_tls'],
            `profile ${name} 的键集必须恰为 stun/turn_tls/tcp_fallback`);
    }

    // 值符合规格（§接口约定 3）：remote-desktop 的 turn_tls 是 "always"，
    // 其余是 "when_available"。
    assert.equal(parsed.auto.stun, true);
    assert.equal(parsed.auto.turn_tls, 'when_available');
    assert.equal(parsed.auto.tcp_fallback, true);
    assert.equal(parsed['remote-desktop'].turn_tls, 'always');

    // 文件头注释必须写明它不属于 profiles/schema/ 管的引擎 profile 加载路径。
    const raw = readText(jsonPath);
    assert.match(raw, /NOT on the engine's profile loading path/,
        '文件头注释必须写明它不进引擎 profile 加载路径');
});

// ---------------------------------------------------------------------------
// 验收标准 7：不静默降级
// ---------------------------------------------------------------------------
test('判据 7：不静默降级；验证方式：无 STUN/TURN 可达时仍尝试并用 N4 词汇报告', () => {
    // (a) 静态：宿主没设任何 STUN/TURN 键时，profile 仍补默认 STUN
    //     —— 引擎不会因为「没配」而跳过收集。
    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    // 注释跨了 // 行标：词与词之间要么是同行的单个空格，要么是
    // 「换行 + 行首 // 注释前缀」（文件是 CRLF，容忍可选的 \r）。
    const acrossLines = (frag) => frag.split(' ').join('(?:\r?\n\\s*//\\s*|\\s)?');
    assert.ok(
        new RegExp(acrossLines(
            'An explicit host STUN server always wins')).test(transport)
            && new RegExp(acrossLines(
                'the profile carries policy defaults, not seam values.')).test(transport),
        '引擎必须说明显式值优先、profile 补默认值（不因没配而跳过）');

    // (b) 单测：对着一个「黑洞」STUN 服务器（绑定了但永不回应）打开，
    //     收集确实跑起来（Gathering 状态出现）——没有 STUN 可达不等于不尝试。
    //     同样显式列出用例名（vendored gtest 不支持花括号过滤）。
    const r = runGtest('src/modules/ice/tests',
        'test_connectivity_diagnostics_test',
        `${MODULE_SUITE}.GatheringStateGatheringWhileStunPending:`
        + `${MODULE_SUITE}.ConnectedWhileStillGathering`);
    assert.equal(r.status, 0,
        `黑洞 STUN 用例未通过（exit=${r.status}）：\n${r.out}`);
    assert.ok(gtestOk(r.out,
        'ConnectivityDiagnostics.GatheringStateGatheringWhileStunPending'),
        '无 STUN 可达时收集仍进行，未跑出 OK：\n' + r.out);
    assert.ok(gtestOk(r.out,
        'ConnectivityDiagnostics.ConnectedWhileStillGathering'),
        '无 STUN 可达时仍能连通、gathering 独立运行，未跑出 OK：\n' + r.out);

    // (c) 单测：引擎面的失败理由词汇在「收集结束且零候选」时报
    //     "no-candidates" —— 如实报告，而不是假装无事。
    const e = runGtest('src/engine/tests',
        'test_engine_connectivity', `${ENGINE_SUITE}.FailureReasonNoCandidates`);
    assert.equal(e.status, 0,
        `FailureReasonNoCandidates 未通过（exit=${e.status}）：\n${e.out}`);
    assert.ok(gtestOk(e.out, 'EngineConnectivity.FailureReasonNoCandidates'),
        '"no-candidates" 用例未跑出 OK：\n' + e.out);
});

// ---------------------------------------------------------------------------
// 验收标准 8：API 契约同步
// ---------------------------------------------------------------------------
test('判据 8：API 契约同步；验证方式：test_api_classification 自身通过', () => {
    // (a) 静态：公开头加了本片的公开字段。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    for (const field of [
        'ice_gathering_state', 'ice_host_candidates',
        'ice_srflx_candidates', 'ice_relay_candidates',
        'ice_failure_reason', 'turn_transport_used',
    ]) {
        assert.ok(engineHpp.includes(field),
            `engine.hpp 的 TransportStats 缺公开字段 ${field}`);
    }
    assert.ok(engineHpp.includes('connectivity_profile'),
        'engine.hpp 的 EngineConfig 缺 connectivity_profile 键');

    // (b) 加字段不改头文件数量：manifest count 保持 198（ADR-015 加法式，
    //     见 v1-device-policy 先例——新增头才 bump）。kBaselineHeaderCount
    //     与 manifest 的 count 由 test_api_classification 自身核对。
    const manifest = readText(path.join(ROOT,
        'docs/api/public_headers.manifest'));
    assert.match(manifest, /count: 198/,
        'public_headers.manifest 的 count 必须保持 198（本片只加字段不加头）');

    // (c) 自身通过：直接跑 test_api_classification 二进制。
    const r = runGtest('tests', 'test_api_classification', '*');
    assert.equal(r.status, 0,
        `test_api_classification 未通过（exit=${r.status}）：\n${r.out}`);
    assert.ok(
        gtestOk(r.out, 'public_header_classification.total_header_count_matches_baseline')
            || r.out.includes('[==========]'),
        'test_api_classification 没有产出 gtest 运行输出：\n' + r.out);
});

// ---------------------------------------------------------------------------
// 不破坏既有行为（ICE 模块回归）
// ---------------------------------------------------------------------------
test('不破坏既有行为；验证方式：ICE 既有套件全绿（选路 / 候选 / TURN）', () => {
    // 本片在 ICE 传输上加了诊断面（gathering 状态、turn 归属、凭据拒绝闩锁），
    // 但选路、候选 SDP、TURN 分配这些既有行为不能变。按真实套件名分组跑：
    const regression = [
        // test_ice_test：收集 / 选路 / 候选 SDP / 双代理回环
        { exe: 'test_ice_test',
          filter: 'IceTransport.*:CandidateSdp.*:IceTransportLoopback.*',
          note: 'ICE 传输（打开/收集/选路）与候选 SDP' },
        // test_turn_relay_test：TURN/UDP 与 TURN/TCP 真实分配
        { exe: 'test_turn_relay_test',
          filter: 'TurnRelay.*:TurnTcp.*',
          note: 'TURN 中继（UDP/TCP 分配）' },
        // test_turn_tls_test：TLS 表面（负向对照）
        { exe: 'test_turn_tls_test',
          filter: 'TurnTls.*',
          note: 'TURN/TLS 表面' },
    ];
    for (const r of regression) {
        const res = runGtest('src/modules/ice/tests', r.exe, r.filter);
        assert.equal(res.status, 0,
            `${r.note}（${r.filter}）未通过（exit=${res.status}）：\n${res.out}`);
        assert.ok(res.out.includes('[==========]'),
            `${r.note} 没有产出 gtest 运行输出：\n${res.out}`);
    }
});

// ---------------------------------------------------------------------------
// 验收测试文件是真实断言（无 todo 用例残留）
// ---------------------------------------------------------------------------
test('验收测试文件是真实断言，无 todo 用例残留；验证方式：node --test 全部通过', () => {
    const own = readText(__filename);
    // 骨架生成的待办用例是 test 的待办重载。用拼接构造 needle，
    // 避免把待办标记本身写进本文件 —— 否则自检会误伤自己。
    const todoNeedle = ['test', 'todo('].join('.');
    assert.ok(!own.includes(todoNeedle),
        '验收文件不得残留 todo 用例（门禁②要求每条用例都是可执行断言）');
});

// ---------------------------------------------------------------------------
// 写集边界断言（ADR-075）
// ---------------------------------------------------------------------------
test('本片改动仅限写集范围；验证方式：改动清单不越界 + 产物在树上', () => {
    // 写集 = 规格「范围」（src/modules/ice/**、src/modules/video_stats/**、
    // src/engine/**、profiles/**、acceptance.test.cjs）；外加 ADR-015 基线
    // 文件、规格文件，以及会话开始前已存在的子模块脏状态。
    const ALLOW = [
        'src/modules/ice',
        'src/modules/video_stats',
        'src/engine',
        'profiles',
        'tests/acceptance/v3-n4-d6-connectivity',
        'specs/slices/v3-n4-d6-connectivity.md',
        // —— ADR-015 公开头基线（加字段不改头数量，但按 v1-device-policy /
        //    v2-ice 先例把这些文件列入写集，出现改动时门禁可审）——
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
        'src/engine/src/engine_transport_stats.hpp',
        'src/engine/src/engine_transport_stats.cpp',
        // D6：profile 解析 + 引擎打开前应用
        'src/engine/src/engine_connectivity_profile.hpp',
        'src/engine/src/engine_connectivity_profile.cpp',
        'src/engine/src/engine_transport.cpp',
        'src/engine/CMakeLists.txt',
        // D6：模板
        'profiles/connectivity/connectivity-profiles.json',
        // 单测 + 注册
        'src/modules/ice/tests/test_connectivity_diagnostics.cpp',
        'src/modules/ice/tests/CMakeLists.txt',
        'src/engine/tests/test_engine_connectivity.cpp',
        'src/engine/tests/CMakeLists.txt',
        // 本片产物
        'tests/acceptance/v3-n4-d6-connectivity/acceptance.test.cjs',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v3-n4-d6-connectivity' });
    // ② 产物检查：产物必须在树上（git ls-files），与合并策略无关。
    //    本片签收前产物是未跟踪的 —— 用「在树上 ∪ 在本片改动清单里」双态判据：
    //    干净已提交树（含 squash 合并后）由 onTree 覆盖，进行中 worktree 由
    //    changed 覆盖，两种状态判定一致。
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

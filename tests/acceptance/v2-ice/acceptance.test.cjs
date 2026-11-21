// 验收测试 —— 切片 v2-ice（N2：ICE-over-TCP，RFC 6544）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把骨架里的 9 条 todo 替换为真实断言，逐条对应规格的 9 条验收标准
// （specs/slices/v2-ice.md §验收标准，行 156-198）：
//   1. TCP 候选的收集与序列化（local_description / gathered_local_candidates 带 tcptype）
//   2. tcptype 的往返（parse ⇄ to_sdp，active / passive）
//   3. tcptype 不被误认成 typ（type 保持 Host）
//   4. 优先级排序（active > passive；foundation 与 UDP 区分；优先级不冲突）
//   5. 主验收 —— TCP-only 防火墙形状下选出 protocol=="tcp" 的候选对
//   6. 负控制 —— UDP 可用时选中对 protocol=="udp"
//   7. N2-1 —— UDP 能提名时 TCP 候选根本没有被收集
//   8. SDP writer 发得出完整 tcptype 行（tcptype 在 typ host 之后）
//   9. 确定性（环回、无外部网络 / Docker / NAT / 时序依赖）
//
// 验证手段：读取源码做静态断言 + 调用门禁同款 build 树里的 ctest 用例
// （gtest_discover_tests 把用例名直接注册成 ctest 测试名，如
// `TcpCandidate.ParseRoundTripActiveAndPassive`，所以 -R 直接锚用例名）。
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
// ctest 只跑本片所在的 ice 测试子树：根 build 树的 CTestTestfile 里挂着
// 别的切片（engine 等）的坏发现文件（缺 DLL 的 test_engine_device_policy），
// ctest 在加载阶段就中止。ice 子树自洽（64 个用例），本片全部断言都在这棵子树里。
const BUILD = path.join(ROOT, 'build', 'src', 'modules', 'ice', 'tests');

// 写集边界判据的共享实现（v5-writeset-acceptance/）。
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, 'utf8'); }

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

// 主验收集成测试：TCP-only 防火墙（41s：39.5s PAC 定时器 + 驱动检查）。
// 规格标准 1/5/6/7 都锚在同一集成用例上，每次各跑一遍以保持用例独立可验证。
const LOOPBACK_SUITE = '^IceTcpLoopback\\.';
const TCP_CAND_SUITE  = '^TcpCandidate\\.';

// ---------------------------------------------------------------------------
// 验收标准 1：TCP 候选的收集与序列化
// ---------------------------------------------------------------------------
test('TCP 候选的收集与序列化；验证方式：UDP 失败后 TCP 候选出现在 local_description() 与 gathered_local_candidates()', () => {
    const ice = readText(path.join(ROOT, 'src/modules/ice/src/ice.cpp'));

    // (a) 静态：两条输出面都带上 TCP 候选 —— local_description() 以
    //     "a=" 前缀把 tcp_local_candidates_ 追加进 SDP…
    assert.match(ice,
        /for \(const auto& line : impl_->tcp_local_candidates_\) \{\s*out \+= "a=";\s*out \+= line;/,
        'local_description() 必须把 TCP 候选行追加进 SDP（"a=" + 候选体）');
    // …gathered_local_candidates() 把 TCP 候选并进本地候选集合。
    assert.match(ice,
        /out\.insert\(out\.end\(\), impl_->tcp_local_candidates_\.begin\(\),\s*impl_->tcp_local_candidates_\.end\(\)\);/,
        'gathered_local_candidates() 必须把 TCP 候选并进本地集合');

    // (b) 集成：环回测试里 UDP 失败后两个输出面确实出现 TCP 候选
    //     （断言在 IceTcpLoopback.TcpOnlyShapeSelectsTcpPair 内部）。
    const t = runCtest(LOOPBACK_SUITE);
    assert.equal(t.status, 0,
        `IceTcpLoopback 未通过（exit=${t.status}）：\n${t.out}`);
    assert.ok(gtestOk(t.out, 'IceTcpLoopback.TcpOnlyShapeSelectsTcpPair'),
        'TCP-only 主用例未跑出 OK：\n' + t.out);
});

// ---------------------------------------------------------------------------
// 验收标准 2：tcptype 的往返（parse ⇄ to_sdp）
// ---------------------------------------------------------------------------
test('tcptype 的往返；验证方式：parse_candidate(to_sdp(c)) 后 tcptype 与 transport 都不丢（active/passive）', () => {
    // (a) 静态：parse_candidate 认 tcptype 令牌，to_sdp 在有 tcptype 时序列化它。
    const ice = readText(path.join(ROOT, 'src/modules/ice/src/ice.cpp'));
    assert.match(ice, /if \(iss >> tok && tok == "tcptype"\) \{\s*iss >> c\.tcptype;\s*iss >> tok;/,
        'parse_candidate 必须解析 tcptype 令牌，并让 tok 继续指向其后令牌');
    assert.match(ice, /if \(!tcptype\.empty\(\)\) \{\s*oss << " tcptype " << tcptype;\s*\}/,
        'to_sdp 必须在有 tcptype 时序列化它');

    // (b) 单元：对 active 和 passive 各做一次往返断言。
    const u = runCtest(TCP_CAND_SUITE);
    assert.equal(u.status, 0,
        `TcpCandidate 未通过（exit=${u.status}）：\n${u.out}`);
    assert.ok(gtestOk(u.out, 'TcpCandidate.ParseRoundTripActiveAndPassive'),
        'tcptype 往返用例未跑出 OK：\n' + u.out);
});

// ---------------------------------------------------------------------------
// 验收标准 3：tcptype 不被误认成 typ
// ---------------------------------------------------------------------------
test('tcptype 不被误认成 typ；验证方式：解析 "typ host tcptype passive" 得到 type==Host 且 tcptype=="passive"', () => {
    // (a) 静态：tcptype 在 "typ <type>" 之后解析，且注释写明不能与 typ 混淆。
    const ice = readText(path.join(ROOT, 'src/modules/ice/src/ice.cpp'));
    assert.match(ice,
        /never confuse it with "typ" itself/,
        'parse_candidate 的注释必须写明 tcptype 不能与 typ 混淆');
    assert.match(ice,
        /Optional "tcptype active\|passive\|so" \(RFC 6544 §4\.1\)/,
        'tcptype 的解析注释必须写明 RFC 6544 §4.1 语义');

    // (b) 单元：字面行解析后 type 仍是 Host，tcptype 单独读出。
    const u = runCtest(TCP_CAND_SUITE);
    assert.equal(u.status, 0,
        `TcpCandidate 未通过（exit=${u.status}）：\n${u.out}`);
    assert.ok(gtestOk(u.out, 'TcpCandidate.TcptypeIsNotMistakenForTyp'),
        'tcptype 不被误认成 typ 用例未跑出 OK：\n' + u.out);
    assert.ok(gtestOk(u.out, 'TcpCandidate.PassiveHostCandidateFromGatherShape'),
        '收集形状的 passive host 候选往返未跑出 OK：\n' + u.out);
});

// ---------------------------------------------------------------------------
// 验收标准 4：优先级排序（RFC 6544 §4.3）
// ---------------------------------------------------------------------------
test('优先级排序；验证方式：active > passive；TCP foundation 与 UDP 区分；TCP 优先级与 UDP 不冲突', () => {
    // (a) 静态：TCP 候选的 foundation 用 "tcp-" 前缀命名空间与 UDP 区分；
    //     优先级按 RFC 6544 §4.3 的 110(active)/100(passive) 计算。
    const tcpHpp = readText(path.join(ROOT,
        'src/modules/ice/include/nimrtc/ice/tcp_candidate.hpp'));
    assert.match(tcpHpp, /foundation is namespaced \("tcp-1", "tcp-2", …\)/,
        'TCP 候选 foundation 必须落在 "tcp-" 命名空间（与 libjuice 的数字 foundation 区分）');
    assert.match(tcpHpp, /type_pref = \(tcptype == TcpCandidateRole::Active\) \? 110u : 100u;/,
        'tcp_candidate.hpp 必须实现 RFC 6544 §4.3 的 110/100 类型偏好');

    // (b) 单元：三条优先级/区分断言全绿。
    const u = runCtest(TCP_CAND_SUITE);
    assert.equal(u.status, 0,
        `TcpCandidate 未通过（exit=${u.status}）：\n${u.out}`);
    for (const gcase of [
        'TcpCandidate.ActivePriorityExceedsPassive',
        'TcpCandidate.PriorityDisjointFromLibjuiceUdp',
        'TcpCandidate.FoundationDistinctFromUdp',
    ]) {
        assert.ok(gtestOk(u.out, gcase),
            `优先级用例未跑出 OK：${gcase}\n${u.out}`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 5：主验收 —— TCP-only 防火墙形状下选出 TCP 候选对
// ---------------------------------------------------------------------------
test('主验收 —— TCP-only 防火墙下选出 TCP 候选对；验证方式：两个 peer 选中对 protocol=="tcp"', () => {
    // (a) 静态：集成测试里被选中对的 protocol 必须读作 "tcp"（经 seam）。
    const it = readText(path.join(ROOT, 'src/modules/ice/tests/test_ice_tcp.cpp'));
    assert.match(it,
        /pa->local\.protocol[\s\S]{0,40}"tcp"/,
        '集成测试必须断言选中对的 protocol == "tcp"');
    assert.match(it,
        /selected_pair\(\)/,
        '集成测试必须经 plugins::IICETransport::selected_pair() 读协议');

    // (b) 静态：seam 的 tcptype 字段存在（acceptance 标准 4 走 seam 读）。
    const seam = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp'));
    assert.match(seam, /std::string\s+tcptype;/,
        'plugins::IceCandidate 缺 tcptype 字段（seam 未加）');

    // (c) 集成：两个 peer 各自选中一对 TCP 候选。
    const t = runCtest(LOOPBACK_SUITE);
    assert.equal(t.status, 0,
        `IceTcpLoopback 未通过（exit=${t.status}）：\n${t.out}`);
    assert.ok(gtestOk(t.out, 'IceTcpLoopback.TcpOnlyShapeSelectsTcpPair'),
        'TCP-only 主用例未跑出 OK（两 peer 选 TCP 对）：\n' + t.out);
});

// ---------------------------------------------------------------------------
// 验收标准 6：负控制 —— UDP 可用时不得选 TCP
// ---------------------------------------------------------------------------
test('负控制 —— UDP 可用时选中对 protocol=="udp"；验证方式：UDP 正常路径选 UDP 对、tcptype 为空', () => {
    // (a) 静态：UDP 正常路径的断言必须落在同一个集成二进制里。
    const it = readText(path.join(ROOT, 'src/modules/ice/tests/test_ice_tcp.cpp'));
    assert.match(it,
        /pair->local\.protocol,\s*"udp"/,
        '负控制必须断言 UDP 正常时选中对 protocol == "udp"');
    assert.match(it,
        /pair->local\.tcptype\.empty\(\)/,
        '负控制必须断言 UDP 对不带 tcptype');

    // (b) 集成：同一套环回测试，UDP 可用那一路选出 protocol=="udp"。
    const t = runCtest(LOOPBACK_SUITE);
    assert.equal(t.status, 0,
        `IceTcpLoopback 未通过（exit=${t.status}）：\n${t.out}`);
    assert.ok(gtestOk(t.out, 'IceTcpLoopback.UdpWorksSelectsUdpAndGathersNoTcpCandidates'),
        'UDP 负控制用例未跑出 OK：\n' + t.out);
});

// ---------------------------------------------------------------------------
// 验收标准 7：N2-1 —— TCP 只在 UDP 失败之后才尝试
// ---------------------------------------------------------------------------
test('N2-1 —— UDP 能提名时 TCP 候选根本没有被收集；验证方式：UDP 正常路径零 TCP 候选', () => {
    // (a) 静态：进入 TCP 路径的唯一判据是 ICE agent 报 Failed（规格 §接口约定 5）。
    const ice = readText(path.join(ROOT, 'src/modules/ice/src/ice.cpp'));
    assert.match(ice, /if \(tcp_driver_ \|\| state != plugins::IceState::Failed\) return;/,
        'start_tcp_fallback 必须以 Failed 状态为进入判据');
    assert.match(ice, /state != plugins::IceState::Failed/,
        'TCP fallback 不得在 UDP 还能提名时启动（N2-1）');

    // (b) 静态 + 集成：UDP 正常路径收集到的候选里没有任何 TCP 传输的候选
    //     （断言在 UdpWorks... 用例内部，含 gathered_local_candidates 检查）。
    const it = readText(path.join(ROOT, 'src/modules/ice/tests/test_ice_tcp.cpp'));
    assert.match(it,
        /has_tcp_candidate\(a\.gathered_local_candidates\(\)\)/,
        '集成测试必须断言 UDP 正常时 gathered_local_candidates 无 TCP 候选');

    const t = runCtest(LOOPBACK_SUITE);
    assert.equal(t.status, 0,
        `IceTcpLoopback 未通过（exit=${t.status}）：\n${t.out}`);
    assert.ok(gtestOk(t.out, 'IceTcpLoopback.UdpWorksSelectsUdpAndGathersNoTcpCandidates'),
        'N2-1 负控制用例未跑出 OK：\n' + t.out);
});

// ---------------------------------------------------------------------------
// 验收标准 8：SDP writer 真的发得出 tcptype
// ---------------------------------------------------------------------------
test('SDP writer 发得出完整 tcptype 行；验证方式：逐字段匹配，tcptype 在 "typ host" 之后', () => {
    // (a) 静态：munger 不过滤候选体（spec §行 36-38：munger 从不过滤候选体，
    //     缺口在产出候选体的人那里，本片不改 munger 生产代码）——
    //     候选体原样拼接，tcptype 令牌不会被剥掉。
    const munger = readText(path.join(ROOT, 'src/modules/sdp/src/munger.cpp'));
    assert.match(munger, /out \+= "a=candidate:";[\s\S]*?out \+= cand;/,
        'munger 必须原样拼接候选体（a=candidate: + 候选体），不得过滤 tcptype');

    // (b) 单元：to_sdp() 逐字段匹配完整行，且 tcptype 在 "typ host" 之后。
    const u = runCtest(TCP_CAND_SUITE);
    assert.equal(u.status, 0,
        `TcpCandidate 未通过（exit=${u.status}）：\n${u.out}`);
    assert.ok(gtestOk(u.out, 'TcpCandidate.SdpIsFullLineWithTcptypeAfterTypHost'),
        '完整 tcptype 候选行用例未跑出 OK：\n' + u.out);

    // (c) 静态：用例内断言 tcptype 位置在 typ host 之后（防止字段顺序回归）。
    const tcpTest = readText(path.join(ROOT,
        'src/modules/ice/tests/test_tcp_candidate.cpp'));
    assert.match(tcpTest, /EXPECT_GT\(tcp_pos, typ_pos\)/,
        '单测必须断言 tcptype 出现在 "typ host" 之后');
});

// ---------------------------------------------------------------------------
// 验收标准 9：确定性
// ---------------------------------------------------------------------------
test('确定性；验证方式：环回、无外部网络 / Docker / NAT / 时序依赖，固定超时上界', () => {
    const it = readText(path.join(ROOT, 'src/modules/ice/tests/test_ice_tcp.cpp'));

    // (a) 环回：两个 transport 都绑 127.0.0.1，不依赖宿主网络配置。
    assert.match(it, /bind_address\s*=\s*"127\.0\.0\.1"/,
        '集成测试必须绑环回地址');
    // (b) 无外部依赖：不碰 Docker、外部 NAT 探测服务器、公网地址。
    assert.ok(!it.includes('docker'),
        '集成测试不得依赖 Docker daemon');
    assert.ok(!/\b(8\.8\.8\.8|1\.1\.1\.1|223\.5\.5\.5|114\.114\.114\.114)\b/.test(it),
        '集成测试不得依赖外部探测服务器');
    assert.ok(!/www\.|\.com\b|\.net\b|\.org\b/.test(it),
        '集成测试不得依赖 DNS 域名解析');
    // (c) 时序确定性：失败进入点靠 PAC 定时器（39.5s）这一确定性事件，
    //     等待和轮询都带固定上界（70s / 20s / 15s），不是靠运气。
    assert.match(it, /keep_credential_lines\(/,
        '测试必须用「只留 ufrag/pwd、剥掉候选」的确定性防火墙形状（空 check list）');
    assert.match(it, /70000/, '等待 Failed 必须带固定超时上界');
    assert.match(it, /39\.5 s|39500/, '注释必须写明 PAC 定时器（39.5s）这一确定性触发点');

    // (d) 集成：主用例（41s）在门禁环境里能整跑并得出固定结论（见标准 5 的整跑）。
    const t = runCtest(LOOPBACK_SUITE);
    assert.equal(t.status, 0,
        `IceTcpLoopback 未通过（exit=${t.status}）：\n${t.out}`);
    assert.ok(gtestOk(t.out, 'IceTcpLoopback.TcpOnlyShapeSelectsTcpPair'),
        '确定性主用例未跑出 OK：\n' + t.out);
});

// ---------------------------------------------------------------------------
// 不破坏既有行为（ICE 模块回归）
// ---------------------------------------------------------------------------
test('不破坏既有行为；验证方式：ICE 既有套件全绿（UDP 选路 / 候选 SDP / consent / restart / TURN）', () => {
    // 本片把 TCP 候选并进本地候选面、在 selected_pair() 里先查 TCP 提名，
    // 但 UDP 那条路的行为不能变 —— 用 ICE 模块既有套件做回归。
    const regression = [
        { re: '^IceTransport',     note: 'ICE 传输（工厂/打开/选路 + UDP 回环提名回归）' },
        { re: '^CandidateSdp\\.',  note: '候选 SDP 解析/序列化（parse_candidate/to_sdp 回归）' },
        { re: '^IceConsentFreshness\\.', note: 'consent freshness' },
        { re: '^IceRestart\\.',    note: 'ICE restart' },
        { re: '^Turn',             note: 'TURN 中继 / TURN-TCP / TURN-TLS' },
    ];
    for (const r of regression) {
        const res = runCtest(r.re);
        assert.equal(res.status, 0,
            `${r.note}（${r.re}）未通过（exit=${res.status}）：\n${res.out}`);
        assert.ok(res.out.includes('[==========]'),
            `${r.note}（${r.re}）没有产出 gtest 运行输出：\n${res.out}`);
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
    // 写集 = 规格「范围」（src/modules/ice/**、src/modules/sdp/**、
    // ice_transport.hpp）；外加验收文件本身、规格文件，以及会话开始前
    // 已存在的子模块脏状态（git status 快照，不是本片改出来的）。
    const ALLOW = [
        'src/modules/ice',
        'src/modules/sdp',
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp',
        'tests/acceptance/v2-ice',
        'specs/slices/v2-ice.md',
        // —— ADR-015 公开头基线（人已授权扩写集，照 v1-device-policy 先例）——
        // 新增规格强制的公开头 tcp_candidate.hpp 让公开头数量 197 -> 198：
        // kBaselineHeaderCount 必须跟着 bump、冻结的 manifest 必须补一条、
        // CHANGELOG 按 check_api_manifest.py 的指引记录理由。
        'docs/api/public_headers.manifest',
        'tests/test_api_classification.cpp',
        'CHANGELOG.md',
        // —— 会话前已存在的子模块脏状态 ——
        'src/third_party/dav1d/src',
        'src/third_party/gmssl/src',
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
        // seam + 引擎接口
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp',
        'src/modules/ice/include/nimrtc/ice/ice.hpp',
        // TCP 协作者
        'src/modules/ice/include/nimrtc/ice/tcp_candidate.hpp',
        'src/modules/ice/src/tcp_socket.cpp',
        'src/modules/ice/src/tcp_socket.hpp',
        'src/modules/ice/src/tcp_framing.cpp',
        'src/modules/ice/src/tcp_framing.hpp',
        'src/modules/ice/src/tcp_ice.cpp',
        'src/modules/ice/src/tcp_ice.hpp',
        // 引擎内集成
        'src/modules/ice/src/ice.cpp',
        // ADR-015 公开头基线产物（新增公开头 tcp_candidate.hpp 时同步）
        'docs/api/public_headers.manifest',
        // 测试
        'src/modules/ice/tests/test_tcp_candidate.cpp',
        'src/modules/ice/tests/test_tcp_framing.cpp',
        'src/modules/ice/tests/test_tcp_stun.cpp',
        'src/modules/ice/tests/test_ice_tcp.cpp',
        // 本片产物
        'tests/acceptance/v2-ice/acceptance.test.cjs',
        'specs/slices/v2-ice.md',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v2-ice' });
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

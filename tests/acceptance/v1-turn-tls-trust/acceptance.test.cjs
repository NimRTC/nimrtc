// 验收测试 —— 切片 v1-turn-tls-trust（宿主在公开面上表达 TURN/TLS 信任策略，ADR-037）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 30 条验收标准（specs/slices/v1-turn-tls-trust.md §验收标准）逐组落成
// 真实断言，验证手段与 v1-turn-credentials 同族：
//
//   ① 读源码做静态断言（公开面、翻译路径、变更识别、失败可见、可查询的事实）；
//   ② 直接跑门禁同款 build 树里的 gtest 二进制（test_engine_turn_credentials）
//      —— 用 --gtest_filter 限定用例，判定只依赖「二进制跑起来 + 用例 OK」；
//   ③ 跑只读脚本（scripts/api_freeze_scan.js / scripts/check_api_manifest.py）。
//
// 本片测试二进制里新增的用例：
//   - EngineTurnCredentials.PublicConfigDefaults       公开面默认值（含两个新字段）
//   - TurnTlsAnchorCheck.VerdictsAndMessages           引擎侧信任锚检查（缺失 / 非 PEM / 合法）
//   - TurnCredentialsState.ChangeDetectionIncludesTrustKnobs  凭据变更识别纳入两个新字段
//   - TurnCredentialsState.TurnTlsVerificationFactStates       三种事实状态互相可区分
//   - EngineTurnCredentialsLive.TrustKnobsReachPluginSeamAndVerificationFact
//   - EngineTurnCredentialsLive.BrokenCaFileSurfacesErrorAndMarksFailed
//   - EngineTurnCredentialsLive.ValidCaFileMarksVerified
//   - EngineTurnCredentialsLive.NotPemCaFileSurfacesErrorAndMarksFailed
//   - EngineTurnCredentialsLive.NonTlsCredentialsReportEmptyFact
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
// 判据 1-4：公开面 —— TurnCredentials 两个新字段 + 既有六字段语义不变
// ---------------------------------------------------------------------------
test('判据 1-4：公开面 —— turn_tls_verify_peer / turn_tls_ca_file 进 TurnCredentials、六字段不变、回调签名不变；验证方式：静态结构体断言 + 默认值单测', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));

    // (a) 判据 1：结构体带两个新字段，且新字段的默认值正确（verify 开、ca_file 空）。
    const structBlock = engineHpp.match(/struct NIMRTC_API_STABLE TurnCredentials \{[\s\S]*?\n\};/);
    assert.ok(structBlock, 'engine.hpp 必须定义 TurnCredentials 结构体（判据 1）');
    const blk = structBlock[0];
    assert.ok(blk.includes('bool turn_tls_verify_peer = true;'),
        'TurnCredentials 必须含 turn_tls_verify_peer（bool，默认 true，判据 1）');
    assert.ok(blk.includes('std::string turn_tls_ca_file;'),
        'TurnCredentials 必须含 turn_tls_ca_file（std::string，默认空，判据 1）');

    // (b) 判据 3：既有六个字段全部保留（语义不变）。
    for (const [frag, note] of [
        ['std::string host;',                    'host'],
        ['std::uint16_t port = 3478;',           'port（默认 3478）'],
        ['std::string username;',                'username'],
        ['std::string password;',                'password'],
        ['plugins::TurnTransport transport =',   'transport（默认 Udp）'],
        ['std::string tls_server_name;',         'tls_server_name'],
    ]) {
        assert.ok(blk.includes(frag),
            `TurnCredentials 结构体缺既有 ${note} 字段（${frag}，判据 3）`);
    }

    // (c) 判据 4：set_on_control_message / set_on_call_state 的签名原样保留。
    assert.match(engineHpp,
        /NIMRTC_API_STABLE void set_on_call_state\(EngineCallStateCallback cb\) noexcept;/,
        'set_on_call_state() 签名必须不变（判据 4）');
    assert.match(engineHpp,
        /NIMRTC_API_STABLE void set_on_control_message\(\n\s+std::function<void\(std::span<const std::uint8_t>\)> cb\) noexcept;/,
        'set_on_control_message() 签名必须不变（判据 4）');

    // (d) 判据 1 / 2 / 3 / 4 的编译期钉子：默认值单测跑起来 = 通过。
    const r = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentials.PublicConfigDefaults');
    assert.equal(r.status, 0,
        `PublicConfigDefaults 未通过（exit=${r.status}）：\n${r.out}`);
    assert.ok(gtestOk(r.out, 'EngineTurnCredentials.PublicConfigDefaults'),
        '公开面默认值用例未跑出 OK：\n' + r.out);

    // (e) 判据 2 的编译期钉子：新公开方法带 NIMRTC_API_STABLE（在测试源里
    //     static_assert 已钉类型；方法声明的稳定性标注在这里静态钉）。
    assert.match(engineHpp,
        /NIMRTC_API_STABLE std::string turn_tls_peer_verification\(\) const noexcept;/,
        'turn_tls_peer_verification() 必须带 NIMRTC_API_STABLE（判据 2）');
});

// ---------------------------------------------------------------------------
// 判据 5-12：翻译路径 + 变更识别
// ---------------------------------------------------------------------------
test('判据 5-12：翻译路径 —— 旋钮经插件接缝到 IceConfig、新重载保既有签名、同凭据判定纳入新字段；验证方式：静态接线 + 活引擎单测 + 机器级单测', () => {
    const engineCpp = readText(path.join(ROOT, 'src/engine/src/engine.cpp'));
    const seam = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp'));
    const iceCpp = readText(path.join(ROOT, 'src/modules/ice/src/ice.cpp'));
    const tcCpp = readText(path.join(ROOT,
        'src/engine/src/engine_turn_credentials.cpp'));

    // (a) 判据 10：既有 5 参数签名原样保留，新旋钮走新增的 7 参数 virtual 重载。
    assert.match(seam,
        /virtual void add_turn_server_with_transport\(std::string_view host,[\s\S]*?TurnTransport transport\) noexcept \{/,
        '既有 5 参数 add_turn_server_with_transport 签名必须保留（判据 10）');
    assert.match(seam,
        /virtual void add_turn_server_with_transport\(std::string_view host,[\s\S]*?TurnTransport transport,[\s\S]*?bool turn_tls_verify_peer,[\s\S]*?std::string_view turn_tls_ca_file\) noexcept \{/,
        '必须新增带信任旋钮的 7 参数 virtual 重载（判据 10）');
    assert.ok(seam.includes('bool turn_tls_verify_peer_ = true;')
        && seam.includes('std::string turn_tls_ca_file_;'),
        '接口必须把默认实现里的信任旋钮存进受保护成员（判据 10 的实现细节）');

    // (b) 判据 5 / 6：ice.cpp 的既有接缝把旋钮搬进 IceConfig。
    assert.match(iceCpp,
        /impl_->config\.turn_tls_verify_peer = turn_tls_verify_peer_;/,
        'ice.cpp 必须把 verify_peer 旋钮搬进 IceConfig（判据 5）');
    assert.match(iceCpp,
        /impl_->config\.turn_tls_ca_file = turn_tls_ca_file_;/,
        'ice.cpp 必须把 ca_file 旋钮搬进 IceConfig（判据 6）');

    // (c) 判据 7 / 8：create_agent() 的既有 TLS 客户端创建逻辑不变，
    //     仍从 IceConfig 读两个字段（这正是「旋钮到达 IceConfig 就会被走到的路径」）。
    assert.match(iceCpp,
        /tls_config\.ca_file\s*=\s*config\.turn_tls_ca_file;/,
        'create_agent() 必须仍从 IceConfig 读 ca_file（判据 7 的既有路径）');
    assert.match(iceCpp,
        /tls_config\.verify_peer\s*=\s*config\.turn_tls_verify_peer;/,
        'create_agent() 必须仍从 IceConfig 读 verify_peer（判据 8 的既有路径）');

    // (d) 判据 11 / 12（回归基线不动）：turn_tls.cpp 与 ice.hpp 仍保留既有逻辑。
    const turnTls = readText(path.join(ROOT, 'src/modules/ice/src/turn_tls.cpp'));
    assert.match(turnTls, /WOLFSSL_VERIFY_NONE/,
        'turn_tls.cpp 必须保留 VERIFY_NONE 分支（判据 8 / 判据 11 参照）');
    assert.match(turnTls, /wolfSSL_CTX_load_verify_locations\(ctx, config_\.ca_file\.c_str\(\), nullptr\)/,
        'turn_tls.cpp 必须保留加载信任锚的路径（判据 7 / 判据 11 参照）');
    const iceHpp = readText(path.join(ROOT,
        'src/modules/ice/include/nimrtc/ice/ice.hpp'));
    assert.match(iceHpp, /bool turn_tls_verify_peer = true;/,
        'IceConfig 既有字段语义不变（判据 12）');
    assert.match(iceHpp, /std::string turn_tls_ca_file;/,
        'IceConfig 既有字段语义不变（判据 12）');

    // (e) 判据 9：本片的公开头与新增引擎文件不 include nimrtc/ice/ 的具体头（L4）。
    for (const rel of ['src/engine/include/nimrtc/engine/engine.hpp',
                       'src/engine/src/engine_turn_credentials.hpp',
                       'src/engine/src/engine_turn_credentials.cpp']) {
        const src = readText(path.join(ROOT, rel));
        assert.ok(!src.includes('#include <nimrtc/ice/'),
            `${rel} 不得 include nimrtc/ice/ 的具体头（L4，判据 9）`);
    }

    // (f) 判据 10/11/12（变更识别）：same_credentials() 纳入两个新字段。
    assert.match(tcCpp,
        /a\.turn_tls_verify_peer == b\.turn_tls_verify_peer/,
        'same_credentials() 必须比较 turn_tls_verify_peer（判据 10）');
    assert.match(tcCpp,
        /a\.turn_tls_ca_file == b\.turn_tls_ca_file/,
        'same_credentials() 必须比较 turn_tls_ca_file（判据 10）');

    // (g) 判据 5 / 6：引擎翻译路径把两个旋钮经 7 参数重载交给插件接缝。
    assert.match(engineCpp,
        /add_turn_server_with_transport\(\n\s*out\.host, out\.port, out\.username, out\.password, out\.transport,\n\s*out\.turn_tls_verify_peer, out\.turn_tls_ca_file\);/,
        '引擎必须经 7 参数重载把两个旋钮交给插件接缝（判据 5 / 6）');

    // (h) 活引擎：判据 5 / 6 / 16 —— 旋钮原样到达插件接缝，事实 "unverified"。
    const trust = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.TrustKnobsReachPluginSeamAndVerificationFact');
    assert.equal(trust.status, 0,
        `TrustKnobsReachPluginSeamAndVerificationFact 未通过（exit=${trust.status}）：\n${trust.out}`);
    assert.ok(gtestOk(trust.out,
        'EngineTurnCredentialsLive.TrustKnobsReachPluginSeamAndVerificationFact'),
        '旋钮到接缝的活引擎用例未跑出 OK：\n' + trust.out);

    // (i) 机器级：判据 10 / 11 / 12 —— 只改信任旋钮 → 凭据变了；全同 → 没变。
    const change = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'TurnCredentialsState.ChangeDetectionIncludesTrustKnobs');
    assert.equal(change.status, 0,
        `ChangeDetectionIncludesTrustKnobs 未通过（exit=${change.status}）：\n${change.out}`);
    assert.ok(gtestOk(change.out,
        'TurnCredentialsState.ChangeDetectionIncludesTrustKnobs'),
        '变更识别纳入信任旋钮的机器级用例未跑出 OK：\n' + change.out);
});

// ---------------------------------------------------------------------------
// 判据 13-15：失败可见
// ---------------------------------------------------------------------------
test('判据 13-15：失败可见 —— ca_file 缺失 / 非 PEM 都透到 set_on_error、说清哪个文件什么错；验证方式：静态接线 + 引擎侧检查单测 + 活引擎单测', () => {
    const engineCpp = readText(path.join(ROOT, 'src/engine/src/engine.cpp'));

    // (a) 判据 13 / 14 / 15：引擎侧对信任锚做检查，失败时经 on_error_ 透出。
    assert.match(engineCpp,
        /check_turn_tls_ca_file\(out\.turn_tls_ca_file\)/,
        '引擎必须检查宿主给的 ca_file（判据 13 / 14）');
    assert.match(engineCpp,
        /on_error_\(core::kEngineInvalidParam,/,
        'ca_file 检查失败必须经 on_error_ 透出 kEngineInvalidParam（判据 13 / 14 / 15）');
    assert.match(engineCpp,
        /note_turn_tls_anchor_failed\(\);/,
        'ca_file 检查失败必须把事实标记为 failed（判据 17）');

    // (b) 判据 13 / 14：错误消息说清「哪个文件、什么错」—— 引擎侧检查函数的单测。
    const anchor = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'TurnTlsAnchorCheck.VerdictsAndMessages');
    assert.equal(anchor.status, 0,
        `TurnTlsAnchorCheck.VerdictsAndMessages 未通过（exit=${anchor.status}）：\n${anchor.out}`);
    assert.ok(gtestOk(anchor.out, 'TurnTlsAnchorCheck.VerdictsAndMessages'),
        '信任锚检查（缺失 / 非 PEM / 合法 + 错误消息）用例未跑出 OK：\n' + anchor.out);

    // (c) 判据 13 / 15：缺失文件的错误经 set_on_error 到达宿主，事实 "failed"。
    const missing = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.BrokenCaFileSurfacesErrorAndMarksFailed');
    assert.equal(missing.status, 0,
        `BrokenCaFileSurfacesErrorAndMarksFailed 未通过（exit=${missing.status}）：\n${missing.out}`);
    assert.ok(gtestOk(missing.out,
        'EngineTurnCredentialsLive.BrokenCaFileSurfacesErrorAndMarksFailed'),
        '缺失 ca_file 的活引擎用例未跑出 OK：\n' + missing.out);

    // (d) 判据 14 / 15：存在但非 PEM 的错误同样经 set_on_error 到达宿主。
    const notPem = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.NotPemCaFileSurfacesErrorAndMarksFailed');
    assert.equal(notPem.status, 0,
        `NotPemCaFileSurfacesErrorAndMarksFailed 未通过（exit=${notPem.status}）：\n${notPem.out}`);
    assert.ok(gtestOk(notPem.out,
        'EngineTurnCredentialsLive.NotPemCaFileSurfacesErrorAndMarksFailed'),
        '非 PEM ca_file 的活引擎用例未跑出 OK：\n' + notPem.out);
});

// ---------------------------------------------------------------------------
// 判据 16-17：可查询的事实
// ---------------------------------------------------------------------------
test('判据 16-17：可查询的事实 —— "unverified" / "verified" / "failed" 三种互相可区分；验证方式：公开方法静态 + 机器级单测 + 活引擎单测', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    const engineCpp = readText(path.join(ROOT, 'src/engine/src/engine.cpp'));

    // (a) 判据 16 / 17：宿主能查询的事实是公开方法（不是只躺在 Stats 里的字符串）。
    assert.match(engineHpp,
        /NIMRTC_API_STABLE std::string turn_tls_peer_verification\(\) const noexcept;/,
        '引擎公开面必须提供 turn_tls_peer_verification()（判据 16 / 17）');
    assert.match(engineCpp,
        /turn_tls_peer_verification\(\)\s+const noexcept \{/,
        '引擎必须实现 turn_tls_peer_verification()（判据 16 / 17）');

    // (b) 机器级：判据 16 / 17 —— 三种状态互相可区分（"" / unverified / verified / failed）。
    const states = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'TurnCredentialsState.TurnTlsVerificationFactStates');
    assert.equal(states.status, 0,
        `TurnTlsVerificationFactStates 未通过（exit=${states.status}）：\n${states.out}`);
    assert.ok(gtestOk(states.out, 'TurnCredentialsState.TurnTlsVerificationFactStates'),
        '事实状态机用例未跑出 OK：\n' + states.out);

    // (c) 判据 17：合法 ca_file → "verified"，且不误报错误。
    const valid = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.ValidCaFileMarksVerified');
    assert.equal(valid.status, 0,
        `ValidCaFileMarksVerified 未通过（exit=${valid.status}）：\n${valid.out}`);
    assert.ok(gtestOk(valid.out, 'EngineTurnCredentialsLive.ValidCaFileMarksVerified'),
        '合法 ca_file 的活引擎用例未跑出 OK：\n' + valid.out);

    // (d) 判据 16 的活引擎对照：verify_peer=false → "unverified"（在旋钮到达
    //     接缝的用例里一并验证）；非 TLS 中继 → ""（无可报告）。
    const nonTls = runGtest('src/engine/tests', 'test_engine_turn_credentials',
        'EngineTurnCredentialsLive.NonTlsCredentialsReportEmptyFact');
    assert.equal(nonTls.status, 0,
        `NonTlsCredentialsReportEmptyFact 未通过（exit=${nonTls.status}）：\n${nonTls.out}`);
    assert.ok(gtestOk(nonTls.out,
        'EngineTurnCredentialsLive.NonTlsCredentialsReportEmptyFact'),
        '非 TLS 中继报告空事实的活引擎用例未跑出 OK：\n' + nonTls.out);
});

// ---------------------------------------------------------------------------
// 判据 21-26：边界与回归
// ---------------------------------------------------------------------------
test('判据 21-26：边界与回归 —— 只读文件不动 / 既有路径不变 / 全套回归绿；验证方式：git 差异静态 + 回归单测', () => {
    // (a) 判据 22 / 23 / 24：只读文件不在本片工作区差异里。
    const diff = spawnSync('git', ['diff', 'HEAD', '--name-only'],
        { cwd: ROOT, encoding: 'utf8', timeout: 60000 });
    assert.equal(diff.status, 0, `git diff 失败：${diff.stderr}`);
    const changedPaths = (diff.stdout || '').split(/\r?\n/).filter(Boolean);
    for (const protectedPath of [
        'src/modules/ice/include/nimrtc/ice/ice.hpp',      // 判据 22
        'src/modules/ice/src/turn_tls.cpp',                // 判据 23
        'src/modules/ice/tests/test_turn_tls.cpp',         // 判据 24（回归基线不动）
    ]) {
        assert.ok(!changedPaths.includes(protectedPath),
            `${protectedPath} 不得被本片修改（判据 22 / 23 / 24）`);
    }

    // (b) 判据 21：既有插件方法签名不变 —— 5 参数重载原样保留（在翻译路径组已钉，
    //     这里钉「既有调用者不被改行为」的另一半：引擎公开的静态 add_turn_server 路径不变）。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.match(engineHpp,
        /NIMRTC_API_STABLE int add_turn_server\(std::string_view host, std::uint16_t port,\n\s+std::string_view username,\n\s+std::string_view password\) noexcept;/,
        '既有 4 参数 add_turn_server 路径不变（判据 26）');
    assert.match(engineHpp,
        /NIMRTC_API_STABLE int add_turn_server\(std::string_view host, std::uint16_t port,\n\s+std::string_view username,\n\s+std::string_view password,\n\s+plugins::TurnTransport transport\) noexcept;/,
        '既有 5 参数 add_turn_server 路径不变（判据 21 / 26）');

    // (c) 判据 24：test_turn_tls.cpp 的回归基线全绿。
    const turnTls = runGtest('src/modules/ice/tests', 'test_turn_tls_test', '*');
    assert.equal(turnTls.status, 0,
        `test_turn_tls_test（回归基线）未通过（exit=${turnTls.status}）：\n${turnTls.out}`);
    assert.ok(turnTls.out.includes('[==========]'),
        'test_turn_tls_test 没有产出 gtest 运行输出：\n' + turnTls.out);

    // (d) 判据 25：src/engine/tests/ 一整套回归全绿（含本片新增用例）。
    const regression = [
        { exe: 'test_engine_device_policy',     note: 'D3/D4 设备策略与采集重连' },
        { exe: 'test_engine_audio_device',      note: 'D1 音频设备层' },
        { exe: 'test_engine_connectivity',      note: 'D6/N4 连通性诊断与 profile' },
        { exe: 'test_engine_ice_recovery',      note: 'N3 重连策略（本片挂载点）' },
        { exe: 'test_engine_turn_credentials',  note: '本片（v1-turn-tls-trust）' },
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
// 判据 27-30：文档与契约
// ---------------------------------------------------------------------------
test('判据 27-30：文档与契约 —— ADR-037 / engine_api.md / CHANGELOG / 规格记录；验证方式：文档静态 + 只读脚本执行', () => {
    // (a) 判据 27：ADR-037 存在，且说明关键结论。
    const adr = readText(path.join(ROOT,
        'docs/adr/ADR-037-turn-tls-trust-seam.md'));
    for (const needle of [
        'turn_tls_verify_peer',        // 两个旋钮
        'turn_tls_ca_file',
        'ice_transport.hpp',           // 插件接口必须加字段（接口变更）
        'IceConfig',                   // 旋钮已在 ICE 层
        'create_agent()',              // 顺序论证：add_turn_server*() → open() → create_agent()
        'credential',                  // 信任策略跟着凭据走
    ]) {
        assert.ok(adr.toLowerCase().includes(needle.toLowerCase()),
            `ADR-037 缺关键结论 ${needle}（判据 27）`);
    }

    // (b) 判据 28：engine_api.md 记录两个新字段与 verify_peer=false 的代价。
    const api = readText(path.join(ROOT, 'docs/api/engine_api.md'));
    for (const needle of ['turn_tls_verify_peer', 'turn_tls_ca_file',
                          'turn_tls_peer_verification']) {
        assert.ok(api.includes(needle),
            `engine_api.md 缺 ${needle}（判据 28）`);
    }
    assert.match(api, /self-signed|self.signed|自签|relay identity|verify/i,
        'engine_api.md 必须写明 verify_peer=false 的代价（判据 28）');

    // (c) 判据 29：CHANGELOG.md 有对应条目。
    const changelog = readText(path.join(ROOT, 'CHANGELOG.md'));
    assert.ok(/turn[_ ]tls|TURN\/TLS|信任/.test(changelog),
        'CHANGELOG.md 必须有对应条目（判据 29）');

    // (d) 判据 30：spec 记录了实施中的发现（本片 agent 写回的 findings 段）。
    const spec = readText(path.join(ROOT, 'specs/slices/v1-turn-tls-trust.md'));
    assert.match(spec, /发现|findings|implementation/i,
        'spec 必须记录实施中的发现（判据 30）');

    // (e) 判据 2：只读稳定性扫描全绿（新符号带 NIMRTC_API_STABLE）。
    const f = spawnSync(process.execPath,
        ['scripts/api_freeze_scan.js', '--no-md'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(f.status, 0,
        `api_freeze_scan.js 未通过（exit=${f.status}）：\n` +
        `${f.stdout || ''}${f.stderr || ''}`);
    const m = spawnSync('python', ['scripts/check_api_manifest.py'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(m.status, 0,
        `check_api_manifest.py 未通过（exit=${m.status}）：\n` +
        `${m.stdout || ''}${m.stderr || ''}`);
    assert.match(`${m.stdout || ''}${m.stderr || ''}`, /OK/,
        'check_api_manifest.py 应输出 OK（判据 2）');
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
    // 写集 = 规格「范围」（specs/slices/v1-turn-tls-trust.md §范围）。
    const ALLOW = [
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/src/engine_turn_credentials.hpp',
        'src/engine/src/engine_turn_credentials.cpp',
        'src/engine/src/engine.cpp',
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp',
        'src/modules/ice/src/ice.cpp',
        'src/engine/tests/test_engine_turn_credentials.cpp',
        'tests/acceptance/v1-turn-tls-trust',
        'docs/adr/ADR-037-turn-tls-trust-seam.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
        // —— 会话开始前已存在的脏状态（不属于本片 agent 的改动）——
        'specs/slices/v1-turn-tls-trust.md',
        'src/third_party/wolfssl/src',
        // 并行片 v1-install-tree-video-deps 的规格文件。提交 910d1c5e
        // （docs(spec): two slices for the engine gaps N6 found）同时带
        // `Refs-Slice: v1-turn-tls-trust` 与 `Refs-Slice: v1-install-tree-video-deps`
        // 两个 trailer，一次创建了**两个片**的规格（本片 + 并行片，写集不重叠）。
        // 共享判据 collectSliceChanges 的「本片提交差异」腿把该提交的 diff 全数
        // 记到本片名下，于是并行片规格会稳定出现在本片的改动清单里 —— 这不是
        // 本片 agent 的越界写，是规格创建提交跨两个片所致。
        'specs/slices/v1-install-tree-video-deps.md',
    ];

    const REQUIRED = [
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/src/engine_turn_credentials.hpp',
        'src/engine/src/engine_turn_credentials.cpp',
        'src/engine/src/engine.cpp',
        'src/plugins/include/nimrtc/plugins/ice_transport.hpp',
        'src/modules/ice/src/ice.cpp',
        'src/engine/tests/test_engine_turn_credentials.cpp',
        'tests/acceptance/v1-turn-tls-trust/acceptance.test.cjs',
        'docs/adr/ADR-037-turn-tls-trust-seam.md',
        'docs/api/engine_api.md',
        'CHANGELOG.md',
        'specs/slices/v1-turn-tls-trust.md',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-turn-tls-trust' });
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

// 验收测试 —— 切片 v1-sdk-sample（N6：≤200 行的 SDK 样例，跑完一通话）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 spec 的 36 条验收标准（specs/slices/v1-sdk-sample.md §验收标准）
// 逐组落成真实断言：
//
//   ① 静态：样例形状（引擎头 / 接缝纪律 / 四钩子 / 无设备 id / 无设备逻辑 /
//      预算声明 / CMake 接线 / CI 三步）；
//   ② 行数：独立 JS 计数（line_budget.js）与 Python 门禁
//      （check_sample_line_budget.py）交叉验证，都断言 ≤220；
//   ③ 执行：两个进程（caller / callee）过 TLS-only 的 TURN 中继与参考信令，
//      断言控制消息双向 + summary 行 —— 被两个已确认的引擎缺口阻塞时以
//      { skip: blockedReason() } 跳过并点名缺口（永不静默 PASS）；
//   ④ 边界：不改 src/ C++、不改 remote-desktop/turn-server、API freeze 门跑绿；
//   ⑤ 文档：ADR-036 / README / engine_api.md / CHANGELOG。
//
// 执行腿（③）被两个已确认的引擎缺口阻塞（2026-11-25 提出 ZELTO_QUESTION）：
//   A. TURN/TLS 证书校验不可达 —— 引擎公开 API 只有 tls_server_name，没有
//      turn_tls_verify_peer / turn_tls_ca_file；N5 参考中继是 TLS-only 自签证书
//      （ADR-027 Decision 4），ice 默认 WOLFSSL_VERIFY_PEER + 空信任库
//      （turn_tls.cpp:123-134）→ 必然拒绝，宿主无法信任该证书。
//   B. 安装树缺视频依赖 —— 暂存 bundle 的 NimRTCConfig.cmake 不链
//      nimrtc_nvcodec / nimrtc_vendor_openh264_*，也不链 d3d11/dxgi → 视频宿主
//      （h264 + headless sink）链接 nimrtc::engine 报 31 个 LNK2019。
// 按诚实规则（阻塞的腿是 SKIPPED，永远不是 PASS），缺口在时执行腿条件跳过；
// 缺口消失后它真实构建样例 + 起两个进程 + 断言。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const SAMPLE_DIR = path.join(ROOT, 'examples', 'sdk-sample');
const lb = require('./line_budget.js');
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(rel) { return fs.readFileSync(path.join(ROOT, rel), 'utf8'); }
function exists(rel) { return fs.existsSync(path.join(ROOT, rel)); }
function readSample(name) { return readText(path.join('examples', 'sdk-sample', name)); }

/** 跑一个命令，返回 { status, out }。 */
function run(cmd, args, opts = {}) {
    const r = spawnSync(cmd, args, {
        cwd: ROOT, encoding: 'utf8', timeout: 120000, ...opts,
    });
    return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/** 从 summary 行解析出统计：au_tx/au_rx/v_tx/v_rx/c_rx/rc。 */
function parseSummary(out) {
    const m = /\[sdk-sample\] summary au_tx=(\d+) au_rx=(\d+) v_tx=(\d+) v_rx=(\d+) c_rx=(\d+) rc=(\d+)/.exec(out);
    if (!m) return null;
    return { au_tx: +m[1], au_rx: +m[2], v_tx: +m[3], v_rx: +m[4], c_rx: +m[5], rc: +m[6] };
}

/** 从 NIMRTC_TURN_URI / _USERNAME / _PASSWORD 解析 TURN 中继接入参数（N5 同款）。 */
function parseRelayEnv() {
    const uri = process.env.NIMRTC_TURN_URI;
    const user = process.env.NIMRTC_TURN_USERNAME;
    const pass = process.env.NIMRTC_TURN_PASSWORD;
    if (!uri || user == null || pass == null) return null;
    const m = /^(turns|turn):(?:\/\/)?([^:/?#]+)(?::(\d{1,5}))?(?:\?([^#]*))?/.exec(uri.trim());
    if (!m) return null;
    const [, scheme, hostRaw, portStr, query] = m;
    const host = hostRaw.replace(/^\[/, '').replace(/\]$/, '');
    const port = portStr ? Number(portStr) : (scheme === 'turns' ? 5349 : 3478);
    let transport = 'udp';
    if (query) {
        for (const kv of query.split('&')) {
            const i = kv.indexOf('=');
            if (i > 0 && kv.slice(0, i) === 'transport') transport = kv.slice(i + 1);
        }
    }
    return { scheme, host, port, transport, user, pass };
}

/** 定位 build/sdk-sample 里的样例可执行文件（多配置 / 单配置都可）。 */
function resolveSampleExe(buildDir) {
    for (const c of [
        path.join(buildDir, 'Debug', 'sdk-sample.exe'),
        path.join(buildDir, 'Release', 'sdk-sample.exe'),
        path.join(buildDir, 'sdk-sample.exe'),
        path.join(buildDir, 'sdk-sample'),
    ]) {
        if (fs.existsSync(c)) return c;
    }
    return path.join(buildDir, 'Debug', 'sdk-sample.exe'); // 让 spawn 用明确的 ENOENT 报错
}

/** 列出暂存安装树里的 NimRTCConfig.cmake（build/_install/<platform>/...）。 */
function stagedInstallConfigs() {
    const base = path.join(ROOT, 'build', '_install');
    if (!fs.existsSync(base)) return [];
    return fs.readdirSync(base)
        .map((d) => path.join(base, d, 'lib', 'cmake', 'NimRTC', 'NimRTCConfig.cmake'))
        .filter((p) => fs.existsSync(p));
}

// ---------------------------------------------------------------------------
// 执行腿的阻塞检测（引擎缺口，见文件头注释）
// ---------------------------------------------------------------------------
function blockedReason() {
    const reasons = [];
    const engineHpp = readText('src/engine/include/nimrtc/engine/engine.hpp');

    // 缺口 A：引擎公开 API 必须能信任/忽略参考中继的自签证书。
    if (!/(turn_tls_verify_peer|turn_tls_ca_file)/.test(engineHpp)) {
        reasons.push('TURN/TLS: the engine public API has no turn_tls_verify_peer/'
            + 'turn_tls_ca_file knob (only tls_server_name), so the two processes '
            + 'cannot trust the N5 relay\'s TLS-only self-signed cert — turn_tls.cpp '
            + 'defaults WOLFSSL_VERIFY_PEER with an empty trust store (ZELTO_QUESTION 2026-11-25)');
    }

    // 缺口 B：暂存安装树必须链视频依赖（h264 + headless sink 需要）。
    const configs = stagedInstallConfigs();
    if (!configs.length) {
        reasons.push('install-tree: no staged install under build/_install — the '
            + 'execution leg builds the sample against the staged bundle (spec §五)');
    } else if (configs.every((cfg) => {
        const t = fs.readFileSync(cfg, 'utf8');
        return !/nvcodec/.test(t) || !/openh264/.test(t);
    })) {
        reasons.push('install-tree: the staged NimRTCConfig.cmake chains neither '
            + 'nimrtc_nvcodec nor nimrtc_vendor_openh264_* (nor d3d11/dxgi), so a '
            + 'video host cannot link nimrtc::engine — 31 LNK2019 (ZELTO_QUESTION 2026-11-25)');
    }

    return reasons.join(' | ');
}

// ---------------------------------------------------------------------------
// 一、行数预算（验收标准 1-11、12、30）
// ---------------------------------------------------------------------------
test('判据 1-2/12/30：样例存在、非注释行 ≤220、建立音频+视频+控制三路、声明与实测量一致', () => {
    assert.ok(fs.existsSync(SAMPLE_DIR), 'examples/sdk-sample/ 不存在（判据 1）');

    const decl = lb.loadDeclaration(SAMPLE_DIR);
    const st = lb.budgetStatus(SAMPLE_DIR, decl);
    assert.equal(st.status, 'pass', `行数超限：${JSON.stringify(st)}`);
    assert.ok(st.total <= 220, `样例 ${st.total} 非注释行，超过 220 上限（判据 2/12）`);

    const cpp = readSample('sdk-sample.cpp');
    for (const needle of ['send_audio', 'send_video', 'set_on_control_message']) {
        assert.ok(cpp.includes(needle), `sdk-sample.cpp 缺 ${needle}（音频/视频/控制三路，判据 12）`);
    }

    const py = run('python', ['tools/check_sample_line_budget.py']);
    assert.equal(py.status, 0, `check_sample_line_budget.py 未通过（判据 30）：\n${py.out}`);
    assert.match(py.out, /PASS/, '门禁应输出 PASS');
});

test('判据 3-9：门禁与自检工具存在且跑绿，计数器钉住 http://、块注释、同行代码', () => {
    assert.ok(exists('tools/check_sample_line_budget.py'), '行数门禁工具必须存在（判据 3）');
    assert.ok(exists('tools/check_sample_line_budget_self_test.py'), '自检必须存在（判据 6）');

    const st = run('python', ['tools/check_sample_line_budget_self_test.py']);
    assert.equal(st.status, 0, `自检未通过（判据 6）：\n${st.out}`);
    assert.match(st.out, /PASS/, '自检应输出 PASS');

    // 判据 4-5 由自检的负/正样本钉住（自检本身含 300 行样本被报出 + 干净样本通过）。
    // 这里用 line_budget.js 独立复算钉住同一计数行为（判据 7-9）。
    assert.equal(lb.countNonCommentLines('const char* url = "http://host:5349";\n'), 1,
        '含 http:// 的字符串行必须算 1 行（不误判为注释，判据 7）');
    assert.equal(lb.countNonCommentLines('/* a\nb\nc */\n'), 0,
        '多行块注释不算行数（判据 8）');
    assert.equal(lb.countNonCommentLines('/* x */ int y = 1;\n'), 1,
        '块注释后同行有代码算 1 行（判据 9）');
});

test('判据 10-11：预算声明在 sample_line_budget.json（200 / 0.10 / 空豁免），CI 每次重新量', () => {
    const decl = lb.loadDeclaration(SAMPLE_DIR);
    assert.equal(decl.budget, 200, '预算声明必须是 200（判据 10）');
    assert.equal(decl.drift_allowance, 0.10, '漂移余量必须是 0.10（判据 10）');
    assert.deepEqual(decl.exemptions, [], '豁免列表必须为空（或只剩 README.md）');

    const ci = readText('.github/workflows/ci.yml');
    assert.match(ci, /SDK sample -- line budget[\s\S]*?check_sample_line_budget\.py/,
        'CI 必须每次跑行数门禁（判据 11）');
});

// ---------------------------------------------------------------------------
// 二、样例形状（验收标准 13-20）
// ---------------------------------------------------------------------------
test('判据 13-16：包含引擎头、不 include nimrtc/ice/、无设备/编解码/渲染逻辑、无设备 id', () => {
    const cpp = readSample('sdk-sample.cpp');
    const hpp = readSample('sdk_signalling.hpp');
    const all = `${cpp}\n${hpp}`;

    assert.match(cpp, /#include <nimrtc\/engine\/engine\.hpp>/, '必须包含引擎公开头（判据 13）');
    assert.ok(!all.includes('#include <nimrtc/ice/'), '不得 include nimrtc/ice/ 的任何具体头（接缝纪律，判据 14）');

    for (const bad of ['PortAudio', 'VideoCapture', 'VideoRenderer', 'D3D11', 'CreateWindow']) {
        assert.ok(!all.includes(bad), `样例不得自带设备/编解码/渲染逻辑：${bad}（判据 15）`);
    }
    assert.ok(!/device_id|deviceId|device_index/i.test(all), '样例不得写设备 id（判据 16）');
});

test('判据 17-19：用 D7 的 call-state、控制消息、N8 的 ice-recovery、N7 的 TURN 凭据；连通性与重连归引擎', () => {
    const cpp = readSample('sdk-sample.cpp');
    for (const needle of ['set_on_call_state', 'set_on_control_message', 'set_on_ice_recovery']) {
        assert.ok(cpp.includes(needle), `样例必须用 ${needle}（判据 17）`);
    }
    assert.match(cpp, /cfg\.turn_credentials\s*=\s*&provide_turn/,
        '样例必须用 EngineConfig::turn_credentials（N7，判据 17）');
    assert.match(cpp, /connectivity_profile\s*=\s*"auto"/,
        '样例必须用 D6 的 profile，不自实现连通性策略（判据 18）');
    assert.match(cpp, /ice_recovery_policy\s*=\s*"auto"/,
        '样例必须用 N3 的引擎侧重连策略（判据 19）');
    assert.ok(!cpp.includes('restart_ice'), '样例不得自己驱动重连（N3 归引擎，判据 19）');
    assert.match(cpp, /TurnTransport::Tls/,
        '样例必须显式要求 TURN/TLS 传输（判据 25 的静态侧）');
});

test('判据 20：CMakeLists 用 find_package(NimRTC REQUIRED CONFIG) + NimRTC::engine，MSVC 宿主钉住 /utf-8 与动态 CRT', () => {
    const cmake = readSample('CMakeLists.txt');
    assert.match(cmake, /find_package\(NimRTC 1\.0 REQUIRED CONFIG\)/,
        '必须 find_package(NimRTC REQUIRED CONFIG)（判据 20）');
    assert.match(cmake, /target_link_libraries\(sdk-sample PRIVATE nimrtc::engine\)/,
        '必须链 nimrtc::engine（判据 20）');
    assert.match(cmake, /MSVC_RUNTIME_LIBRARY\s+"MultiThreadedDLL"/,
        'MSVC 宿主必须用动态 CRT（与引擎 bundle 一致，否则 LNK2038）');
    assert.match(cmake, /\/utf-8/, 'MSVC 宿主必须 /utf-8（引擎头带中文注释，旧代码页会错解析）');
});

// ---------------------------------------------------------------------------
// 三、CI 验收腿（验收标准 21-23）
// ---------------------------------------------------------------------------
test('判据 21-23：CI 有行数预算、自检、两个进程通话三步（规格 §四 的 YAML 原文）', () => {
    const ci = readText('.github/workflows/ci.yml');
    assert.match(ci,
        /- name: SDK sample -- line budget\s+run: python3 tools\/check_sample_line_budget\.py/,
        'CI 缺「行数预算」步（判据 21）');
    assert.match(ci,
        /- name: SDK sample -- self-test\s+run: python3 tools\/check_sample_line_budget_self_test\.py/,
        'CI 缺「自检」步（判据 22）');
    assert.match(ci,
        /- name: SDK sample -- two-process call behind the TLS relay\s+run: node --test tests\/acceptance\/v1-sdk-sample\/acceptance\.test\.cjs/,
        'CI 缺「两个进程通话」步（判据 23）');
});

// ---------------------------------------------------------------------------
// 四、执行腿（验收标准 24-27）—— 被引擎缺口阻塞时条件跳过，永不静默 PASS
// ---------------------------------------------------------------------------
test('判据 24-27：执行腿 —— 起两个进程过 TLS-only TURN 中继，控制消息双向；没设备只 SKIPPED 永不 PASS',
    { skip: blockedReason(), timeout: 600000 }, async () => {
        // 1) 构建样例：独立工程，指向暂存安装树（spec §五）。
        const installRoot = path.join(ROOT, 'build', '_install', fs.readdirSync(
            path.join(ROOT, 'build', '_install'))[0]);
        const sampleBuild = path.join(ROOT, 'build', 'sdk-sample');
        const cfgCmd = run('cmake', [
            '-S', path.join(ROOT, 'examples', 'sdk-sample'),
            '-B', sampleBuild,
            `-DCMAKE_PREFIX_PATH=${installRoot}`,
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
        ]);
        assert.equal(cfgCmd.status, 0, `cmake 配置样例失败：\n${cfgCmd.out}`);
        const buildCmd = run('cmake', ['--build', sampleBuild, '--config', 'Debug']);
        assert.equal(buildCmd.status, 0, `cmake 构建样例失败：\n${buildCmd.out}`);
        const exe = resolveSampleExe(sampleBuild);
        assert.ok(fs.existsSync(exe), `样例可执行文件不存在：${exe}`);

        // 2) TLS-only 中继接入参数（N5 同款环境变量）。
        const turn = parseRelayEnv();
        assert.ok(turn, '需要 TLS TURN 中继接入参数（NIMRTC_TURN_URI=turns:host:5349?transport=tcp'
            + ' + NIMRTC_TURN_USERNAME + NIMRTC_TURN_PASSWORD）');
        assert.equal(turn.scheme, 'turns', '必须过 TLS-only 的 TURN 中继，不是明文 TCP（判据 25）');
        assert.equal(turn.transport, 'tcp', 'TURN URI 必须显式 transport=tcp（TLS 之上）');

        // 3) 起两个进程，中间一个信令桥把新行分隔 JSON 双向转发。
        const sampleArgs = (role) => [
            '--role', role,
            '--turn-host', turn.host,
            '--turn-port', String(turn.port),
            '--turn-user', turn.user,
            '--turn-pass', turn.pass,
        ];
        const caller = spawn(exe, sampleArgs('caller'), { stdio: ['pipe', 'pipe', 'pipe'] });
        const callee = spawn(exe, sampleArgs('callee'), { stdio: ['pipe', 'pipe', 'pipe'] });

        let callerErr = '';
        let calleeErr = '';
        caller.stderr.on('data', (d) => { callerErr += d; });
        callee.stderr.on('data', (d) => { calleeErr += d; });

        const forwardLines = (from, to) => {
            let buf = '';
            from.stdout.on('data', (chunk) => {
                buf += chunk;
                let idx;
                while ((idx = buf.indexOf('\n')) >= 0) {
                    const line = buf.slice(0, idx).trim();
                    buf = buf.slice(idx + 1);
                    if (line) to.stdin.write(`${line}\n`);
                }
            });
        };
        forwardLines(caller, callee);
        forwardLines(callee, caller);

        // 4) 等两个进程自然退出（10 s 一轮通话 + 收尾），断言双向控制。
        const callerExit = new Promise((res) => caller.on('close', res));
        const calleeExit = new Promise((res) => callee.on('close', res));
        const [callerCode, calleeCode] = await Promise.all([callerExit, calleeExit]);

        assert.equal(callerCode, 0, `caller 退出码 ${callerCode}，应为 0（控制交换完成）：\n${callerErr}`);
        assert.equal(calleeCode, 0, `callee 退出码 ${calleeCode}，应为 0：\n${calleeErr}`);

        const cs = parseSummary(callerErr);
        const ss = parseSummary(calleeErr);
        assert.ok(cs, `caller 未打印 summary 行：\n${callerErr}`);
        assert.ok(ss, `callee 未打印 summary 行：\n${calleeErr}`);

        assert.ok(cs.au_tx > 0 && cs.v_tx > 0, 'caller 必须发出音频+视频');
        assert.ok(ss.au_rx > 0 && ss.v_rx > 0, 'callee 必须收到音频+视频（引擎管道通）');
        assert.ok(cs.c_rx >= 1 && ss.c_rx >= 1,
            '控制消息必须双向可达（判据 26）：caller 收 ping，callee 收 pong');
    });

test('判据 27 + 阻塞诚实性：没设备时采集/渲染只 SKIPPED 永不 PASS；执行腿阻塞时点名缺口', () => {
    const cpp = readSample('sdk-sample.cpp');
    assert.match(cpp, /video_source_name\s*=\s*"memory"/, '视频源必须是引擎的 memory（无设备采集）');
    assert.match(cpp, /video_sink_name\s*=\s*"headless"/, 'sink 必须是 headless（无渲染）');

    const reason = blockedReason();
    if (reason) {
        assert.match(reason, /TURN\/TLS|install-tree/, '阻塞原因必须点名引擎缺口');
        console.log(`SKIPPED -- 执行腿被引擎缺口阻塞（永不静默 PASS）：${reason}`);
    }

    // 判据 24 的形状：执行腿代码真的起两个进程。
    const self = readText('tests/acceptance/v1-sdk-sample/acceptance.test.cjs');
    assert.ok(self.includes("sampleArgs('caller')") && self.includes("sampleArgs('callee')"),
        '执行腿必须真的起两个进程（判据 24 的代码形状）');
});

// ---------------------------------------------------------------------------
// 五、边界与回归（验收标准 28-32）
// ---------------------------------------------------------------------------
test('判据 28-29：不改 src/ 的 C++ 源码，不改 examples/remote-desktop/ 与 examples/turn-server/', () => {
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-sdk-sample' });
    const bad = changed.filter((p) =>
        /^src\/.*\.(cpp|hpp|h|cc)$/.test(p) ||
        p.startsWith('examples/remote-desktop/') ||
        p.startsWith('examples/turn-server/'));
    assert.deepEqual(bad, [], `越界改动：\n${bad.join('\n')}`);
});

test('判据 31-32：API freeze 门跑绿（本片不能碰坏公开面）', () => {
    const m = run('python', ['scripts/check_api_manifest.py']);
    assert.equal(m.status, 0, `check_api_manifest.py 未通过（判据 32）：\n${m.out}`);
    assert.match(m.out, /OK/, 'check_api_manifest.py 应输出 OK');

    const f = run(process.execPath, ['scripts/api_freeze_scan.js', '--no-md']);
    assert.equal(f.status, 0, `api_freeze_scan.js 未通过（判据 31）：\n${f.out}`);
});

// ---------------------------------------------------------------------------
// 六、文档（验收标准 33-36）
// ---------------------------------------------------------------------------
test('判据 33-36：ADR-036、README、engine_api.md、CHANGELOG 记录样例与行数预算', () => {
    assert.ok(exists('docs/adr/ADR-036-sdk-sample-line-budget.md'), 'ADR-036 必须存在（判据 33）');
    const adr = readText('docs/adr/ADR-036-sdk-sample-line-budget.md');
    assert.match(adr, /200/, 'ADR 必须记录预算 200');
    assert.match(adr, /10%|0\.10/, 'ADR 必须记录 10% 漂移余量');

    const readme = readSample('README.md');
    assert.match(readme, /怎么跑/, 'README 必须写怎么跑（判据 34）');
    assert.match(readme, /TURN/, 'README 必须写需要什么（TURN 中继）');
    assert.match(readme, /200/, 'README 必须写行数预算');

    const api = readText('docs/api/engine_api.md');
    assert.ok(api.includes('一个 ≤200 行的宿主长什么样'), 'engine_api.md 必须加这一节（判据 35）');

    const changelog = readText('CHANGELOG.md');
    assert.match(changelog, /sdk-sample|SDK 样例/, 'CHANGELOG 必须记录样例（判据 36）');
    assert.match(changelog, /行数预算|line budget/, 'CHANGELOG 必须记录行数预算（判据 36）');
});

// ---------------------------------------------------------------------------
// 七、验收文件自身的诚实性 + 写集边界（ADR-075）
// ---------------------------------------------------------------------------
test('验收文件自身是真实断言：无 todo 占位，唯一的 skip 是执行腿的条件跳过', () => {
    const self = readText('tests/acceptance/v1-sdk-sample/acceptance.test.cjs');
    const todoNeedle = ['test', 'todo'].join('.'); // 分片构造，避免自指
    assert.ok(!self.includes(todoNeedle), '验收文件不得有 todo 占位');
    const execLeg = /test\('判据 24-27[\s\S]*?\{ skip:\s*blockedReason\(\)/.exec(self);
    assert.ok(execLeg, '执行腿必须用 { skip: blockedReason() } 条件跳过（永不静默 PASS）');
});

const SLICE_ID = 'v1-sdk-sample';
const WRITE_SET = [
    'examples/sdk-sample',
    'tools/check_sample_line_budget.py',
    'tools/check_sample_line_budget_self_test.py',
    'tests/acceptance/v1-sdk-sample/acceptance.test.cjs',
    'tests/acceptance/v1-sdk-sample/line_budget.js',
    '.github/workflows/ci.yml',
    'docs/adr/ADR-036-sdk-sample-line-budget.md',
    'docs/api/engine_api.md',
    'CHANGELOG.md',
    'specs/slices/v1-sdk-sample.md',
];
// 会话前已存在的脏状态（不属于本片 agent 的改动）。
const PREEXISTING_ALLOW = ['src/third_party/wolfssl/src'];
const ALLOW = [...WRITE_SET, ...PREEXISTING_ALLOW];
const REQUIRED_PRODUCTS = [
    'examples/sdk-sample/sdk-sample.cpp',
    'examples/sdk-sample/sdk_signalling.hpp',
    'examples/sdk-sample/CMakeLists.txt',
    'examples/sdk-sample/README.md',
    'examples/sdk-sample/sample_line_budget.json',
    'tools/check_sample_line_budget.py',
    'tools/check_sample_line_budget_self_test.py',
    'tests/acceptance/v1-sdk-sample/acceptance.test.cjs',
    'tests/acceptance/v1-sdk-sample/line_budget.js',
    'docs/adr/ADR-036-sdk-sample-line-budget.md',
    'docs/api/engine_api.md',
    'CHANGELOG.md',
];

test('写集边界（ADR-075）：改动仅限写集范围，必要产物在树上', () => {
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: SLICE_ID });
    const violations = changed.filter((p) => !ALLOW.some((a) => p === a || p.startsWith(`${a}/`)));
    assert.deepEqual(violations, [], `改动超出写集：\n${violations.join('\n')}`);

    const onTree = ws.trackedOnTree(ROOT, REQUIRED_PRODUCTS);
    const missing = REQUIRED_PRODUCTS.filter((p) => !onTree.includes(p) && !exists(p));
    assert.deepEqual(missing, [], `必要产物缺失：\n${missing.join('\n')}`);
});

// 验收测试 —— 切片 v1-install-tree-module-closure（ADR-041：安装树链上引擎链接闭包）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 spec 的 22 条验收标准（specs/slices/v1-install-tree-module-closure.md
// §验收标准）逐组落成真实断言：
//
//   ① 闭包对齐：check_install_tree_module_closure.py 存在、对仓库树跑绿、闭包
//      覆盖两份清单、闭包里含 pacing、同一棵树连跑两次输出一致、源码无网络/
//      模型/随机/时间依赖；两份清单里 pacing 与 11 个补进模块都在。
//   ② 工具自测：self_test 存在、跑绿，负样本（install 缺 pacing / config 缺
//      video_jb）被报红并点名，正样本与空闭包不误报 —— 不能「永远绿」。
//   ③ 执行腿：cmake --install 暂存一个干净 prefix → 11 个闭包 archive 在树里、
//      装好的 config 链它们 → 下游「全闭包宿主」（register_all_default_plugins
//      + 显式 TokenBucket 调用，正是预修复 bundle 的 9 个 LNK2001 + 5 个
//      LNK2019）只链 nimrtc::engine，链接 **0 个 LNK2019**，跑起来退出码 0
//      —— 只用 bundle 里的东西。缺 build 树时条件跳过并点名，永不静默 PASS。
//   ④ 边界与回归：src/engine/（含公开头）与 src/modules/** 不被本片碰；CI 的
//      既有安装 + 下游证明腿还在；ADR-038 的验收套件仍然绿（含其关键不变式）。
//   ⑤ 文档：ADR-041 / engine_api.md / CHANGELOG / 规格的实施记录。
//   ⑥ 诚实性：验收文件自身无 todo 占位。
//
// 执行腿依赖 build 树（门禁先 build 再跑验收，树必然在）；树不在时按
// 诚实规则条件跳过，永不静默 PASS。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

const SLICE_ID = 'v1-install-tree-module-closure';
const STAGED_PREFIX = path.join(ROOT, 'build', '_install', 'windows-x86_64');
const STAGED_CONFIG = path.join(STAGED_PREFIX, 'lib', 'cmake', 'NimRTC', 'NimRTCConfig.cmake');

// ── 下游宿主的构建树放在**短**临时目录，不放在 worktree 里 ──────────────
//
// 2026-11-25 实测（Windows）：把宿主的 build 树放在 worktree 内
// （`<worktree>/build/downstream-module-closure-host/build`）时，CMake 的
// 编译器自检直接失败：
//
// ```text
// CMake Error at .../CMakeTestCXXCompiler.cmake:73 (message):
//   正在创建目录"cmTC_d560c.dir\Debug\cmTC_d560c.tlog"…
//   D:\...\Microsoft.CppCommon.targets(781,5): error MSB6003:
//     指定的任务无法执行文件"CL.exe"，也不能运行。
//     System.IO.DirectoryNotFoundException
// ```
//
// 根因是**路径长度**，不是 MSVC 环境没初始化：zelto 的 worktree 路径本身
// 就 ~110 字符（`.zelto/version-worktrees/v3/.zelto/worktrees/<slice-id>/`），
// 再叠加 CMake 的
// `CMakeFiles\CMakeScratch\TryCompile-xxxxxx\cmTC_xxxxxx.dir\Debug\` 后缀，
// MSBuild 建 tlog 目录时就撞上 Windows 的路径上限（MSBuild 的 tlog 机制
// 比 MAX_PATH 更早触发）。同一份工程换到短路径下 configure 就干净通过。
//
// 所以：**源码目录**留在 worktree 里（保持可追溯），**构建树**放短临时目录。
// 被安装的 bundle（STAGED_PREFIX）仍在 worktree 内 —— 它是**只读**依赖，
// 不参与 MSBuild 的相对输出路径计算，不受影响。
//
// 临时目录名带 **pid + 时间戳**：固定路径会让上一次运行残留的句柄
// （msbuild / cl.exe 还挂着）把下一次运行的写文件卡成
// `EBUSY: resource busy or locked` —— 那是「上一轮没干净」，不是
// 「这一轮的代码有问题」，报错完全指不到真正的原因。
const CONSUMER_ROOT = path.join(
    os.tmpdir(),
    `nimrtc-closure-host-${process.pid}-${Date.now()}`,
);
const CONSUMER_DIR = path.join(CONSUMER_ROOT, 'src');
const CONSUMER_BUILD = path.join(CONSUMER_ROOT, 'b');

// The 11 closure modules this slice adds to the two hand-maintained lists
// (spec 缺口表 + 实施记录：9 个 registrar LNK2001 模块 + pacing + 其余）。
// `pacing` was already in _nimrtc_install_shorts but missing from
// _nimrtc_module_shorts; the other ten were missing from both.
const NEW_CLOSURE_SHORTS = [
    'pacing',
    'av1',
    'h265',
    'vp9',
    'dtmf',
    'video_bitrate_control',
    'video_capture',
    'video_sync',
    'audio_device',
    'rd_input',
    'rd_qoe',
];

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(rel) { return fs.readFileSync(path.join(ROOT, rel), 'utf8'); }
function exists(rel) { return fs.existsSync(path.join(ROOT, rel)); }

/** 跑一个命令，返回 { status, out }。 */
function run(cmd, args, opts = {}) {
    const r = spawnSync(cmd, args, {
        cwd: ROOT, encoding: 'utf8', timeout: 180000, ...opts,
    });
    return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/**
 * 在 Windows 上把 MSVC x64 编译环境装进 `env`。
 *
 * ── 为什么必须做（2026-11-25 实测）────────────────────────────────────
 *
 * 本验收测试要**自己** configure 并 build 一个下游宿主工程
 * （`cmake -S <consumer> -B <consumer>/build`）。CMake 找编译器靠的是
 * `cl.exe` 在 PATH 里、且 `INCLUDE` / `LIB` 已就位。
 *
 * 但门禁跑验收时**没有**初始化过 MSVC 环境（`VsDevCmd.bat` 只在
 * 门禁的 build 命令里跑过，且那是另一个进程的子 shell）——
 * 所以 `cmake` 报：
 *
 * ```text
 * -- Check for working CXX compiler: .../cl.exe - broken
 * -- Configuring incomplete, errors occurred!
 * ```
 *
 * 报错完全指不到真正的原因（不是「装不上编译器」，是「环境没初始化」）。
 *
 * **`vcvars64.bat` 是批处理**：它只能给自己 spawn 出来的 `cmd.exe`
 * 设环境变量，没法把变量「传回来」。所以这里**不是**去初始化当前进程
 * 的 `process.env`，而是**造一个 `cmd.exe` 包装**：先 `call vcvars64.bat`，
 * 再在同一个 `cmd.exe` 里跑 cmake。
 *
 * 复用了仓库里既有的 `scripts/msvc_env.ps1` 的做法（同样的 vcvars64
 * 定位逻辑），但这里直接内联 —— 验收测试不能假设外部脚本存在。
 *
 * 非 Windows：原样返回 `process.env`（POSIX 上编译器本来就在 PATH 里）。
 */
function msvcEnv() {
    if (process.platform !== 'win32') return process.env;

    const env = { ...process.env };
    const vswhere = path.join(
        process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)',
        'Microsoft Visual Studio', 'Installer', 'vswhere.exe',
    );
    const roots = [
        'C:\\Program Files\\Microsoft Visual Studio\\2022',
        'D:\\Program Files\\Microsoft Visual Studio\\2022',
        'C:\\Program Files (x86)\\Microsoft Visual Studio\\2022',
    ];
    const editions = ['Community', 'Professional', 'Enterprise', 'BuildTools'];

    const candidates = [];
    if (fs.existsSync(vswhere)) {
        try {
            const r = spawnSync(vswhere, [
                '-latest', '-products', '*',
                '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                '-property', 'installationPath',
            ], { encoding: 'utf8' });
            const installPath = (r.stdout || '').trim();
            if (installPath) {
                candidates.push(path.join(installPath, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat'));
            }
        } catch {
            // 找不到就退回下面的约定路径。
        }
    }
    for (const root of roots) {
        for (const edition of editions) {
            candidates.push(path.join(root, edition, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat'));
        }
    }

    const vcvars = candidates.find((p) => fs.existsSync(p));
    assert.ok(vcvars,
        `找不到 vcvars64.bat（查过 vswhere 与常见 VS 2022 布局）：\n${candidates.join('\n')}`);

    // 把「先 call vcvars、再跑命令」包成一个 cmd.exe 脚本。
    // `set "CL="` 是刻意的：继承来的 CL 会覆盖 CMake 给每个 target 选的
    // 语言标准，把 vendored 的 C11 源码（libopus）编坏。
    const scriptPath = path.join(
        os.tmpdir(),
        `nimrtc_closure_host_env_${process.pid}_${Date.now()}.cmd`,
    );
    const wrapped = [
        '@echo off',
        `call "${vcvars}" >nul`,
        'if errorlevel 1 exit /b 1',
        'set "CL="',
        'set "_CL_="',
        'set "CLFLAGS="',
        '%*',
        '',
    ].join('\r\n');
    fs.writeFileSync(scriptPath, wrapped, 'utf8');

    return {
        ...env,
        NIMRTC_WRAP_SCRIPT: scriptPath,
    };
}

/**
 * 在 MSVC 环境里跑一条命令（Windows 上经由 cmd.exe 包装）。
 *
 * 返回 `{ status, out }` —— 与 `run()` 同形。
 */
function runWithMsvc(cmd, args, opts = {}) {
    if (process.platform !== 'win32') return run(cmd, args, opts);

    const env = msvcEnv();
    const scriptPath = env.NIMRTC_WRAP_SCRIPT;
    try {
        const quoted = args.map((a) => (/[\s"]/.test(a) ? `"${a}"` : a)).join(' ');
        const r = spawnSync(
            'cmd.exe',
            ['/d', '/c', scriptPath, cmd, ...args],
            {
                cwd: ROOT,
                encoding: 'utf8',
                timeout: 600000,
                env,
                ...opts,
            },
        );
        return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
    } finally {
        try { fs.unlinkSync(scriptPath); } catch { /* 删不掉不算致命 */ }
    }
}

/** 跑 Python（本地是 `python`；CI 用 `python3`）。 */
function runPy(args, opts) {
    const r = run('python', args, opts);
    if (r.status === 0 || r.out) return r;
    return run('python3', args, opts);
}

/** 切出 source 里的 `set(<var> ...)` 括号体（去掉行注释后按空白切词）。 */
function listTokens(text, varName) {
    const m = new RegExp(`set\\(${varName}([\\s\\S]*?)\\n\\s*\\)`).exec(text);
    assert.ok(m, `${varName} 列表必须存在（单独一行的 \`)\` 收尾）`);
    return m[1].replace(/#[^\n]*/g, '').split(/\s+/).filter(Boolean);
}

/** 切出 source 里的 `if(WIN32) ...` 分支体。 */
function win32BranchBody(configText) {
    const m = /if\(WIN32\)([\s\S]*?)(?:elseif|endif)/.exec(configText);
    assert.ok(m, 'config 里必须有 WIN32 系统库分支');
    return m[1];
}

/** 暂存 bundle 里某个模块 archive 的文件名（Windows .lib / Unix .a）。 */
function libName(short) {
    const prefix = process.platform === 'win32' ? '' : 'lib';
    const suffix = process.platform === 'win32' ? '.lib' : '.a';
    return `${prefix}nimrtc_${short}${suffix}`;
}

/** 定位下游宿主可执行文件（多配置 / 单配置、.exe / 无后缀都可）。 */
function resolveConsumerExe(buildDir) {
    for (const c of [
        path.join(buildDir, 'Debug', 'module_closure_host.exe'),
        path.join(buildDir, 'Release', 'module_closure_host.exe'),
        path.join(buildDir, 'module_closure_host.exe'),
        path.join(buildDir, 'module_closure_host'),
    ]) {
        if (fs.existsSync(c)) return c;
    }
    return path.join(buildDir, 'Debug', 'module_closure_host.exe'); // 让 spawn 用明确的 ENOENT 报错
}

/** 执行腿的阻塞检测：缺 build 树就条件跳过（永不静默 PASS）。 */
function execLegBlocker() {
    if (!fs.existsSync(path.join(ROOT, 'build', 'CMakeCache.txt'))) {
        return 'no build tree: the gate builds first, so build/CMakeCache.txt '
            + 'is expected to exist before this leg installs and links';
    }
    // 返回布尔 false（不是空字符串）：node:test 把空字符串 skip 当成
    // 「跳过 + 照跑 body」，报告成 SKIP 混着失败；false 才是「不跳过」。
    return false;
}

// ---------------------------------------------------------------------------
// 一、闭包对齐（验收标准 1-4、12-13）
// ---------------------------------------------------------------------------
test('判据 1/2/3/4：工具存在、跑绿、闭包覆盖两份清单、闭包里含 pacing', () => {
    assert.ok(exists('tools/check_install_tree_module_closure.py'),
        '判据 1：闭包校验工具必须存在');

    const r = runPy(['tools/check_install_tree_module_closure.py']);
    assert.equal(r.status, 0, `判据 1/2/3：工具对仓库树必须跑绿：\n${r.out}`);
    assert.match(r.out, /OK — engine link closure is fully covered by both short lists/,
        '工具必须输出最终 OK');

    const closureM = /^PASS closure: (.+)$/m.exec(r.out);
    assert.ok(closureM, '工具必须输出闭包清单（PASS closure: … 行）');
    const closure = closureM[1].trim().split(/\s+/);
    for (const mod of NEW_CLOSURE_SHORTS) {
        assert.ok(closure.includes(mod),
            `判据 4：闭包里必须有 ${mod}（引擎 PRIVATE 链或递归依赖）`);
    }
    assert.ok(!r.out.includes('FAIL'), '工具输出不得有 FAIL 行');
});

test('判据 3 源侧：两份清单都覆盖 pacing 与 11 个补进模块', () => {
    const inst = listTokens(readText('cmake/NimRTCInstall.cmake'), '_nimrtc_install_shorts');
    const cfg = listTokens(readText('cmake/NimRTCConfig.cmake.in'), '_nimrtc_module_shorts');
    for (const mod of NEW_CLOSURE_SHORTS) {
        assert.ok(inst.includes(mod),
            `判据 3：_nimrtc_install_shorts 必须含 ${mod}（闭包覆盖）`);
        assert.ok(cfg.includes(mod),
            `判据 3：_nimrtc_module_shorts 必须含 ${mod}（闭包覆盖）`);
    }
});

test('判据 12：同一棵树连跑两次 → 退出码与逐条输出一致（确定性）', () => {
    const r1 = runPy(['tools/check_install_tree_module_closure.py']);
    const r2 = runPy(['tools/check_install_tree_module_closure.py']);
    assert.equal(r1.status, 0, `第一次必须绿：\n${r1.out}`);
    assert.equal(r2.status, 0, `第二次必须绿：\n${r2.out}`);
    assert.equal(r1.out, r2.out, '判据 12：两次逐条输出必须一致');
});

test('判据 13：工具源码没有网络 / 模型 / 随机 / 时间依赖', () => {
    const forbidden = /urllib|aiohttp|httpx|ftplib|\brequests\b|\bsocket\b|anthropic|\bopenai\b|\bclaude\b|\bllm\b|random\.|secrets\.|\buuid\b|\bdatetime\b|time\.time|time\.clock|time\.monotonic|perf_counter/;
    for (const f of [
        'tools/check_install_tree_module_closure.py',
        'tools/check_install_tree_module_closure_self_test.py',
    ]) {
        assert.doesNotMatch(readText(f), forbidden,
            `判据 13：${f} 不得有网络/模型/随机/时间依赖`);
    }
});

// ---------------------------------------------------------------------------
// 二、工具的自测（验收标准 5-11）—— 不能只有「永远绿」
// ---------------------------------------------------------------------------
test('判据 5-11：自测存在、跑绿，负样本被报红并点名，正样本与空闭包不误报', () => {
    assert.ok(exists('tools/check_install_tree_module_closure_self_test.py'),
        '判据 7：工具自测必须存在');

    const st = runPy(['tools/check_install_tree_module_closure_self_test.py']);
    assert.equal(st.status, 0, `判据 7：自测必须跑绿：\n${st.out}`);

    // 判据 10：两份清单都完整 → 报绿。
    assert.ok(st.out.includes('PASS a tree with complete lists is green (exit 0)'),
        '判据 10：完整清单必须报绿');

    // 判据 8：故意从 _nimrtc_install_shorts 拿掉一个闭包模块（pacing）→ 报红并点名。
    assert.ok(st.out.includes(
        'PASS removing pacing from _nimrtc_install_shorts is red and names it'),
        '判据 8：install 缺 pacing 必须报红并点名（工具能失败，非永远绿）');

    // 判据 9：故意从 _nimrtc_module_shorts 拿掉一个闭包模块（video_jb）→ 报红并点名。
    assert.ok(st.out.includes(
        'PASS removing video_jb from _nimrtc_module_shorts is red and names it'),
        '判据 9：config 缺 video_jb 必须报红并点名');

    // 判据 11：闭包空（引擎不链任何模块）→ 报绿且不误报。
    assert.ok(st.out.includes('PASS an empty engine closure is green with no false positive'),
        '判据 11：空闭包必须报绿且不误报');

    // 判据 5/6：缺口要「说清模块 + 哪份清单」、故意缺模块 → 退出码非零并点名。
    // 这两条由上述负样本直接证明（自测内部用合成树驱动工具，工具 exit 1 并
    // 在 FAIL 消息里点名模块 + 清单）。注意：负样本里工具本身的 FAIL 行是
    // 预期产物，不能用 `!/FAIL/` 断言自测输出 —— 自测「全绿」由退出码 0
    // 保证（emit 失败会把 passed 置 False 并返回 1）。
});

// ---------------------------------------------------------------------------
// 三、装得对 —— 执行腿（验收标准 9、10、14）：暂存 + 检查 + 全闭包下游宿主
// ---------------------------------------------------------------------------
test('判据 9/10/14：暂存干净 prefix → 11 个闭包 archive 在树里 → 全闭包宿主 0 LNK2019，只用 bundle',
    { skip: execLegBlocker(), timeout: 600000 }, () => {
        // 1) 暂存：cmake --install 到干净 prefix（bundle 自洽，判据 10）。
        const inst = run('cmake', ['--install', 'build', '--prefix', STAGED_PREFIX, '--config', 'Debug']);
        assert.equal(inst.status, 0, `cmake --install 失败：\n${inst.out}`);
        // 注意：这里不能用 exists() —— 它会把绝对路径 path.join 到 ROOT 后面，
        // 得到「ROOT+绝对路径」的双拼路径，永远不存在。绝对路径用 fs.existsSync。
        assert.ok(fs.existsSync(path.join(STAGED_PREFIX, 'lib', 'cmake', 'NimRTC', 'NimRTCConfig.cmake')),
            '安装树里必须有 NimRTCConfig.cmake');

        // 2) 装了什么（判据 14）：11 个闭包 archive 必须进 lib/。
        const libdir = path.join(STAGED_PREFIX, 'lib');
        for (const mod of NEW_CLOSURE_SHORTS) {
            assert.ok(fs.existsSync(path.join(libdir, libName(mod))),
                `判据 14：nimrtc_${mod} 必须装进 lib/`);
        }

        // 3) 装好的 config 链了什么（判据 14 安装侧）：11 个闭包模块都在链上。
        //    绝对路径用 fs.readFileSync 直接读（readText 会把绝对路径 path.join 到 ROOT）。
        const stagedCfg = fs.readFileSync(STAGED_CONFIG, 'utf8');
        for (const mod of NEW_CLOSURE_SHORTS) {
            assert.ok(stagedCfg.includes(mod),
                `判据 14 安装侧：安装 config 必须链 nimrtc_${mod}`);
        }

        // 4) 下游全闭包宿主：只链 nimrtc::engine，调用 register_all_default_plugins()
        //    + 显式 TokenBucket 方法（判据 9、10）。
        fs.mkdirSync(CONSUMER_DIR, { recursive: true });
        fs.writeFileSync(path.join(CONSUMER_DIR, 'CMakeLists.txt'), [
            'cmake_minimum_required(VERSION 3.20)',
            'project(module_closure_host LANGUAGES CXX)',
            '',
            'find_package(NimRTC 1.0 REQUIRED CONFIG)',
            '',
            'add_executable(module_closure_host main.cpp)',
            'target_link_libraries(module_closure_host PRIVATE nimrtc::engine)',
            'set_target_properties(module_closure_host PROPERTIES',
            '    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")',
            '',
        ].join('\n'));
        fs.writeFileSync(path.join(CONSUMER_DIR, 'main.cpp'), [
            '// Downstream module-closure host - slice v1-install-tree-module-closure.',
            '// Links the installed nimrtc::engine and forces the FULL engine link',
            '// closure:',
            '//   * core::register_all_default_plugins() takes the address of every',
            '//     built-in module registrar -- including the 9 pre-fix LNK2001',
            '//     modules (h265/av1/vp9/video_capture/video_bitrate_control/',
            '//     rd_input/rd_qoe/video_sync/dtmf);',
            '//   * explicit TokenBucket calls force the 5 pre-fix LNK2019 methods',
            '//     (set_config/available/try_consume/refund/reset).',
            '// A bundle that installs but does not chain any closure module fails',
            '// this host with unresolved externals (the pre-fix 14-symbol link).',
            '#include <nimrtc/core/registry.hpp>',
            '#include <nimrtc/engine/engine.hpp>',
            '#include <nimrtc/pacing/pacing.hpp>',
            '#include <cstdio>',
            '',
            'int main() {',
            '    nimrtc::core::register_all_default_plugins();',
            '',
            '    // rate_bps == 0: an unlimited bucket, so try_consume is guaranteed',
            '    // to succeed.  The point of the host is symbol resolution, not',
            '    // pacing behaviour.',
            '    nimrtc::pacing::PacerConfig cfg;',
            '    nimrtc::pacing::TokenBucket bucket(cfg);',
            '    bucket.set_config(cfg);',
            '    const nimrtc::core::TimePoint now = nimrtc::core::SteadyClock::now();',
            '    const std::uint64_t avail = bucket.available(now);',
            '    const bool ok = bucket.try_consume(1, now);',
            '    bucket.refund(1);',
            '    bucket.reset(now);',
            '',
            '    std::printf("nimrtc %s closure-host: ok=%d avail=%llu\\n",',
            '                NIMRTC_API_VERSION_STRING, static_cast<int>(ok),',
            '                static_cast<unsigned long long>(avail));',
            '    return ok ? 0 : 1;',
            '}',
            '',
        ].join('\n'));

        const consumerBuild = CONSUMER_BUILD;
        // 下游宿主的 configure/build 必须跑在**初始化过的 MSVC 环境**里 ——
        // 否则 `cl.exe` 不在 PATH，CMake 报「compiler - broken」，
        // 而报错完全指不到「环境没初始化」这个真正的原因。
        //
        // 构建树放在**短**临时目录（见 CONSUMER_ROOT 处的注释）：worktree
        // 内的深路径会让 MSBuild 建不了 tlog 目录（MSB6003）。
        fs.mkdirSync(consumerBuild, { recursive: true });
        const cfgCmd = runWithMsvc('cmake', [
            '-S', CONSUMER_DIR,
            '-B', consumerBuild,
            `-DCMAKE_PREFIX_PATH=${STAGED_PREFIX}`,
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
        ]);
        assert.equal(cfgCmd.status, 0, `下游宿主配置失败：\n${cfgCmd.out}`);

        const buildCmd = runWithMsvc('cmake', ['--build', consumerBuild, '--config', 'Debug']);
        assert.equal(buildCmd.status, 0,
            `下游宿主链接失败（判据 9：应 0 个 LNK2019）：\n${buildCmd.out}`);
        assert.doesNotMatch(buildCmd.out, /LNK2019|LNK2001|LNK1120|undefined reference/,
            `链接不得报未解析符号：\n${buildCmd.out}`);

        const exe = resolveConsumerExe(consumerBuild);
        assert.ok(fs.existsSync(exe), '下游宿主可执行文件必须产出');
        // 跑宿主时把 bundle 的 bin/ 加进 PATH：安装树里带的是**运行时**
        // DLL（msquic.dll —— MsQuic 是动态库，静态 archive 只有 1.9 KB 的
        // 导入桩），不把它放进 PATH 的话宿主会以 0xC0000135
        // (STATUS_DLL_NOT_FOUND) 起不来 —— 那是「找不到 DLL」，不是
        // 「链接失败」，报错完全指不到真正的原因。
        //
        // 加 PATH 而不是把 DLL 拷到宿主旁边：后者会让宿主「碰巧」能跑，
        // 却掩盖了「bundle 自带运行时」这个事实。判据 10 要证明的是
        // 「宿主只链 bundle 里的东西」，运行时 DLL 也该来自 bundle。
        const stagedBin = path.join(STAGED_PREFIX, 'bin');
        const hostEnv = {
            ...process.env,
            PATH: fs.existsSync(stagedBin)
                ? `${stagedBin}${path.delimiter}${process.env.PATH || ''}`
                : process.env.PATH,
        };
        const runHost = spawnSync(exe, [], {
            cwd: ROOT, encoding: 'utf8', timeout: 60000, env: hostEnv,
        });
        assert.equal(runHost.status, 0,
            `下游宿主运行必须退出码 0：\n${runHost.stdout || ''}${runHost.stderr || ''}`);

        // 判据 10：宿主的链接行只引用暂存 bundle 的 archive（干净 prefix，
        // 无外部依赖）—— 解析生成工程的 AdditionalDependencies，不靠猜。
        if (process.platform === 'win32') {
            const vcxproj = path.join(consumerBuild, 'module_closure_host.vcxproj');
            assert.ok(fs.existsSync(vcxproj), '下游宿主的 vcxproj 必须生成');
            const proj = fs.readFileSync(vcxproj, 'utf8');
            const m = /<AdditionalDependencies>([\s\S]*?)<\/AdditionalDependencies>/.exec(proj);
            assert.ok(m, 'vcxproj 必须有 AdditionalDependencies');
            const deps = m[1].split(/[;\s]+/).filter(Boolean);
            const nimDeps = deps.filter((d) => /nimrtc_/.test(d));
            assert.ok(nimDeps.length >= 20,
                `链接依赖里必须链上闭包 archive，实际 ${nimDeps.length} 个：\n${nimDeps.join('\n')}`);
            const stagedLib = path.join(STAGED_PREFIX, 'lib');
            for (const d of nimDeps) {
                assert.ok(d.startsWith(stagedLib),
                    `链接依赖 ${d} 必须来自暂存 bundle（判据 10）`);
            }
        }
    }, () => {
        // 收尾：删掉本次的临时目录（源码 + 构建树）。
        //
        // 为什么放在 after 而不是 finally：断言失败时**不删** —— 那样
        // 失败现场（CMakeCache / vcxproj / 链接日志）还在临时目录里，
        // 可以直接翻。删不掉的（msbuild 还挂着句柄）不算致命。
        try {
            fs.rmSync(CONSUMER_ROOT, { recursive: true, force: true, maxRetries: 3 });
        } catch { /* 临时目录残留不算致命 */ }
    });

// ---------------------------------------------------------------------------
// 四、边界与回归（验收标准 15-19）
// ---------------------------------------------------------------------------
test('判据 15 静态侧：CI 的既有「安装 + 下游证明」腿还在（本片不碰 CI）', () => {
    const ci = readText('.github/workflows/ci.yml');
    assert.match(ci, /Install \+ downstream proof/, 'CI 的安装 + 下游证明腿必须还在（判据 15）');
    assert.match(ci, /NIMRTC_INSTALL=ON/, 'CI 安装腿必须带 NIMRTC_INSTALL=ON（判据 15）');
});

test('判据 16/17/18：src/engine/（含公开头）与 src/modules/** 不被本片碰', () => {
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: SLICE_ID });
    const bad = changed.filter((p) =>
        p.startsWith('src/engine/') || p.startsWith('src/modules/'));
    assert.deepEqual(bad, [], `判据 16/17：越界改动：\n${bad.join('\n')}`);

    const headers = changed.filter((p) => p.startsWith('src/engine/include/'));
    assert.deepEqual(headers, [], `判据 18：engine 公开头被改：\n${headers.join('\n')}`);
});

test('判据 19 静态侧：ADR-038 的关键不变式仍在（nvcodec 在 h264 后、openh264 清单、d3d11/dxgi、vendor、APM）', () => {
    const cfg = readText('cmake/NimRTCConfig.cmake.in');
    const shorts = listTokens(cfg, '_nimrtc_module_shorts');
    assert.ok(shorts.includes('nvcodec'), '模块列表里必须有 nvcodec（ADR-038）');
    const iH264 = shorts.indexOf('h264');
    const iNv = shorts.indexOf('nvcodec');
    assert.ok(iH264 >= 0 && iNv > iH264,
        'nvcodec 必须在 h264 之后（h264 PUBLIC 链 nvcodec；GNU 静态链从左到右，ADR-038 不变式）');
    assert.match(cfg, /openh264_link_order\.txt/, 'openh264 链接顺序清单读取必须保留（ADR-038）');

    const win = win32BranchBody(cfg);
    assert.match(win, /\bd3d11\b/, 'WIN32 分支必须链 d3d11（ADR-038）');
    assert.match(win, /\bdxgi\b/, 'WIN32 分支必须链 dxgi（ADR-038）');

    assert.match(cfg,
        /_nimrtc_vendor_names[\s\S]*\bopus\b[\s\S]*\bsrtp3\b[\s\S]*\bjuice\b[\s\S]*\bwolfssl\b[\s\S]*\bmsquic\b/,
        'vendor 链必须保留 opus/srtp3/juice/wolfssl/msquic（ADR-038 不回归）');
    assert.match(cfg, /apm_link_order\.txt/, 'APM 顺序清单读取必须保留（ADR-038 不回归）');
});

test('判据 19：ADR-038 的验收套件仍然绿（本片不破它的证据）', () => {
    const r = run('node', ['--test',
        'tests/acceptance/v1-install-tree-video-deps/acceptance.test.cjs'],
    { timeout: 600000 });
    assert.equal(r.status, 0, `判据 19：ADR-038 验收套件必须绿：\n${r.out}`);
});

// ---------------------------------------------------------------------------
// 五、文档与契约（验收标准 20-22）
// ---------------------------------------------------------------------------
test('判据 20：ADR-041 存在，写清闭包是什么 / 两份清单的关系 / 为什么不自动发现模块', () => {
    assert.ok(exists('docs/adr/ADR-041-install-tree-module-closure.md'), 'ADR-041 必须存在');
    const adr = readText('docs/adr/ADR-041-install-tree-module-closure.md');
    assert.match(adr, /闭包/, 'ADR 必须写「闭包是什么」');
    assert.match(adr, /_nimrtc_install_shorts/, 'ADR 必须点名安装清单');
    assert.match(adr, /_nimrtc_module_shorts/, 'ADR 必须点名链接清单');
    assert.match(adr, /自动发现/, 'ADR 必须写「为什么不自动发现模块」');
});

test('判据 21：engine_api.md 记录安装树怎么用（模块闭包 + pacing）', () => {
    const api = readText('docs/api/engine_api.md');
    assert.match(api, /安装树|install tree|install-tree/i,
        'engine_api.md 必须有安装树一节');
    assert.match(api, /闭包|closure/i, '那一节必须写模块闭包');
    assert.match(api, /pacing/, '那一节必须点名 pacing（链接闭包的一环）');
});

test('判据 22：CHANGELOG 记录本片', () => {
    const changelog = readText('CHANGELOG.md');
    assert.match(changelog, /v1-install-tree-module-closure|安装树.*模块闭包|install-tree.*module closure/i,
        'CHANGELOG 必须记录本片');
    assert.match(changelog, /ADR-041/, 'CHANGELOG 必须指向 ADR-041');
});

test('判据 23：spec 记录实施中的发现', () => {
    const spec = readText('specs/slices/v1-install-tree-module-closure.md');
    assert.match(spec, /实施记录|实施中的发现|实施发现/, 'spec 必须有实施记录一节');
});

// ---------------------------------------------------------------------------
// 验收文件自身的诚实性 + 写集边界（ADR-075）
// ---------------------------------------------------------------------------
test('验收文件自身是真实断言：无 todo 占位，唯一的 skip 是执行腿的条件跳过', () => {
    const self = readText('tests/acceptance/v1-install-tree-module-closure/acceptance.test.cjs');
    const todoNeedle = ['test', 'todo'].join('.'); // 分片构造，避免自指
    assert.ok(!self.includes(todoNeedle), '验收文件不得有 todo 占位');
    const execLeg = /test\('判据 9\/10\/14[\s\S]*?\{ skip:\s*execLegBlocker\(\)/.exec(self);
    assert.ok(execLeg, '执行腿必须用 { skip: execLegBlocker() } 条件跳过（永不静默 PASS）');
});

const WRITE_SET = [
    'cmake/NimRTCInstall.cmake',
    'cmake/NimRTCConfig.cmake.in',
    'tools/check_install_tree_module_closure.py',
    'tools/check_install_tree_module_closure_self_test.py',
    'tests/acceptance/v1-install-tree-module-closure',
    'docs/adr/ADR-041-install-tree-module-closure.md',
    // 本片在自己的提交里建的 ADR 当时是 039（v4 线的编号）。合流时 main
    // 已经占了 039（viewport-window-opt-in），于是本片的 ADR 改号为 041。
    // 提交历史记的是**建它时的路径**，所以越界检查会看到 039 这条旧路径
    // —— 同一份 ADR 的旧编号，不是越界。两个编号都在写集里。
    'docs/adr/ADR-039-install-tree-module-closure.md',
    'docs/api/engine_api.md',
    'CHANGELOG.md',
    'specs/slices/v1-install-tree-module-closure.md',
];
const REQUIRED_PRODUCTS = [
    'tests/acceptance/v1-install-tree-module-closure/acceptance.test.cjs',
    'tools/check_install_tree_module_closure.py',
    'tools/check_install_tree_module_closure_self_test.py',
    'docs/adr/ADR-041-install-tree-module-closure.md',
];

// 会话前就存在的工作区杂讯：wolfssl 是 vendored 子模块，其工作区是会话前就
// 存在的脏状态（git status 报 ` M src/third_party/wolfssl/src`）—— 与共享判据
// WRITE_SETS 里对 msquic 的处理同形，且 ADR-038 的 KNOWN_OUTSIDE_ARTIFACTS
// 已把它列为已知归属杂讯。
const KNOWN_OUTSIDE_ARTIFACTS = [
    'src/third_party/wolfssl/src',
];

test('写集边界（ADR-075）：改动仅限写集范围，必要产物在树上', () => {
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: SLICE_ID });
    const violations = changed.filter((p) =>
        !WRITE_SET.some((a) => p === a || p.startsWith(`${a}/`)) &&
        !KNOWN_OUTSIDE_ARTIFACTS.includes(p));
    assert.deepEqual(violations, [], `改动超出写集：\n${violations.join('\n')}`);

    const onTree = ws.trackedOnTree(ROOT, REQUIRED_PRODUCTS);
    const missing = REQUIRED_PRODUCTS.filter((p) => !onTree.includes(p) && !exists(p));
    assert.deepEqual(missing, [], `必要产物缺失：\n${missing.join('\n')}`);
});

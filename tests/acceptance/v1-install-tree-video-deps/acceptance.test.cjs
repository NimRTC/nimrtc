// 验收测试 —— 切片 v1-install-tree-video-deps（ADR-038：安装树补上视频依赖）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 spec 的 23 条验收标准（specs/slices/v1-install-tree-video-deps.md
// §验收标准）逐组落成真实断言：
//
//   ① 静态：暂存的 cmake 源（config / install / openh264 wrapper）必须
//      链 nvcodec、按 manifest 读 openh264 顺序、Windows 链 d3d11/dxgi；
//      音频 / 基础路径的既有链不回归；CI 的既有安装腿还在。
//   ② 工具：check_install_tree_video_deps.py 存在、对暂存树跑绿、自检
//      的负样本被报出（不能「永远绿」）、同一工具再跑一次仍绿。
//   ③ 执行腿：cmake --install 暂存一个干净 prefix → 树里有 nvcodec +
//      三个 openh264 archive + 顺序清单 → 下游「视频宿主」（h264 +
//      headless sink）只链 nimrtc::engine，链接 **0 个 LNK2019**，跑起来
//      退出码 0 —— 只用了 bundle 里的东西。缺 build 树时条件跳过并点名，
//      永不静默 PASS。
//   ④ 边界：src/engine/（含公开头）与 src/modules/ice/ 不被本片碰；
//      src/ 下没有 C++ 源码改动；改动清单不越写集、必要产物在树上。
//   ⑤ 文档：ADR-038 / engine_api.md / CHANGELOG / 规格的实施记录。
//   ⑥ 诚实性：验收文件自身无 todo 占位。
//
// 执行腿依赖 build 树（门禁先 build 再跑验收，树必然在）；树不在时按
// 诚实规则条件跳过，永不静默 PASS。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

const SLICE_ID = 'v1-install-tree-video-deps';
const STAGED_PREFIX = path.join(ROOT, 'build', '_install', 'windows-x86_64');
const STAGED_CONFIG = path.join(STAGED_PREFIX, 'lib', 'cmake', 'NimRTC', 'NimRTCConfig.cmake');
const CONSUMER_DIR = path.join(ROOT, 'build', 'downstream-video-host');

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

/** 跑 Python（本地是 `python`；CI 用 `python3`）。 */
function runPy(args, opts) {
    const r = run('python', args, opts);
    if (r.status === 0 || r.out) return r;
    return run('python3', args, opts);
}

/** 切出 source 里的 `set(_nimrtc_module_shorts ...)` 括号体。 */
function moduleShortsBody(configText) {
    const m = /set\(_nimrtc_module_shorts([\s\S]*?)\n\s*\)/.exec(configText);
    assert.ok(m, '_nimrtc_module_shorts 列表必须在（单独一行的 `)` 收尾）');
    return m[1];
}

/** 切出 source 里的 `if(WIN32) ...` 分支体。 */
function win32BranchBody(configText) {
    const m = /if\(WIN32\)([\s\S]*?)(?:elseif|endif)/.exec(configText);
    assert.ok(m, 'config 里必须有 WIN32 系统库分支');
    return m[1];
}

/** 定位下游宿主可执行文件（多配置 / 单配置、.exe / 无后缀都可）。 */function resolveConsumerExe(buildDir) {
    for (const c of [
        path.join(buildDir, 'Debug', 'video_host.exe'),
        path.join(buildDir, 'Release', 'video_host.exe'),
        path.join(buildDir, 'video_host.exe'),
        path.join(buildDir, 'video_host'),
    ]) {
        if (fs.existsSync(c)) return c;
    }
    return path.join(buildDir, 'Debug', 'video_host.exe'); // 让 spawn 用明确的 ENOENT 报错
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
// 二、链了什么 —— 暂存 cmake 源（验收标准 4-8）+ 音频/基础不回归（11 的静态侧）
// ---------------------------------------------------------------------------
test('判据 4/5/6：config 源链 nvcodec（在 h264 之后）、按清单读 openh264、Windows 链 d3d11+dxgi', () => {
    const cfg = readText('cmake/NimRTCConfig.cmake.in');

    const body = moduleShortsBody(cfg);
    const shorts = body.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
    assert.ok(shorts.includes('nvcodec'), '模块列表里必须有 nvcodec（判据 4）');
    const iH264 = shorts.indexOf('h264');
    const iNv = shorts.indexOf('nvcodec');
    assert.ok(iH264 >= 0 && iNv > iH264,
        'nvcodec 必须在 h264 之后（h264 PUBLIC 链 nvcodec；GNU 静态链从左到右，判据 4）');

    assert.match(cfg, /openh264_link_order\.txt/,
        'config 必须读 openh264 的链接顺序清单（判据 5/6）');

    const win = win32BranchBody(cfg);
    assert.match(win, /d3d11/, 'WIN32 分支必须链 d3d11（判据 7）');
    assert.match(win, /dxgi/, 'WIN32 分支必须链 dxgi（判据 7）');
});

test('判据 8：nvcodec 引用的 NVIDIA import lib —— 实为 LoadLibrary 动态加载，bundle 不需要 nvidia-encode.lib', () => {
    // 设计 §四：核实 NVIDIA import lib（nvidia-encode / nvidia-video）要不要进
    // bundle。实现时的核实结论是「不要」：nvenc/nvdec 用 LoadLibrary +
    // GetProcAddress（src/nvenc_encoder.cpp / nvdec_decoder.cpp 的 dynlink 路径），
    // 没有对 NVIDIA import lib 的链接期引用；只有 d3d11 是 `#pragma comment(lib,
    // "d3d11.lib")`（判据 7 已覆盖）。
    const nvenc = readText('src/modules/nvcodec/src/nvenc_encoder.cpp');
    assert.match(nvenc, /LoadLibrary/, 'NVENC 必须走 LoadLibrary（动态加载）');
    assert.doesNotMatch(nvenc, /#pragma comment\(lib, "nvidia-encode/, '不得链接期引用 nvidia-encode.lib');
    const nvdec = readText('src/modules/nvcodec/src/nvdec_decoder.cpp');
    // NVDEC 不直接写 LoadLibrary：它 include vendored ffnvcodec 的 dynlink_loader.h
    // （cuda_load_functions / cuvid_load_functions 在运行时 LoadLibrary + GetProcAddress）。
    assert.match(nvdec, /dynlink_loader\.h|LoadLibrary/, 'NVDEC 必须走 dynlink 加载（dynlink_loader.h）');
    assert.doesNotMatch(nvdec, /#pragma comment\(lib, "nvidia-video/, '不得链接期引用 nvidia-video.lib');
});

test('判据 11 静态侧：音频 / 基础路径的既有链不回归（系统库、vendor 名、APM 清单、engine 平铺链都在）', () => {
    const cfg = readText('cmake/NimRTCConfig.cmake.in');
    const win = win32BranchBody(cfg);
    for (const lib of ['ws2_32', 'bcrypt', 'iphlpapi', 'winmm', 'crypt32']) {
        assert.match(win, new RegExp(`\\b${lib}\\b`), `WIN32 分支必须保留 ${lib}（不回归）`);
    }
    assert.match(cfg, /_nimrtc_vendor_names[\s\S]*\bopus\b[\s\S]*\bsrtp3\b[\s\S]*\bjuice\b[\s\S]*\bwolfssl\b[\s\S]*\bmsquic\b/,
        'vendor 链必须保留 opus/srtp3/juice/wolfssl/msquic（不回归）');
    assert.match(cfg, /apm_link_order\.txt/, 'APM 顺序清单读取必须保留（不回归）');
    assert.match(cfg, /nimrtc::engine/, 'engine 平铺链目标必须保留（不回归）');
});

test('判据 19 静态侧：CI 的既有「安装 + 下游证明」腿还在，安装腿带 OPENH264 构建', () => {
    const ci = readText('.github/workflows/ci.yml');
    assert.match(ci, /Install \+ downstream proof/, 'macOS 的 Install + downstream proof 腿必须还在');
    assert.match(ci, /NIMRTC_INSTALL=ON[\s\S]{0,400}?NIMRTC_PLUGINS_OPENH264=ON/,
        'CI 安装腿必须带 NIMRTC_PLUGINS_OPENH264=ON（openh264 archive 才会进 bundle）');
});

// ---------------------------------------------------------------------------
// 一、装了什么 —— 安装规则（验收标准 1-3 的源侧）+ 二、链了什么（install 侧）
// ---------------------------------------------------------------------------
test('判据 1-3 源侧：install 规则装 nvcodec（含公开头）+ 装 openh264 三个 archive 与顺序清单', () => {
    const inst = readText('cmake/NimRTCInstall.cmake');

    const m = /set\(_nimrtc_install_shorts([\s\S]*?)\n\s*\)/.exec(inst);
    assert.ok(m, '_nimrtc_install_shorts 列表必须存在');
    assert.match(m[1], /^\s*nvcodec\s*$/m, '安装列表里必须有 nvcodec（判据 1 源侧）');

    assert.match(inst, /src\/modules\/nvcodec\/include/,
        'install 必须装 nvcodec 公开头（bundle 自洽，判据 12 源侧）');

    assert.match(inst, /NIMRTC_OPENH264_LINK_ARCHIVES/,
        'install 必须读 openh264 archive 集合（判据 2 源侧）');
    assert.match(inst, /openh264_link_order\.txt/,
        'install 必须写 + 装 openh264 顺序清单（判据 3 源侧）');
});

test('判据 6：openh264 wrapper 发布 archive 集合，顺序 = encoder / decoder / common', () => {
    const wrapper = readText('src/third_party/openh264/CMakeLists.txt');
    const m = /set_property\(GLOBAL PROPERTY NIMRTC_OPENH264_LINK_ARCHIVES([\s\S]*?)\n\s*\)/.exec(wrapper);
    assert.ok(m, 'wrapper 必须发布 NIMRTC_OPENH264_LINK_ARCHIVES');
    const tokens = m[1].replace(/\r?\n/g, ' ').trim().split(/\s+/);
    assert.deepEqual(tokens,
        ['nimrtc_vendor_openh264_encoder', 'nimrtc_vendor_openh264_decoder', 'nimrtc_vendor_openh264_common'],
        '顺序必须是 encoder / decoder / common（consumers 先、common 最后，判据 6）');
});

// ---------------------------------------------------------------------------
// 四、可重复 —— 工具 + 自检（验收标准 13-15），无需 build 树，永远跑
// ---------------------------------------------------------------------------
test('判据 13-15：check_install_tree_video_deps.py 存在、源检查跑绿、自检负样本被报出、再跑一次仍绿', () => {
    assert.ok(exists('tools/check_install_tree_video_deps.py'), '检查工具必须存在（判据 13）');

    // 判据 13 源侧：工具对暂存 cmake 源跑绿（不带树时只跑源检查）。
    const src = runPy(['tools/check_install_tree_video_deps.py']);
    assert.equal(src.status, 0, `工具的源检查必须跑绿：\n${src.out}`);

    // 判据 14：自检的负样本被报出（不能「永远绿」）。
    const st = runPy(['tools/check_install_tree_video_deps.py', '--self-test']);
    assert.equal(st.status, 0, `自检必须跑绿：\n${st.out}`);
    const reported = (st.out.match(/^PASS\s+a .* is reported/gm) || []).length;
    assert.ok(reported >= 4, `自检里至少四条负样本被报出，实际 ${reported} 条`);
    assert.match(st.out, /is not reported/, '自检里必须有正样本（完整树不被报出）');

    // 判据 15：同一工具再跑一次仍绿（确定性 / 可重复）。
    const again = runPy(['tools/check_install_tree_video_deps.py']);
    assert.equal(again.status, 0, `同一工具再跑一次仍要绿：\n${again.out}`);
});

// ---------------------------------------------------------------------------
// 三、装得对 —— 执行腿（验收标准 1-3、9-10、13）：暂存 + 检查 + 下游视频宿主
// ---------------------------------------------------------------------------
test('判据 1-3/9-10/13：暂存干净 prefix → 树里有视频依赖 → 视频宿主 0 LNK2019，只用 bundle',
    { skip: execLegBlocker(), timeout: 600000 }, () => {
        // 1) 暂存：cmake --install 到干净 prefix（bundle 自洽，判据 10）。
        const inst = run('cmake', ['--install', 'build', '--prefix', STAGED_PREFIX, '--config', 'Debug']);
        assert.equal(inst.status, 0, `cmake --install 失败：\n${inst.out}`);
        // 注意：这里不能用 exists() —— 它会把绝对路径 path.join 到 ROOT 后面，
        // 得到「ROOT+绝对路径」的双拼路径，永远不存在。绝对路径用 fs.existsSync。
        assert.ok(fs.existsSync(path.join(STAGED_PREFIX, 'lib', 'cmake', 'NimRTC', 'NimRTCConfig.cmake')),
            '安装树里必须有 NimRTCConfig.cmake');

        // 2) 装了什么（判据 1-3）：nvcodec + 三个 openh264 archive + 顺序清单。
        const libdir = path.join(STAGED_PREFIX, 'lib');
        assert.ok(fs.existsSync(path.join(libdir, 'nimrtc_nvcodec.lib')), '判据 1：nimrtc_nvcodec 必须装进 lib/');
        const manifest = fs.readFileSync(path.join(libdir, 'openh264_link_order.txt'), 'utf8');
        const order = manifest.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
        assert.deepEqual(order, [
            'nimrtc_vendor_openh264_encoder.lib',
            'nimrtc_vendor_openh264_decoder.lib',
            'nimrtc_vendor_openh264_common.lib',
        ], '判据 2/3：三个 openh264 archive 的清单顺序必须是 encoder / decoder / common');
        for (const name of order) {
            assert.ok(fs.existsSync(path.join(libdir, name)), `判据 2：${name} 必须装进 lib/`);
        }

        // 3) 装好的 config 链了什么（判据 4-7 的安装侧 + 归属：N6 缺口 B 解除）。
        //    绝对路径用 fs.readFileSync 直接读（readText 会把绝对路径 path.join 到 ROOT）。
        const stagedCfg = fs.readFileSync(
            path.join(STAGED_PREFIX, 'lib', 'cmake', 'NimRTC', 'NimRTCConfig.cmake'), 'utf8');
        assert.match(stagedCfg, /nvcodec/, '安装 config 必须链 nimrtc_nvcodec（判据 4 安装侧）');
        assert.match(stagedCfg, /openh264_link_order\.txt/, '安装 config 必须读 openh264 清单（判据 5 安装侧）');
        if (process.platform === 'win32') {
            assert.match(stagedCfg, /\bd3d11\b/, 'Windows 安装 config 必须链 d3d11（判据 7 安装侧）');
            assert.match(stagedCfg, /\bdxgi\b/, 'Windows 安装 config 必须链 dxgi（判据 7 安装侧）');
        }
        assert.ok(stagedCfg.includes('nvcodec') && stagedCfg.includes('openh264'),
            '安装 config 同时含 nvcodec 与 openh264 —— N6 执行腿的缺口 B 检查应不再命中（归属）');

        // 4) 检查工具对暂存树跑绿（判据 13 树侧）。
        const tool = runPy(['tools/check_install_tree_video_deps.py', '--root', STAGED_PREFIX]);
        assert.equal(tool.status, 0, `检查工具对暂存树必须跑绿：\n${tool.out}`);
        assert.match(tool.out, /OK — install tree video dependencies are in place/, '工具必须输出最终 OK');

        // 5) 下游视频宿主：只链 nimrtc::engine，h264 + headless sink（判据 9、10）。
        fs.mkdirSync(CONSUMER_DIR, { recursive: true });
        fs.writeFileSync(path.join(CONSUMER_DIR, 'CMakeLists.txt'), [
            'cmake_minimum_required(VERSION 3.20)',
            'project(video_host LANGUAGES CXX)',
            '',
            'find_package(NimRTC 1.0 REQUIRED CONFIG)',
            '',
            'add_executable(video_host video_host.cpp)',
            'target_link_libraries(video_host PRIVATE nimrtc::engine)',
            'set_target_properties(video_host PROPERTIES',
            '    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")',
            '',
        ].join('\n'));
        fs.writeFileSync(path.join(CONSUMER_DIR, 'video_host.cpp'), [
            '// Downstream video host - slice v1-install-tree-video-deps link proof.',
            '// Links the installed nimrtc::engine (h264 + headless sink); every video',
            '// symbol must resolve (the pre-fix bundle produced 31 LNK2019).',
            '#include <nimrtc/engine/engine.hpp>',
            '#include <nimrtc/h264/codec_plugin.hpp>',
            '#include <nimrtc/h264/hw_backends.hpp>',
            '#include <nimrtc/nvcodec/nv_backends.hpp>',
            '#include <nimrtc/nvcodec/nv_codec_kind.hpp>',
            '#include <nimrtc/video_sink/video_sink.hpp>',
            '',
            '// h264 + headless sink: register the software H.264 backend and the',
            '// default video plugins, probe the NVENC adapter, and build a headless',
            '// sink.  (No NimRTCEngine construction: that closure needs every',
            '// built-in codec module, which is beyond this packaging slice.)',
            'int main() {',
            '    nimrtc::h264::register_default_plugins();',
            '    nimrtc::h264::register_default_video_backends();',
            '    (void)nimrtc::nvcodec::nvenc_codec_available(nimrtc::nvcodec::NvCodec::kH264);',
            '    nimrtc::plugins::VideoSinkConfig sink_cfg;',
            '    sink_cfg.width = 1280;',
            '    sink_cfg.height = 720;',
            '    auto sink = nimrtc::video_sink::create_headless_sink(sink_cfg);',
            '    return sink ? 0 : 1;',
            '}',
            '',
        ].join('\n'));

        const consumerBuild = path.join(CONSUMER_DIR, 'build');
        const cfgCmd = run('cmake', [
            '-S', CONSUMER_DIR,
            '-B', consumerBuild,
            `-DCMAKE_PREFIX_PATH=${STAGED_PREFIX}`,
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
        ]);
        assert.equal(cfgCmd.status, 0, `下游宿主配置失败：\n${cfgCmd.out}`);

        const buildCmd = run('cmake', ['--build', consumerBuild, '--config', 'Debug']);
        assert.equal(buildCmd.status, 0,
            `下游宿主链接失败（判据 9：应 0 个 LNK2019）：\n${buildCmd.out}`);
        assert.doesNotMatch(buildCmd.out, /LNK2019|LNK2001|LNK1120|undefined reference/,
            `链接不得报未解析符号：\n${buildCmd.out}`);

        const exe = resolveConsumerExe(consumerBuild);
        assert.ok(fs.existsSync(exe), '下游宿主可执行文件必须产出');
        const runHost = run(exe, []);
        assert.equal(runHost.status, 0, `下游宿主运行必须退出码 0：\n${runHost.out}`);

        // 判据 10：宿主的链接行只引用暂存 bundle 的 archive（干净 prefix，
        // 无外部依赖）—— 解析生成工程的 AdditionalDependencies，不靠猜。
        if (process.platform === 'win32') {
            const vcxproj = path.join(consumerBuild, 'video_host.vcxproj');
            assert.ok(fs.existsSync(vcxproj), '下游宿主的 vcxproj 必须生成');
            const proj = fs.readFileSync(vcxproj, 'utf8');
            const m = /<AdditionalDependencies>([\s\S]*?)<\/AdditionalDependencies>/.exec(proj);
            assert.ok(m, 'vcxproj 必须有 AdditionalDependencies');
            const deps = m[1].split(/[;\s]+/).filter(Boolean);
            const nimDeps = deps.filter((d) =>
                /nimrtc_|openh264|opus\.lib|webrtc_apm/.test(d));
            assert.ok(nimDeps.length >= 5, `链接依赖里必须有视频相关 archive，实际：\n${nimDeps.join('\n')}`);
            const stagedLib = path.join(STAGED_PREFIX, 'lib');
            for (const d of nimDeps) {
                assert.ok(d.startsWith(stagedLib),
                    `链接依赖 ${d} 必须来自暂存 bundle（判据 10）`);
            }
        }
    });

// ---------------------------------------------------------------------------
// 五、边界与回归（验收标准 16-18）
// ---------------------------------------------------------------------------
test('判据 16-18：src/engine/（含公开头）与 src/modules/ice/ 不被本片碰；src/ 下无 C++ 源码改动', () => {
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: SLICE_ID });
    const bad = changed.filter((p) =>
        p.startsWith('src/engine/') ||
        p.startsWith('src/modules/ice/') ||
        /^src\/.*\.(cpp|hpp|h|cc)$/.test(p));
    assert.deepEqual(bad, [], `越界改动：\n${bad.join('\n')}`);
});

test('判据 12：engine 公开头集合不变 —— src/engine/include 不在改动清单里', () => {
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: SLICE_ID });
    const touched = changed.filter((p) => p.startsWith('src/engine/include/'));
    assert.deepEqual(touched, [], `engine 公开头被改：\n${touched.join('\n')}`);
});

// ---------------------------------------------------------------------------
// 六、文档与契约（验收标准 20-23）
// ---------------------------------------------------------------------------
test('判据 20：ADR-038 存在，写清装什么 / 链什么 / 顺序为什么重要 / NVIDIA import lib 的处置', () => {
    assert.ok(exists('docs/adr/ADR-038-install-tree-video-dependencies.md'), 'ADR-038 必须存在');
    const adr = readText('docs/adr/ADR-038-install-tree-video-dependencies.md');
    assert.match(adr, /装什么/, 'ADR 必须写「装什么」');
    assert.match(adr, /链什么/, 'ADR 必须写「链什么」');
    assert.match(adr, /顺序|order|from left to right|从左往右/, 'ADR 必须写「顺序为什么重要」');
    assert.match(adr, /import lib|import library|nvidia-encode|LoadLibrary|动态加载/,
        'ADR 必须写「NVIDIA import lib 的处置」');
});

test('判据 21：engine_api.md 记录安装树里现在提供的视频依赖', () => {
    const api = readText('docs/api/engine_api.md');
    assert.match(api, /安装树.*视频依赖|install-tree.*video/i,
        'engine_api.md 必须有安装树视频依赖这一节');
    assert.match(api, /nvcodec/, '那一节必须点名 nvcodec');
    assert.match(api, /openh264/, '那一节必须点名 openh264');
});

test('判据 22：CHANGELOG 有对应条目', () => {
    const changelog = readText('CHANGELOG.md');
    assert.match(changelog, /v1-install-tree-video-deps|安装树.*视频依赖|install-tree.*video dep/i,
        'CHANGELOG 必须记录本片');
    assert.match(changelog, /ADR-038/, 'CHANGELOG 必须指向 ADR-038');
});

test('判据 23：spec 记录实施中的发现', () => {
    const spec = readText('specs/slices/v1-install-tree-video-deps.md');
    assert.match(spec, /实施记录|实施中的发现|实施发现/, 'spec 必须有实施记录一节');
});

// ---------------------------------------------------------------------------
// 验收文件自身的诚实性 + 写集边界（ADR-075）
// ---------------------------------------------------------------------------
test('验收文件自身是真实断言：无 todo 占位，唯一的 skip 是执行腿的条件跳过', () => {
    const self = readText('tests/acceptance/v1-install-tree-video-deps/acceptance.test.cjs');
    const todoNeedle = ['test', 'todo'].join('.'); // 分片构造，避免自指
    assert.ok(!self.includes(todoNeedle), '验收文件不得有 todo 占位');
    const execLeg = /test\('判据 1-3\/9-10\/13[\s\S]*?\{ skip:\s*execLegBlocker\(\)/.exec(self);
    assert.ok(execLeg, '执行腿必须用 { skip: execLegBlocker() } 条件跳过（永不静默 PASS）');
});

const WRITE_SET = [
    'cmake/NimRTCInstall.cmake',
    'cmake/NimRTCConfig.cmake.in',
    'cmake/NimRTCVendored.cmake',
    'src/modules/nvcodec/CMakeLists.txt',
    'src/third_party/openh264/CMakeLists.txt',
    'tests/acceptance/v1-install-tree-video-deps',
    'tools/check_install_tree_video_deps.py',
    'docs/adr/ADR-038-install-tree-video-dependencies.md',
    'docs/api/engine_api.md',
    'CHANGELOG.md',
    'specs/slices/v1-install-tree-video-deps.md',
];
const REQUIRED_PRODUCTS = [
    'tests/acceptance/v1-install-tree-video-deps/acceptance.test.cjs',
    'tools/check_install_tree_video_deps.py',
    'docs/adr/ADR-038-install-tree-video-dependencies.md',
];

// 共享判据 collectSliceChanges 的三个来源里，有两个会把**不是本片 agent 改的**
// 路径算进 changed（写集边界判据的已知归属杂讯，不是真实越界）：
const KNOWN_OUTSIDE_ARTIFACTS = [
    // 910d1c5e 一个提交同时创建了两个切片规格，且同时带两个 Refs-Slice trailer
    // （v1-turn-tls-trust / v1-install-tree-video-deps）。共享判据按 trailer 把
    // 整个提交的 diff 归属给每个匹配的切片 → 并行片的规格文件必然落进本片 changed。
    'specs/slices/v1-turn-tls-trust.md',
    // wolfssl 是 vendored 子模块，工作区是会话前就存在的脏状态 —— 与共享判据
    // WRITE_SETS 里对 msquic 的处理（「会话前已存在的子模块脏状态」）同形。
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

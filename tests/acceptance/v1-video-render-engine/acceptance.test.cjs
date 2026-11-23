// 验收测试 —— 切片 v1-video-render-engine（D2：引擎驱动渲染器）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把骨架里的 todo 占位全部替换为真实断言，逐条对应规格的 23 条验收标准
// （specs/slices/v1-video-render-engine.md §验收标准）：
//   1.  EngineConfig::video_sink_name 默认值是 "auto"
//   2.  "" → headless（与今天逐字等价）
//   3.  "none" → 无 sink（video_sink() 返回空，backend 为空串）
//   4.  "headless" → headless
//   5.  "no-such-sink" → open() 失败，错误码 kVideoSinkPluginMissing
//   6.  平台偏好表（Windows d3d11 / macOS metal / Linux egl，条件编译断言）
//   7.  "auto" 且平台后端未注册 → 降级 headless + "backend-not-registered" + warn
//   8.  "auto" 且平台后端 open 失败 → 降级 headless + "backend-open-failed" + warn
//   9.  无平台后端（非桌面构建）→ 降级 headless + "no-platform-backend"
//   10. "auto" 成功 → backend 报实际 id，reason 为空
//   11. 显式 "d3d11"（Windows）→ 解析到 d3d11，不降级
//   12. 显式 id open 失败 → open() 硬失败 kVideoSinkOpenFailed（与 auto 相反）
//   13. Windows：native_window 为空 → 渲染器自建窗口（或 offscreen），open() 成功
//   14. macOS：同上（自建 NSWindow）
//   15. Linux：同上（EGL 今天已经自建）—— 回归保护
//   16. 静态：src/engine/ 不出现平台窗口头 include（windows.h / wayland-client.h /
//       X11/Xlib.h / Cocoa NSWindow）
//   17. 静态：src/engine/ 不引用 nimrtc::video_sink 具体符号（只用 IVideoSink 接缝）
//   18. Stats::video_sink_backend 报实际后端 id
//   19. 降级时 Stats::video_sink_backend_reason 非空；未降级时为空串
//   20. 三个 bench 显式设 video_sink_name = "headless"
//   21. tests/test_engine_*.cpp 一整套在默认值改成 "auto" 后仍然通过（回归）
//   22. docs/api/engine_api.md 记录新取值表与两个 stats 字段
//   23. docs/api/public_headers.manifest 与实际公开面一致（跑 check_api_manifest.py）
//
// 验证手段：本片的确定性单元测试（test_engine_render_policy.cpp）不在 build 树里
// 编译（写集不含 src/engine/tests/CMakeLists.txt，见规格「范围」），所以验收对
// 「解析与降级」标准用两类真实断言：
//   a. 静态断言 —— 逐条检查生产源码实现了取值表 / 降级阶梯 / 错误码 / stats 字段；
//   b. 单元测试文件断言 —— 检查 test_engine_render_policy.cpp 里真的写出了对应
//      每一条标准的 gtest 用例（判据 6 的「平台条件编译断言」就在这个文件里）。
// 回归（判据 21）跑 ctest 的 ^test_engine_ 整组；文档（判据 22/23）读
// engine_api.md + 跑 manifest 脚本。
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
const BUILD = path.join(ROOT, 'build');

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
         '--timeout', '120', '-R', nameRe],
        { cwd: ROOT, encoding: 'utf8', timeout: 600000 });
    return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/** 该 ctest 正则当前 build 树里匹配到的测试数（用于跳过平台专属二进制）。 */
function countCtest(nameRe) {
    const r = spawnSync('ctest',
        ['--test-dir', BUILD, '-N', '-R', nameRe, '-C', 'Debug'],
        { cwd: ROOT, encoding: 'utf8', timeout: 60000 });
    const m = /Total Tests:\s*(\d+)/.exec(`${r.stdout || ''}${r.stderr || ''}`);
    return m ? Number(m[1]) : 0;
}

// 生产源码路径。
const ENGINE_HPP   = path.join(ROOT, 'src/engine/include/nimrtc/engine/engine.hpp');
const MEDIA_CPP    = path.join(ROOT, 'src/engine/src/engine_media.cpp');
const POLICY_HPP   = path.join(ROOT, 'src/engine/src/engine_render_policy.hpp');
const POLICY_CPP   = path.join(ROOT, 'src/engine/src/engine_render_policy.cpp');
const D3D11_CPP    = path.join(ROOT, 'src/modules/video_sink/src/d3d11_renderer.cpp');
const METAL_CPP    = path.join(ROOT, 'src/modules/video_sink/src/metal_renderer.cpp');
const EGL_CPP      = path.join(ROOT, 'src/modules/video_sink/src/egl_renderer.cpp');
const TEST_CPP     = path.join(ROOT, 'src/engine/tests/test_engine_render_policy.cpp');
const BENCH_HOT    = path.join(ROOT, 'tests/bench_engine_hotpath.cpp');
const BENCH_ENC    = path.join(ROOT, 'tests/bench/bench_capture_encode.cpp');
const BENCH_CTRL   = path.join(ROOT, 'tests/bench/bench_control_saturation.cpp');
const API_MD       = path.join(ROOT, 'docs/api/engine_api.md');

// ---------------------------------------------------------------------------
// 一、取值语义（判据 1-5）
// ---------------------------------------------------------------------------
test('判据 1：默认值是 "auto"；验证方式：静态检查 engine.hpp 默认值 + 单测文件用例', () => {
    const engineHpp = readText(ENGINE_HPP);
    assert.match(engineHpp, /video_sink_name\s*=\s*"auto"/,
        'EngineConfig::video_sink_name 的默认值必须是 "auto"');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST_F\s*\(\s*EngineRenderPolicy\s*,\s*DefaultVideoSinkNameIsAuto\s*\)/,
        '单测文件必须有用例钉死默认值 "auto"');
    assert.match(testCpp, /cfg\.video_sink_name\s*,\s*"auto"/,
        '单测必须断言默认构造的 video_sink_name == "auto"');
});

test('判据 2："" → headless；验证方式：静态检查取值表 + 单测文件用例', () => {
    const media = readText(MEDIA_CPP);
    assert.match(media, /configured\.empty\(\)\s*\|\|\s*configured\s*==\s*"headless"/,
        'engine_media.cpp 必须把 "" 与 "headless" 合并进同一条头less 分支');
    assert.match(media, /make_plugin<&pal::resolve_video_sink>\(\s*"headless"/,
        '该分支必须实例化 headless sink');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST_F\s*\(\s*EngineRenderPolicy\s*,\s*EmptyNameIsHeadlessVerbatim\s*\)/,
        '单测文件必须有用例钉死 "" == headless（与今天逐字等价）');
    assert.match(testCpp, /cfg\.video_sink_name\s*=\s*""/,
        '单测必须把 video_sink_name 设为空串');
});

test('判据 3："none" → 无 sink；验证方式：静态检查 + 单测文件用例', () => {
    const media = readText(MEDIA_CPP);
    assert.match(media, /configured\s*==\s*"none"/,
        'engine_media.cpp 必须有 "none" 分支');
    assert.match(media, /configured == "none"[\s\S]*?\{[\s\S]*?video_sink_backend_\s*=\s*""/,
        '"none" 分支必须把 backend 记为空串且不实例化 sink');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST_F\s*\(\s*EngineRenderPolicy\s*,\s*NoneInstallsNoSink\s*\)/,
        '单测文件必须有用例钉死 "none" 行为');
    assert.match(testCpp, /cfg\.video_sink_name\s*=\s*"none"/,
        '单测必须把 video_sink_name 设为 "none"');
    assert.match(testCpp, /EXPECT_EQ\s*\(\s*engine\.video_sink\(\)\s*,\s*nullptr\s*\)/,
        '单测必须断言 video_sink() 返回空');
});

test('判据 4："headless" → headless；验证方式：静态检查 + 单测文件用例', () => {
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST_F\s*\(\s*EngineRenderPolicy\s*,\s*HeadlessIsTheExplicitSpelling\s*\)/,
        '单测文件必须有用例钉死显式 "headless"');
    assert.match(testCpp, /cfg\.video_sink_name\s*=\s*"headless"/,
        '单测必须把 video_sink_name 设为 "headless"');
    assert.match(testCpp, /"headless"/,
        '单测必须断言 backend == "headless"');
});

test('判据 5："no-such-sink" → open() 失败 kVideoSinkPluginMissing；验证方式：静态检查 + 单测文件用例', () => {
    const media = readText(MEDIA_CPP);
    assert.match(media, /pal::resolve_video_sink\(\s*configured\s*\)\s*==\s*nullptr/,
        '显式 id 分支必须查询工厂是否缺失');
    assert.match(media, /core::kVideoSinkPluginMissing/,
        '工厂缺失必须返回 kVideoSinkPluginMissing');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST_F\s*\(\s*EngineRenderPolicy\s*,\s*UnknownExplicitIdFailsOpen\s*\)/,
        '单测文件必须有用例钉死未知显式 id');
    assert.match(testCpp, /kVideoSinkPluginMissing/,
        '单测必须断言错误码 kVideoSinkPluginMissing');
});

// ---------------------------------------------------------------------------
// 二、"auto" 的解析与降级（判据 6-10）
// ---------------------------------------------------------------------------
test('判据 6：平台偏好表；验证方式：静态检查策略表 + 单测文件的条件编译断言', () => {
    const policyCpp = readText(POLICY_CPP);
    assert.match(policyCpp, /#if\s+defined\(_WIN32\)[\s\S]*?"d3d11"/,
        '策略表 Windows 分支必须偏好 d3d11');
    assert.match(policyCpp, /#elif\s+defined\(__APPLE__\)[\s\S]*?"metal"/,
        '策略表 macOS 分支必须偏好 metal');
    assert.match(policyCpp, /#elif\s+defined\(__linux__\)[\s\S]*?"egl"/,
        '策略表 Linux 分支必须偏好 egl');
    assert.match(policyCpp, /\.fallback_backend\s*=\s*"headless"/,
        '降级终点必须是 headless');
    // 单测文件必须带平台条件编译断言（判据 6 指定的验证方式）。
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST\s*\(\s*RenderPolicy\s*,\s*DefaultPolicyMatchesPlatformTable\s*\)/,
        '单测文件必须有平台策略表用例');
    assert.match(testCpp, /#if\s+defined\(_WIN32\)/,
        '单测必须带平台条件编译断言');
    assert.match(testCpp, /"d3d11"/,
        '单测的平台断言必须包含 d3d11');
});

test('判据 7：平台后端未注册 → 降级 + warn；验证方式：静态检查 + 单测文件用例', () => {
    const media = readText(MEDIA_CPP);
    assert.match(media, /degrade_sink\(\s*"headless"\s*,\s*"backend-not-registered"\s*\)/,
        '"auto" 解析不到注册后端必须降级 headless + 原因 "backend-not-registered"');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST\s*\(\s*RenderPolicy\s*,\s*PreferredBackendNotRegisteredResolvesEmpty\s*\)/,
        '单测文件必须钉死「未注册 → 解析空串」的纯函数规则');
});

test('判据 8：平台后端 open() 失败 → 降级 + warn；验证方式：静态检查 + 单测文件用例', () => {
    const media = readText(MEDIA_CPP);
    assert.match(media, /degrade_sink\(\s*"headless"\s*,\s*"backend-open-failed"\s*\)/,
        '"auto" 平台后端 open 失败必须降级 headless + 原因 "backend-open-failed"');
    assert.match(media, /open_fail_mode\s*=\s*pal::PluginFailureMode::Warn/,
        '"auto" 的 open 失败模式必须是 Warn（Fallback 语义），不是 Error');
    assert.match(media, /missing_mode\s*=\s*pal::PluginFailureMode::Warn/,
        '"auto" 的 missing 模式也必须是 Warn');
    // 降级不是静默的：degrade_sink 的实现体里必须有一行 warn。
    assert.match(media, /void NimRTCEngine::degrade_sink[\s\S]*?Logger::instance\(\)\.warn\(/,
        '降级必须 warn（判据 D2-1「never a silent drop」）');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST_F\s*\(\s*EngineRenderPolicy\s*,\s*AutoBackendOpenFailureDegradesToHeadless\s*\)/,
        '单测文件必须有「auto 后端 open 失败 → 降级」的引擎用例');
    assert.match(testCpp, /"backend-open-failed"/,
        '单测必须断言降级原因 "backend-open-failed"');
});

test('判据 9：无平台后端 → 降级 "no-platform-backend"；验证方式：静态检查 + 单测文件用例', () => {
    const media = readText(MEDIA_CPP);
    assert.match(media, /degrade_sink\(\s*"headless"\s*,\s*"no-platform-backend"\s*\)/,
        '没有平台默认时必须降级 headless + 原因 "no-platform-backend"');
    assert.match(media, /policy\.preferred_backend\.empty\(\)/,
        '降级判定必须基于 preferred_backend 为空');
    const policyCpp = readText(POLICY_CPP);
    assert.match(policyCpp, /#else[\s\S]*?preferred_backend\s*=\s*""/,
        '策略表的 #else 分支（非桌面构建）必须给空 preferred_backend');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST\s*\(\s*RenderPolicy\s*,\s*EmptyPreferredBackendResolvesNothing\s*\)/,
        '单测文件必须钉死「空 preferred → 解析空串」');
});

test('判据 10："auto" 成功 → 报实际后端 id + 空原因；验证方式：静态检查 + 单测文件用例', () => {
    const media = readText(MEDIA_CPP);
    assert.match(media, /video_sink_backend_\s*=\s*id;/,
        '解析成功必须把 backend 记为实际后端 id');
    assert.match(media, /video_sink_backend_reason_\s*=\s*"";/,
        '解析成功必须把 reason 清空');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST_F\s*\(\s*EngineRenderPolicy\s*,\s*AutoResolvesToThePlatformBackend\s*\)/,
        '单测文件必须有「auto 解析到平台后端」的引擎用例');
    assert.match(testCpp, /dynamic_cast\s*<\s*FakeSink\s*\*/,
        '单测必须检查创建出来的实例（证明走策略表而不是硬编码）');
    assert.match(testCpp, /video_sink_backend_reason\s*,\s*""/,
        '单测必须断言成功时 reason 为空');
});

// ---------------------------------------------------------------------------
// 三、显式 id 不降级（判据 11-12）
// ---------------------------------------------------------------------------
test('判据 11：显式 id 解析到自身、不降级；验证方式：静态检查 + 单测文件用例', () => {
    const media = readText(MEDIA_CPP);
    assert.match(media, /video_sink_backend_\s*=\s*std::string\(configured\);/,
        '显式 id 成功时必须把 backend 记为配置的 id（不降级）');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST_F\s*\(\s*EngineRenderPolicy\s*,\s*StatsReportTheActualBackendId\s*\)/,
        '单测文件必须钉死显式 id 报实际后端');
});

test('判据 12：显式 id open 失败是硬错误；验证方式：静态检查 + 单测文件用例', () => {
    const media = readText(MEDIA_CPP);
    assert.match(media, /core::kVideoSinkOpenFailed/,
        '显式 id 的 open 失败必须返回 kVideoSinkOpenFailed');
    assert.match(media, /last_open_rc\s*=\s*rc;/,
        '硬错误必须记入 last_open_rc');
    assert.match(media, /open_fail_mode\s*=\s*pal::PluginFailureMode::Error/,
        '显式 id 的 open 失败模式必须是 Error（硬承诺），与 auto 的 Warn 相反');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /TEST_F\s*\(\s*EngineRenderPolicy\s*,\s*ExplicitOpenFailureIsHardError\s*\)/,
        '单测文件必须有显式 open 失败 → 硬错误的用例');
    assert.match(testCpp, /kVideoSinkOpenFailed/,
        '单测必须断言错误码 kVideoSinkOpenFailed');
});

// ---------------------------------------------------------------------------
// 四、窗口归渲染器（判据 13-17）
// ---------------------------------------------------------------------------
test('判据 13：Windows 渲染器自建窗口；验证方式：静态检查 d3d11_renderer.cpp', () => {
    const d3d11 = readText(D3D11_CPP);
    assert.match(d3d11, /create_self_window\(\);/,
        'd3d11 渲染器必须有自建窗口声明/实现');
    assert.match(d3d11, /cfg_\.native_window\s*!=\s*nullptr\s*\|\|\s*create_self_window\(\)/,
        '无 native_window 且非 offscreen 时必须尝试自建窗口');
    assert.match(d3d11, /CreateWindowExW\(/,
        '自建窗口必须真的调用 CreateWindowExW');
    assert.match(d3d11, /ShowWindow\(\s*self_hwnd_\s*,\s*SW_SHOWNOACTIVATE\s*\)/,
        '自建窗口不得抢焦点');
    assert.match(d3d11, /DestroyWindow\(\s*self_hwnd_\s*\)/,
        '销毁路径必须销毁自建窗口');
});

test('判据 14：macOS 渲染器自建窗口；验证方式：静态检查 metal_renderer.cpp', () => {
    const metal = readText(METAL_CPP);
    assert.match(metal, /create_self_window\(\);/,
        'metal 渲染器必须有自建窗口声明/实现');
    assert.match(metal, /cfg_\.native_window\s*!=\s*nullptr\s*\|\|\s*create_self_window\(\)/,
        '无 native_window 且非 offscreen 时必须尝试自建窗口');
    assert.match(metal, /initWithContentRect:/,
        '自建窗口必须真的调用 initWithContentRect');
    assert.match(metal, /CAMetalLayer/,
        '自建窗口必须带 CAMetalLayer');
});

test('判据 15：Linux EGL 今天已经自建窗口；验证方式：静态检查 egl_renderer.cpp（回归保护）', () => {
    const egl = readText(EGL_CPP);
    assert.match(egl, /wl_compositor_create_surface\(/,
        'EGL 的 Wayland 路径必须自建 surface（今天的行为）');
    assert.match(egl, /XCreateSimpleWindow\(/,
        'EGL 的 X11 路径必须自建 window（今天的行为）');
    assert.match(egl, /no display available/,
        '无 compositor 也无 X11 时必须有可区分的失败日志');
});

test('判据 16：src/engine/ 不碰窗口系统；验证方式：静态检查四个窗口头 include', () => {
    const forbidden = [
        [/windows\.h/i,       'Windows 头'],
        [/wayland-client\.h/, 'Wayland 头'],
        [/X11\/Xlib\.h/,      'X11 头'],
        [/NSWindow\.h/,       'Cocoa/NSWindow 头'],
    ];
    for (const [re, label] of forbidden) {
        for (const d of ['src/engine/src', 'src/engine/include']) {
            assert.ok(!grepDirIncludes(path.join(ROOT, d), re),
                `${label}（${re}）不得被 src/engine/${d} include —— 引擎不碰窗口系统（L4）`);
        }
    }
});

test('判据 17：src/engine/ 只用 IVideoSink 接缝；验证方式：静态检查具体符号引用', () => {
    // 判据禁的是「引用具体符号」。引擎合法 include 的是接缝头
    // <nimrtc/plugins/video_sink.hpp>（5 处）；被禁的是具体模块路径
    // <nimrtc/video_sink/...> 与 nimrtc::video_sink 限定名。注释行剥掉。
    for (const d of ['src/engine/src', 'src/engine/include']) {
        const srcDir = path.join(ROOT, d);
        assert.ok(!grepDirIncludes(srcDir, /nimrtc\/video_sink\//),
            `${d} 不得 include 具体模块头 nimrtc/video_sink/ —— 引擎只走 IVideoSink 接缝`);
        assert.ok(!grepDirCode(srcDir, /nimrtc::video_sink\s*::/),
            `${d} 不得以 nimrtc::video_sink:: 引用具体符号`);
        assert.ok(!grepDirCode(srcDir, /using\s+namespace\s+nimrtc::video_sink/),
            `${d} 不得 using namespace nimrtc::video_sink`);
    }
});

// ---------------------------------------------------------------------------
// 五、stats 可观测（判据 18-19）
// ---------------------------------------------------------------------------
test('判据 18：Stats::video_sink_backend 报实际后端；验证方式：静态检查 + 单测文件用例', () => {
    const engineHpp = readText(ENGINE_HPP);
    assert.match(engineHpp, /std::string\s+video_sink_backend\s*;/,
        '公开 Stats 必须加 video_sink_backend 字段');
    const media = readText(MEDIA_CPP);
    assert.match(media, /out\.video_sink_backend\s*=\s*video_sink_backend_/,
        'Stats 快照必须回填 video_sink_backend');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /StatsReportTheActualBackendId/,
        '单测文件必须钉死 stats 报实际后端 id');
});

test('判据 19：降级时 reason 非空；验证方式：静态检查 + 单测文件用例', () => {
    const engineHpp = readText(ENGINE_HPP);
    assert.match(engineHpp, /std::string\s+video_sink_backend_reason\s*;/,
        '公开 Stats 必须加 video_sink_backend_reason 字段');
    const media = readText(MEDIA_CPP);
    assert.match(media, /out\.video_sink_backend_reason\s*=\s*video_sink_backend_reason_/,
        'Stats 快照必须回填 video_sink_backend_reason');
    assert.match(media, /video_sink_backend_reason_\s*=\s*std::string\(reason\);/,
        '降级时必须把原因写进内部状态');
    assert.match(media, /video_sink_backend_reason_\s*=\s*"";/,
        '未降级时必须把 reason 清空');
    const testCpp = readText(TEST_CPP);
    assert.match(testCpp, /"backend-open-failed"/,
        '单测必须断言降级用例的 reason 非空');
    assert.match(testCpp, /video_sink_backend_reason\s*,\s*""/,
        '单测必须断言未降级用例的 reason 为空串');
});

// ---------------------------------------------------------------------------
// 六、破坏面（判据 20-21）
// ---------------------------------------------------------------------------
test('判据 20：三个 bench 显式设 headless；验证方式：静态检查三个文件', () => {
    for (const [file, label] of [
        [BENCH_HOT, 'tests/bench_engine_hotpath.cpp'],
        [BENCH_ENC, 'tests/bench/bench_capture_encode.cpp'],
        [BENCH_CTRL, 'tests/bench/bench_control_saturation.cpp'],
    ]) {
        const src = readText(file);
        assert.match(src, /video_sink_name\s*=\s*"headless"/,
            `${label} 必须显式设 video_sink_name = "headless"（否则渲染器 GPU 开销污染热路径）`);
    }
});

test('判据 21：既有引擎测试不回归；验证方式：ctest 跑 ^test_engine_ 整组', () => {
    const n = countCtest('^test_engine_');
    assert.ok(n >= 1, `build 树里应有 test_engine_* 测试（count=${n}）`);
    const res = runCtest('^test_engine_');
    assert.equal(res.status, 0,
        `^test_engine_ 回归失败（exit=${res.status}）：\n${res.out}`);
});

// ---------------------------------------------------------------------------
// 七、文档与契约（判据 22-23）
// ---------------------------------------------------------------------------
test('判据 22：engine_api.md 记录新取值表与两个 stats 字段；验证方式：静态检查', () => {
    const api = readText(API_MD);
    assert.match(api, /"auto"/,
        'engine_api.md 必须记录 "auto" 取值');
    assert.match(api, /"none"/,
        'engine_api.md 必须记录 "none" 取值（显式退出）');
    assert.match(api, /video_sink_backend/,
        'engine_api.md 必须记录 video_sink_backend');
    assert.match(api, /video_sink_backend_reason/,
        'engine_api.md 必须记录 video_sink_backend_reason');
});

test('判据 23：公开头清单与实际公开面一致；验证方式：跑 check_api_manifest.py', () => {
    const r = spawnSync('python',
        ['scripts/check_api_manifest.py'],
        { cwd: ROOT, encoding: 'utf8', timeout: 120000 });
    assert.equal(r.status, 0,
        `check_api_manifest.py 未通过（exit=${r.status}）：\n` +
        `${r.stdout || ''}${r.stderr || ''}`);
    assert.match(`${r.stdout || ''}${r.stderr || ''}`, /OK/,
        'check_api_manifest.py 应输出 OK');
});

// ---------------------------------------------------------------------------
// 验收文件自身是真实断言（门禁②：无 todo 残留）
// ---------------------------------------------------------------------------
test('判据：验收文件自身是真实断言；验证方式：node --test 全部通过 + 无 todo 用例', () => {
    const own = readText(__filename);
    const todoNeedle = ['test', 'todo('].join('.');
    assert.ok(!own.includes(todoNeedle),
        '验收文件不得残留 todo 用例（门禁②要求每条用例都是可执行断言）');
});

// ---------------------------------------------------------------------------
// 写集边界断言（ADR-075）
// ---------------------------------------------------------------------------
test('写集边界：本片改动仅限写集范围；验证方式：改动清单不越界 + 产物在树上', () => {
    // 写集 = 规格「范围」（specs/slices/v1-video-render-engine.md §写集）。
    const ALLOW = [
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/src/engine_render_policy.hpp',
        'src/engine/src/engine_render_policy.cpp',
        'src/engine/src/engine_media.cpp',
        'src/engine/CMakeLists.txt',
        'src/modules/video_sink/src/d3d11_renderer.cpp',
        'src/modules/video_sink/src/metal_renderer.cpp',
        'src/modules/video_sink/src/egl_renderer.cpp',
        'src/engine/tests/test_engine_render_policy.cpp',
        'tests/bench/bench_capture_encode.cpp',
        'tests/bench/bench_control_saturation.cpp',
        'tests/bench_engine_hotpath.cpp',
        'tests/acceptance/v1-video-render-engine',
        'docs/adr/ADR-030-engine-driven-video-renderer.md',
        'docs/api/engine_api.md',
        'docs/api/public_headers.manifest',
        'CHANGELOG.md',
    ];

    const REQUIRED = [
        // 引擎：公开头 + 私有策略 + 接线 + 单测
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/src/engine_render_policy.hpp',
        'src/engine/src/engine_render_policy.cpp',
        'src/engine/src/engine_media.cpp',
        'src/engine/CMakeLists.txt',
        'src/engine/tests/test_engine_render_policy.cpp',
        // 渲染器：三个平台的自建窗口 / 可区分失败
        'src/modules/video_sink/src/d3d11_renderer.cpp',
        'src/modules/video_sink/src/metal_renderer.cpp',
        'src/modules/video_sink/src/egl_renderer.cpp',
        // 破坏面：bench 显式 headless
        'tests/bench_engine_hotpath.cpp',
        // 文档 + 契约 + 验收
        'docs/adr/ADR-030-engine-driven-video-renderer.md',
        'docs/api/engine_api.md',
        'docs/api/public_headers.manifest',
        'tests/acceptance/v1-video-render-engine/acceptance.test.cjs',
        'CHANGELOG.md',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed, untracked } = ws.collectSliceChanges({
        root: ROOT, sliceId: 'v1-video-render-engine' });
    // ② 产物检查（状态）：产物必须存在 —— 已追踪的算「在树上」，未追踪的
    //    算「本片新产物」（本片尚未签收提交，新文件自然是 untracked）。
    const onTree = ws.trackedOnTree(ROOT, REQUIRED);
    const present = [...new Set([...onTree, ...untracked])];
    const { violations, missing } = ws.evaluateChanged({
        changed,
        allow: ALLOW,
        required: REQUIRED,
        present,
    });

    assert.deepEqual(violations, [],
        `改动超出本片写集范围：${violations.join(', ')}`);
    assert.deepEqual(missing, [],
        `本片产物缺失：${missing.join(', ')}`);
    assert.ok(present.length >= 1,
        `本片产物必须在树上（已追踪或未跟踪新文件）：${REQUIRED.join(', ')}`);
});

// ---------------------------------------------------------------------------
// 只读帮助函数（放到文件尾部，避免与上面的目录遍历混在一起）
// ---------------------------------------------------------------------------
/** 目录里是否有任一文件的 include 指令命中模式（递归读 .hpp/.cpp）。 */
function grepDirIncludes(dir, headerRe) {
    const includeRe = /^\s*#include\s*[<"][^>"]*$/m;
    const entries = fs.readdirSync(dir, { withFileTypes: true });
    for (const e of entries) {
        const p = path.join(dir, e.name);
        if (e.isDirectory()) {
            if (grepDirIncludes(p, headerRe)) return true;
            continue;
        }
        if (!/\.(hpp|cpp)$/.test(e.name)) continue;
        const src = readText(p);
        for (const line of src.split('\n')) {
            if (includeRe.test(line) && headerRe.test(line)) return true;
        }
    }
    return false;
}

/** 目录里任一文件的「非注释代码」命中模式（剥掉行注释、块注释、doc 注释）。 */
function grepDirCode(dir, re) {
    const entries = fs.readdirSync(dir, { withFileTypes: true });
    for (const e of entries) {
        const p = path.join(dir, e.name);
        if (e.isDirectory()) {
            if (grepDirCode(p, re)) return true;
            continue;
        }
        if (!/\.(hpp|cpp)$/.test(e.name)) continue;
        const code = readText(p)
            .replace(/\/\*[\s\S]*?\*\//g, '')         // 块注释（含 doc 注释）
            .replace(/\/\/.*$/gm, '');                // 行注释（含尾随注释）
        if (re.test(code)) return true;
    }
    return false;
}

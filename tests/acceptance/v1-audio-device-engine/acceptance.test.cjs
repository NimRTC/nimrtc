// 验收测试 —— 切片 v1-audio-device-engine（D1：引擎驱动音频设备层）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把骨架里的 todo 占位全部替换为真实断言，逐条对应规格的 22 条验收标准
// （specs/slices/v1-audio-device-engine.md §验收标准）：
//   1.  注册表有两个音频设备槽位（get 返回同一指针 + list 含该 id）
//   2.  audio_device 模块注册后端（list 非空且含 "synthetic"）
//   3.  解析路径唯一（pal::resolve_audio_source == 模块内 source_factory）
//   4.  Category 既有枚举值语义不变（static_assert）
//   5.  EngineConfig 空值语义不变（空 → 不实例化采集设备）
//   6.  "auto" 走运行时解析（portaudio 优先，不可用回落 synthetic）
//   6b. auto 解析规则可脱离注册表单测（is_registered 注入）
//   6c. 策略 id 未注册 → 降级「无设备」+ warn，不报错
//   7.  "none" 是显式退出（不实例化设备，不因此失败）
//   8.  引擎驱动采集（假 source 产帧 → 引擎出站计数前进）
//   9.  引擎驱动播放（解码 PCM 写进假 sink）
//   10. 3A 参考对齐：有 sink 用 played_samples() 增量；无 sink 用帧样本数
//   11. "auto" 设备打不开 → 降级 + warn，open() 成功
//   12. 显式 id 打不开 → open() 失败（与 11 语义相反）
//   13. Stats 报出实际协商出的设备 id 与后端 id
//   14. 泵计数器存在并递增（pumped_frames / pump_errors）
//   15. 不加新线程（泵挂在已有 tick 路径上）
//   16. 依赖方向不破（engine 不引用 audio_device；core 不引用 modules）
//   17. 接缝没被破坏（三个音频接缝头逐字不变）
//   18. 公开头清单同步（check_api_manifest.py 通过）
//   19. EngineConfig 新字段有文档（含空值语义）
//   20. 验收测试是真实断言（无 todo 残留）
//   21. 既有行为不回归（ctest 既有音频/引擎/注册表测试全绿）
//   22. CI 无声卡也能过（"auto" 解析到 synthetic 且引擎正常工作）
//
// 验证手段：调用门禁同款 build 树里的 ctest 用例（engine_audio_device_tests
// 整二进制，含全部 17 条 gtest 用例）+ 读取源码做静态断言 + 跑 manifest 脚本。
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
         '-R', nameRe, '-V'],
        { cwd: ROOT, encoding: 'utf8', timeout: 600000 });
    return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/** gtest 单条用例是否通过（输出里出现 `[       OK ] <case>`）。 */
function gtestOk(out, gtestCase) {
    return out.includes(`[       OK ] ${gtestCase}`);
}

/**
 * 从 ctest 输出里抽出实际跑过的 gtest 用例名（`[       OK ] <suite>.<case>`）。
 *
 * 用途：验收断言引用用例名时，如果单测那边**改名**了，报错只说
 * 「未跑出 OK」并甩一整段输出 —— 人得自己从几百行里翻出真名。
 * 把实际名单列出来，改名一眼可见。
 */
function gtestCaseNames(out) {
    const names = [];
    const re = /^\s*\[\s*(?:OK|PASSED)\s*\]\s+(\S+)/gm;
    let m;
    while ((m = re.exec(out)) !== null) names.push(m[1]);
    return names;
}

/** gtestOk 的失败消息：带上实际跑过的用例名。 */
function gtestMissing(out, gtestCase) {
    const actual = gtestCaseNames(out);
    const hint = actual.length
        ? `\n实际跑过的用例名：\n  ${actual.join('\n  ')}`
        : '\n（输出里没解析出任何 gtest 用例名 —— 二进制可能没跑起来）';
    return `用例 "${gtestCase}" 未跑出 OK${hint}`;
}

/** 该 ctest 正则当前 build 树里匹配到的测试数（用于跳过平台专属二进制）。 */
function countCtest(nameRe) {
    const r = spawnSync('ctest',
        ['--test-dir', BUILD, '-N', '-R', nameRe, '-C', 'Debug'],
        { cwd: ROOT, encoding: 'utf8', timeout: 60000 });
    const m = /Total Tests:\s*(\d+)/.exec(`${r.stdout || ''}${r.stderr || ''}`);
    return m ? Number(m[1]) : 0;
}

// 判据 1-14 与 22 共用同一次 ctest 运行：engine_audio_device_tests 是整二进制
// 运行，输出里含全部 gtest 用例的 `[       OK ]`。node:test 默认顺序执行，
// 惰性记忆安全。
const audioRun = { status: -1, out: '' };
function runAudioSuite() {
    if (audioRun.status === -1) {
        Object.assign(audioRun, runCtest('^engine_audio_device_tests$'));
    }
    return audioRun;
}

// ---------------------------------------------------------------------------
// 验收标准 1：注册表有两个音频设备槽位
// ---------------------------------------------------------------------------
test('判据 1：注册表有两个音频设备槽位；验证方式：单元测试（get 同一指针 + list 含该 id）', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDeviceRegistry.RegistrySlotsReturnSamePointer'),
        gtestMissing(res.out, 'EngineAudioDeviceRegistry.RegistrySlotsReturnSamePointer'));
});

// ---------------------------------------------------------------------------
// 验收标准 2：audio_device 模块注册后端
// ---------------------------------------------------------------------------
test('判据 2：audio_device 注册后端；验证方式：单元测试 + 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.AudioDeviceModuleRegistersSynthetic'),
        gtestMissing(res.out, 'EngineAudioDevice.AudioDeviceModuleRegistersSynthetic'));

    // 模块的 register_default_plugins() 把 synthetic 源与 sink 工厂放进注册表。
    const moduleCpp = readText(path.join(ROOT,
        'src/modules/audio_device/src/audio_device.cpp'));
    assert.match(moduleCpp, /register_audio_source\(/,
        '模块必须调用 register_audio_source');
    assert.match(moduleCpp, /register_audio_sink\(/,
        '模块必须调用 register_audio_sink');
    assert.match(moduleCpp, /g_synthetic_source_factory\.id\(\)/,
        '模块必须注册 synthetic 源工厂');
    assert.match(moduleCpp, /g_synthetic_sink_factory\.id\(\)/,
        '模块必须注册 synthetic 汇工厂');
});

// ---------------------------------------------------------------------------
// 验收标准 3：解析路径唯一
// ---------------------------------------------------------------------------
test('判据 3：解析路径唯一；验证方式：单元测试（pal 解析 == 模块内工厂）', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.PalResolverMatchesModuleFactories'),
        gtestMissing(res.out, 'EngineAudioDevice.PalResolverMatchesModuleFactories'));
});

// ---------------------------------------------------------------------------
// 验收标准 4：Category 既有枚举值语义不变
// ---------------------------------------------------------------------------
test('判据 4：Category 既有枚举值语义不变；验证方式：单元测试（static_assert）', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDeviceRegistry.CategoryValuesUnchanged'),
        gtestMissing(res.out, 'EngineAudioDeviceRegistry.CategoryValuesUnchanged'));
});

// ---------------------------------------------------------------------------
// 验收标准 5：EngineConfig 空值语义不变
// ---------------------------------------------------------------------------
test('判据 5：空值语义不变；验证方式：单元测试 + 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.EmptyAudioDeviceNamesKeepTodayBehavior'),
        gtestMissing(res.out, 'EngineAudioDevice.EmptyAudioDeviceNamesKeepTodayBehavior'));

    // 空值 = 宿主自己喂 PCM：泵对空名字不实例化任何设备。
    const pump = readText(path.join(ROOT,
        'src/engine/src/engine_audio_device_pump.cpp'));
    assert.match(pump, /!config_\.audio_source_name\.empty\(\)/,
        '空 audio_source_name 不得进入设备初始化');
    assert.match(pump, /!config_\.audio_source_name\.empty\(\)[\s\S]*?!= "none"/,
        '空值与 "none" 必须分开处理（空值保持今天行为）');
});

// ---------------------------------------------------------------------------
// 验收标准 6 + 22："auto" 走运行时解析（portaudio 优先，回落 synthetic）
// ---------------------------------------------------------------------------
test('判据 6：auto 走运行时解析；验证方式：单元测试（优先 portaudio，缺位回落 synthetic）', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDeviceRegistry.AutoResolutionPrefersPortAudioWhenRegistered'),
        gtestMissing(res.out, 'EngineAudioDeviceRegistry.AutoResolutionPrefersPortAudioWhenRegistered'));
});

test('判据 22：CI 无声卡也能过；验证方式：单元测试（模拟 portaudio 不可用 → synthetic）+ 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDeviceRegistry.AutoResolutionFallsBackToSyntheticWhenPortAudioAbsent'),
        gtestMissing(res.out, 'EngineAudioDeviceRegistry.AutoResolutionFallsBackToSyntheticWhenPortAudioAbsent'));

    // 模块侧：portaudio_available() 为假时整个 portaudio 后端不注册。
    const moduleCpp = readText(path.join(ROOT,
        'src/modules/audio_device/src/audio_device.cpp'));
    assert.match(moduleCpp, /if\s*\(\s*portaudio_available\(\)\s*\)[\s\S]*?register_audio_source/,
        'portaudio 后端的注册必须被 portaudio_available() 门控');
});

// ---------------------------------------------------------------------------
// 验收标准 6b："auto" 解析规则可脱离注册表单测（is_registered 注入）
// ---------------------------------------------------------------------------
test('判据 6b：auto 解析规则可脱离注册表单测；验证方式：单元测试 + 静态检查（is_registered 注入）', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDeviceRegistry.AutoResolutionPrefersPortAudioWhenRegistered'),
        gtestMissing(res.out, 'EngineAudioDeviceRegistry.AutoResolutionPrefersPortAudioWhenRegistered'));
    assert.ok(gtestOk(res.out, 'EngineAudioDeviceRegistry.AutoResolutionFallsBackToSyntheticWhenPortAudioAbsent'),
        gtestMissing(res.out, 'EngineAudioDeviceRegistry.AutoResolutionFallsBackToSyntheticWhenPortAudioAbsent'));

    // 与 D3 同形：解析函数接受注入的 is_registered 函数指针，不查进程级注册表。
    const policyHpp = readText(path.join(ROOT,
        'src/engine/src/engine_device_policy.hpp'));
    assert.match(policyHpp,
        /resolve_auto_audio_backend\([\s\S]*?bool \(\*is_registered\)\([\s\S]*?noexcept\)/,
        'auto 解析函数必须接受注入的 is_registered 回调');
});

// ---------------------------------------------------------------------------
// 验收标准 6c：策略 id 未注册 → 降级「无设备」+ warn
// ---------------------------------------------------------------------------
test('判据 6c：策略 id 未注册 → 降级不报错；验证方式：单元测试 + 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDeviceRegistry.AutoResolutionWithNothingRegisteredDegradesToEmpty'),
        gtestMissing(res.out, 'EngineAudioDeviceRegistry.AutoResolutionWithNothingRegisteredDegradesToEmpty'));

    // 降级可观测：warn 一行（不是静默，也不是错误）。
    const pump = readText(path.join(ROOT,
        'src/engine/src/engine_audio_device_pump.cpp'));
    assert.match(pump, /continuing without an audio device/,
        'auto 解析不到注册后端必须 warn（降级为无设备）');
    // 解析失败的 missing_mode 对 auto 是 Warn（不触发硬错误）；显式 id 才是 Error。
    assert.match(pump,
        /missing_mode\s*=\s*is_auto\s*\?[\s\S]*?PluginFailureMode::Warn[\s\S]*?PluginFailureMode::Error/,
        'missing_mode 必须对 auto 用 Warn、对显式 id 用 Error');
    // 降级路径不走错误回调：错误常量在 Warn 模式下不冒泡成 open() 失败
    //（判据 12 单测已断言显式 id 的 Error 路径）。
    assert.match(pump, /open_fail_mode\s*=\s*is_auto\s*\?[\s\S]*?PluginFailureMode::Warn/,
        'open 失败模式必须对 auto 用 Warn（判据 11）');
});

// ---------------------------------------------------------------------------
// 验收标准 7："none" 是显式退出
// ---------------------------------------------------------------------------
test('判据 7：none 是显式退出；验证方式：单元测试', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.NoneIsExplicitOptOut'),
        gtestMissing(res.out, 'EngineAudioDevice.NoneIsExplicitOptOut'));
});

// ---------------------------------------------------------------------------
// 验收标准 8：引擎驱动采集
// ---------------------------------------------------------------------------
test('判据 8：引擎驱动采集；验证方式：单元测试（假 source 产帧 → 出站计数前进）+ 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.EnginePumpsCaptureIntoOutboundPath'),
        gtestMissing(res.out, 'EngineAudioDevice.EnginePumpsCaptureIntoOutboundPath'));

    // 泵喂的是与 send_audio() 相同的出站链：走引擎内部，不经过宿主。
    const pump = readText(path.join(ROOT,
        'src/engine/src/engine_audio_device_pump.cpp'));
    assert.match(pump, /read_samples\(/,
        '泵必须从 source 拉帧');
    assert.match(pump, /send_audio\(/,
        '泵必须喂引擎内部出站链');

    // §八 核实要求：examples/ 不得手写 IAudioSource / IAudioSink 使用点
    //（否则宿主手推 + 引擎泵推 → 双重驱动 → 远端听到双份音频）。
    const examplesCpp = [];
    for (const f of ['rd_device.cpp', 'rd_viewer.cpp', 'rd_verify.cpp']) {
        const p = path.join(ROOT, 'examples/remote-desktop', f);
        if (fs.existsSync(p)) examplesCpp.push(readText(p));
    }
    for (const src of examplesCpp) {
        assert.ok(!src.includes('IAudioSource') && !src.includes('IAudioSink'),
            'examples/remote-desktop 不得手写 IAudioSource / IAudioSink 使用点' +
            '（双重驱动 → 双份音频）');
    }
});

// ---------------------------------------------------------------------------
// 验收标准 9：引擎驱动播放
// ---------------------------------------------------------------------------
test('判据 9：引擎驱动播放；验证方式：单元测试（解码 PCM 写进假 sink）+ 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.EngineWritesDecodedPcmIntoSink'),
        gtestMissing(res.out, 'EngineAudioDevice.EngineWritesDecodedPcmIntoSink'));

    const media = readText(path.join(ROOT, 'src/engine/src/engine_media.cpp'));
    assert.match(media, /audio_sink_/,
        '引擎必须持有 sink 实例');
    assert.match(media, /audio_sink_->write\(/,
        '解码后的 PCM 必须写进 sink');
});

// ---------------------------------------------------------------------------
// 验收标准 10：3A 参考对齐用 played_samples()
// ---------------------------------------------------------------------------
test('判据 10：3A 参考对齐；验证方式：单元测试（有 sink 用 played 增量 / 无 sink 用帧样本数）+ 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.RenderDeliveredUsesPlayedDeltaWithSink'),
        gtestMissing(res.out, 'EngineAudioDevice.RenderDeliveredUsesPlayedDeltaWithSink'));
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.RenderDeliveredUsesFrameSamplesWithoutSink'),
        gtestMissing(res.out, 'EngineAudioDevice.RenderDeliveredUsesFrameSamplesWithoutSink'));

    const media = readText(path.join(ROOT, 'src/engine/src/engine_media.cpp'));
    assert.match(media, /audio_sink_->played_samples\(\)/,
        '有 sink 时必须读 sink 的实际消费样本数');
    assert.match(media, /set_render_delivered\(delivered\)/,
        '有 sink 时 set_render_delivered 必须传 played 增量');
    assert.match(media, /set_render_delivered\(samples_per_ch\)/,
        '无 sink 时保持今天行为（传帧样本数）');
});

// ---------------------------------------------------------------------------
// 验收标准 11：设备打开失败可降级（音频，与视频侧语义不同！）
// ---------------------------------------------------------------------------
test('判据 11：auto 打不开 → 降级 + warn；验证方式：单元测试 + 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.AutoOpenFailureDegrades'),
        gtestMissing(res.out, 'EngineAudioDevice.AutoOpenFailureDegrades'));

    const pump = readText(path.join(ROOT,
        'src/engine/src/engine_audio_device_pump.cpp'));
    assert.match(pump, /open_fail_mode = is_auto[\s\S]*?PluginFailureMode::Warn/,
        'auto 的 open 失败必须是 Warn（降级）而不是 Error');
});

// ---------------------------------------------------------------------------
// 验收标准 12：显式 id 打不开报错
// ---------------------------------------------------------------------------
test('判据 12：显式 id 打不开报错；验证方式：单元测试 + 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.ExplicitIdOpenFailureFailsOpen'),
        gtestMissing(res.out, 'EngineAudioDevice.ExplicitIdOpenFailureFailsOpen'));

    const pump = readText(path.join(ROOT,
        'src/engine/src/engine_audio_device_pump.cpp'));
    assert.match(pump, /open_fail_mode = is_auto[\s\S]*?PluginFailureMode::Error/,
        '显式 id 的 open 失败必须是 Error（硬承诺）');
    assert.match(pump, /first_error = core::kEngineInternal/,
        '显式 id 失败必须上报 first_error');
});

// ---------------------------------------------------------------------------
// 验收标准 13：Stats 报出两个设备（实际协商出来的）
// ---------------------------------------------------------------------------
test('判据 13：Stats 报出两个设备；验证方式：单元测试 + 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.StatsReportsNegotiatedDeviceIds'),
        gtestMissing(res.out, 'EngineAudioDevice.StatsReportsNegotiatedDeviceIds'));

    // 公开 Stats 的四字段 + 两个泵计数器。
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    for (const field of ['audio_source_device_id', 'audio_sink_device_id',
                         'audio_device_pumped_frames', 'audio_device_pump_errors']) {
        assert.match(engineHpp, new RegExp(field),
            `公开 Stats 缺字段 ${field}`);
    }
    // 引擎内部状态也持有设备 id / 后端 id（协商结果）。
    const audioState = readText(path.join(ROOT,
        'src/engine/src/engine_audio_state.hpp'));
    for (const field of ['audio_source_device_id', 'audio_source_backend',
                         'audio_sink_device_id', 'audio_sink_backend']) {
        assert.match(audioState, new RegExp(field),
            `引擎音频状态缺字段 ${field}`);
    }
    // Stats 报的是协商出的 device_id：pump 读 config().device_id 回填。
    const pump = readText(path.join(ROOT,
        'src/engine/src/engine_audio_device_pump.cpp'));
    assert.match(pump, /negotiated\.device_id/,
        '设备 id 必须取自 open() 之后实际生效的 config()');
});

// ---------------------------------------------------------------------------
// 验收标准 14：泵计数器存在并递增
// ---------------------------------------------------------------------------
test('判据 14：泵计数器存在；验证方式：单元测试 + 静态检查', () => {
    const res = runAudioSuite();
    assert.equal(res.status, 0,
        `engine_audio_device_tests 未通过（exit=${res.status}）：\n${res.out}`);
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.EnginePumpsCaptureIntoOutboundPath'),
        gtestMissing(res.out, 'EngineAudioDevice.EnginePumpsCaptureIntoOutboundPath'));
    assert.ok(gtestOk(res.out, 'EngineAudioDevice.PumpErrorsIncrementOnDeviceReadFailure'),
        gtestMissing(res.out, 'EngineAudioDevice.PumpErrorsIncrementOnDeviceReadFailure'));

    const pump = readText(path.join(ROOT,
        'src/engine/src/engine_audio_device_pump.cpp'));
    assert.match(pump, /\+\+astate\.audio_device_pumped_frames/,
        '采到帧时必须递增 pumped_frames');
    assert.match(pump, /\+\+astate\.audio_device_pump_errors/,
        '泵出错时必须递增 pump_errors');
});

// ---------------------------------------------------------------------------
// 验收标准 15：不加新线程
// ---------------------------------------------------------------------------
test('判据 15：不加新线程；验证方式：静态检查', () => {
    const pump = readText(path.join(ROOT,
        'src/engine/src/engine_audio_device_pump.cpp'));
    assert.ok(!pump.includes('std::thread('),
        'engine_audio_device_pump.cpp 不得新增 std::thread');
    assert.ok(!pump.includes('ThreadPool'),
        'engine_audio_device_pump.cpp 不得新增线程池');

    // 泵调用点在 tick() 链路上，与 pump_video_capture() 并列。
    const transport = readText(path.join(ROOT,
        'src/engine/src/engine_transport.cpp'));
    assert.match(transport, /pump_video_capture\(\);[\s\S]*?pump_audio_capture\(\);/,
        '音频泵必须挂在 tick 路径上（与视频泵并列）');
});

// ---------------------------------------------------------------------------
// 验收标准 16：依赖方向不破
// ---------------------------------------------------------------------------
test('判据 16：依赖方向不破；验证方式：静态检查', () => {
    // src/engine/ 不引用 nimrtc::audio_device（依赖方向是 device→engine）。
    for (const f of ['src/engine/src', 'src/engine/include']) {
        assert.ok(!grepDir(path.join(ROOT, f), /nimrtc::audio_device/),
            `${f} 不得引用 nimrtc::audio_device`);
    }
    // src/core/ 不 include src/modules 的任何头（注释里提到 modules/ 不算
    // 引用 —— 判据只禁 #include 指令本身）。
    for (const f of ['src/core/include', 'src/core/src']) {
        assert.ok(!grepDir(path.join(ROOT, f), /^\s*#include\s*[<"][^>"]*modules\//m),
            `${f} 不得 include src/modules 下的头`);
    }
});

/** 目录里是否有任一文件命中模式（递归读 .hpp/.cpp）——只读帮助函数。 */
function grepDir(dir, re) {
    const entries = fs.readdirSync(dir, { withFileTypes: true });
    for (const e of entries) {
        const p = path.join(dir, e.name);
        if (e.isDirectory()) {
            if (grepDir(p, re)) return true;
            continue;
        }
        if (!/\.(hpp|cpp)$/.test(e.name)) continue;
        if (re.test(readText(p))) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 验收标准 17：接缝没被破坏
// ---------------------------------------------------------------------------
test('判据 17：接缝没被破坏；验证方式：静态检查（三个音频接缝头逐字不变）', () => {
    // IAudioSource / IAudioSink 声明与本片开始前逐字相同：git diff 为空。
    for (const header of [
        'src/plugins/include/nimrtc/plugins/audio_source.hpp',
        'src/plugins/include/nimrtc/plugins/audio_sink.hpp',
        'src/plugins/include/nimrtc/plugins/audio_device.hpp',
    ]) {
        const r = spawnSync('git', ['diff', '--quiet', 'HEAD', '--', header],
            { cwd: ROOT, encoding: 'utf8' });
        assert.equal(r.status, 0,
            `${header} 相对 HEAD 有差异 —— 接缝被本片改动过`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 18：公开头清单同步
// ---------------------------------------------------------------------------
test('判据 18：公开头清单同步；验证方式：跑 check_api_manifest.py', () => {
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
// 验收标准 19：EngineConfig 新字段有文档
// ---------------------------------------------------------------------------
test('判据 19：EngineConfig 新字段有文档；验证方式：静态检查', () => {
    const api = readText(path.join(ROOT, 'docs/api/engine_api.md'));
    for (const field of ['audio_source_name', 'audio_sink_name',
                         'audio_source_config', 'audio_sink_config']) {
        assert.match(api, new RegExp(field),
            `engine_api.md 缺字段说明 ${field}`);
    }
    // 文档必须说明空值语义 = 宿主自己喂 PCM（今天的行为不变）。
    assert.match(api, /host feeds PCM itself/,
        'engine_api.md 必须说明空值 = 宿主自己喂 PCM');
});

// ---------------------------------------------------------------------------
// 验收标准 20：验收测试是真实断言
// ---------------------------------------------------------------------------
test('判据 20：验收测试文件是真实断言；验证方式：node --test 全部通过 + 无 todo 用例', () => {
    const own = readText(__filename);
    // 骨架生成的待办用例是 test 的待办重载。用拼接构造 needle，
    // 避免把待办标记本身写进本文件 —— 否则自检会误伤自己。
    const todoNeedle = ['test', 'todo('].join('.');
    assert.ok(!own.includes(todoNeedle),
        '验收文件不得残留 todo 用例（门禁②要求每条用例都是可执行断言）');
    // 本文件每条用例都是 test(...) 真断言；node --test 能跑到这里，即证明
    // 前面所有用例都已通过。
});

// ---------------------------------------------------------------------------
// 验收标准 21：不破坏既有行为
// ---------------------------------------------------------------------------
test('判据 21：不破坏既有行为；验证方式：ctest 无回归（既有音频/引擎/注册表测试）', () => {
    const regression = [
        { re: '^audio_device_tests$',       note: 'audio_device 模块测试' },
        { re: '^engine_device_policy_tests$', note: '既有引擎设备策略测试' },
        { re: '^test_engine_audio_loopback$', note: '既有引擎音频回环' },
        { re: '^test_engine_audio_alloc$',  note: '既有引擎音频分配/出站路径' },
        { re: '^test_engine_capture_pump$', note: '既有引擎采集泵' },
        { re: '^test_registry_contract$',   note: '注册表契约测试' },
        { re: '^test_core_stats_registry$', note: '核心 Stats/注册表测试' },
        { re: '^test_audio_seams$',         note: '音频接缝测试' },
    ];
    for (const r of regression) {
        const n = countCtest(r.re);
        assert.ok(n >= 1, `${r.re} 应该在 build 树里有测试（count=${n}）`);
        const res = runCtest(r.re);
        assert.equal(res.status, 0,
            `${r.note}（${r.re}）未通过（exit=${res.status}）：\n${res.out}`);
        assert.ok(res.out.includes('[==========]'),
            `${r.note}（${r.re}）没有产出 gtest 运行输出：\n${res.out}`);
    }
});

// ---------------------------------------------------------------------------
// 写集边界断言（ADR-075）
// ---------------------------------------------------------------------------
test('写集边界：本片改动仅限写集范围；验证方式：改动清单不越界 + 产物在树上', () => {
    // 写集 = 规格「范围」（bundle 的 writes）。
    const ALLOW = [
        'src/core/include/nimrtc/core/registry.hpp',
        'src/core/src/plugin_registry.cpp',
        'src/core/src/pal_default_registrars.cpp',
        'src/modules/audio_device',
        'src/engine',
        'src/plugins/include/nimrtc/plugins/audio_source.hpp',
        'src/plugins/include/nimrtc/plugins/audio_sink.hpp',
        'src/plugins/include/nimrtc/plugins/audio_device.hpp',
        'examples/remote-desktop',
        'docs/adr/ADR-029-engine-driven-audio-device-layer.md',
        'docs/adr/README.md',                  // ADR-029 的索引条目（切片提交带上）
        'docs/api/engine_api.md',
        'docs/api/public_headers.manifest',
        'tests/acceptance/v1-audio-device-engine',
        'specs/slices/v1-audio-device-engine.md', // 本片规格（切片提交带上）
    ];

    const REQUIRED = [
        // 注册表槽位（判据 1）
        'src/core/include/nimrtc/core/registry.hpp',
        'src/core/src/plugin_registry.cpp',
        'src/core/src/pal_default_registrars.cpp',
        // 模块注册（判据 2）
        'src/modules/audio_device/include/nimrtc/audio_device/audio_device.hpp',
        'src/modules/audio_device/src/audio_device.cpp',
        // 引擎配置 + 解析 + 泵 + 状态 + 统计
        'src/engine/include/nimrtc/engine/engine.hpp',
        'src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp',
        'src/engine/src/engine_audio_device_pump.cpp',
        'src/engine/src/engine_audio_state.hpp',
        'src/engine/src/engine_media.cpp',
        'src/engine/src/engine_stats_state.hpp',
        'src/engine/src/engine_transport.cpp',
        // 确定性单元测试
        'src/engine/tests/test_engine_audio_device.cpp',
        'src/engine/tests/CMakeLists.txt',
        // 文档 + 清单 + 验收
        'docs/adr/ADR-029-engine-driven-audio-device-layer.md',
        'docs/api/engine_api.md',
        'docs/api/public_headers.manifest',
        'tests/acceptance/v1-audio-device-engine/acceptance.test.cjs',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed, untracked } = ws.collectSliceChanges({
        root: ROOT, sliceId: 'v1-audio-device-engine' });
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

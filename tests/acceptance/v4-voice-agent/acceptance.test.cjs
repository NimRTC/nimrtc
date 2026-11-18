// 验收测试 —— 切片 v4-voice-agent（语音 Agent 链路：ASR/LLM/TTS）
//
// 每条验收标准对应一条真实断言（`test`），骨架里的占位符已全部替换为真实断言：
//   1. ASR 后端可由配置项选择，存在 Whisper 与 vosk 两个接入点
//   2. ASR 消费的音频是 post-3A 的 int16 PCM
//   3. LLM 以流式方式被消费（增量回调，而非一次性拿整段）
//   4. TTS 走 Opus 编码器；Tap 音频按 100ms 攒批再送模型
//   5. 打断（barge-in）到停播的耗时被记录
//   6. 缺模型时明确报错，而不是静默退回 mock（除非显式要求退回）
//   7. 缺模型分支的错误输出存在
//
// 验证方式分两层：对 `examples/demo-agent-realtime/voice_agent.hpp`、
// `demo-agent-realtime.cpp` 与 `src/modules/opus/` 源码做结构断言（确定性），
// 并运行门禁同款 build 树里注册的 ctest 用例 `demo-agent-realtime_voice_self_test`
// （确定性：固定帧数、固定 barge-in 时刻，exit=0 且报告全部指标）。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT  = path.resolve(__dirname, '..', '..', '..');
const BUILD = path.join(ROOT, 'build');

const VOICE   = path.join(ROOT, 'examples/demo-agent-realtime/voice_agent.hpp');
const CPP     = path.join(ROOT, 'examples/demo-agent-realtime/demo-agent-realtime.cpp');
const OPUS_H  = path.join(ROOT, 'src/modules/opus/include/nimrtc/opus/opus.hpp');
const OPUS_CP = path.join(ROOT, 'src/modules/opus/src/opus.cpp');

// 写集边界判据：四个套件共用同一份实现（v5-writeset-acceptance/）。
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, 'utf8'); }
function exists(p)   { return fs.existsSync(p); }

/** 定位已构建的 demo-agent-realtime 可执行文件（VS 多配置 / 单配置两套布局）。 */
function binaryPath() {
    const names = process.platform === 'win32'
        ? ['demo-agent-realtime.exe', 'demo-agent-realtime']
        : ['demo-agent-realtime'];
    const dirs = [
        path.join(BUILD, 'examples', 'demo-agent-realtime', 'Debug'),
        path.join(BUILD, 'examples', 'demo-agent-realtime'),
        path.join(BUILD, 'examples', 'Debug'),
        path.join(BUILD, 'examples'),
    ];
    for (const d of dirs) {
        for (const n of names) {
            const p = path.join(d, n);
            if (exists(p)) return p;
        }
    }
    throw new Error('demo-agent-realtime binary not found under ' + BUILD);
}

/** 用受控环境跑二进制（缺省删掉两个模型目录环境变量，保证缺模型分支可复现）。 */
let _noModelEnv = null;
function noModelEnv() {
    if (_noModelEnv) return _noModelEnv;
    const env = { ...process.env };
    delete env.NIMRTC_WHISPER_MODEL_DIR;
    delete env.NIMRTC_VOSK_MODEL_DIR;
    _noModelEnv = env;
    return _noModelEnv;
}
function runBinary(args) {
    const r = spawnSync(binaryPath(), args, {
        cwd: ROOT, encoding: 'utf8', timeout: 120000, env: noModelEnv(),
    });
    return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/** 跑 ctest 里注册的语音自检（锚定精确名称）。惰性：只跑一次。 */
let _voiceSelfTest = null;
function voiceSelfTestRun() {
    if (_voiceSelfTest) return _voiceSelfTest;
    const r = spawnSync('ctest',
        ['--test-dir', BUILD, '--output-on-failure', '-C', 'Debug',
         '-R', '^demo-agent-realtime_voice_self_test$', '-V'],
        { cwd: ROOT, encoding: 'utf8', timeout: 240000 });
    _voiceSelfTest = { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
    return _voiceSelfTest;
}

// ---------------------------------------------------------------------------
// 验收标准 1：ASR 后端可由配置选择，存在 Whisper 与 vosk 两个接入点
// ---------------------------------------------------------------------------
test('ASR后端可由配置项选择，存在Whisper与vosk两个接入点', () => {
    const hpp = readText(VOICE);
    const cpp = readText(CPP);

    // ---- 选择逻辑：make_asr(name, opts) 按名分派 --------------------------
    assert.match(hpp, /make_asr\(std::string_view name/, '缺少 make_asr 工厂');
    assert.match(hpp, /name == "mock"/,        '选择逻辑必须含 mock 分支');
    assert.match(hpp, /name == "whisper"/,     '选择逻辑必须含 whisper 分支');
    assert.match(hpp, /name == "vosk"/,        '选择逻辑必须含 vosk 分支');
    assert.match(hpp, /available_asr_backends/, '缺少后端列表');

    // ---- 两个接入点的实现（后端类） ---------------------------------------
    assert.match(hpp, /class WhisperAsr final : public IASR/, '缺少 WhisperAsr 实现');
    assert.match(hpp, /class VoskAsr final : public IASR/,    '缺少 VoskAsr 实现');
    assert.match(hpp, /class MockAsr final : public IASR/,    '缺少 M1 mock 实现');

    // ---- 命令行选择（缺省仍是 mock） ---------------------------------------
    assert.match(cpp, /--asr/,          '缺少 --asr 命令行选择开关');
    assert.match(cpp, /--voice-self-test/, '缺少 --voice-self-test 入口');
    assert.match(cpp, /std::string_view asr_name = "mock"/,
        '缺省 ASR 后端必须是 mock（M1 实现，不引入真模型依赖时仍能跑）');
});

// ---------------------------------------------------------------------------
// 验收标准 2：ASR 消费的音频是 post-3A 的 int16 PCM
// ---------------------------------------------------------------------------
test('ASR消费的音频是post-3A的int16 PCM', () => {
    const hpp = readText(VOICE);

    // ---- 缓冲类型与采样格式：int16 位深 + 48000Hz 声明 --------------------
    assert.match(hpp, /struct PcmFormat/, '缺少格式声明结构');
    assert.match(hpp, /bits_per_sample = 16/, 'int16 位深声明缺失');
    assert.match(hpp, /sample_rate_hz = 48000/, '48kHz 采样率声明缺失');

    // ---- ASR 接口输入是 const int16_t* ------------------------------------
    assert.match(hpp, /feed\(const std::int16_t\* pcm/, 'ASR 必须消费 int16 缓冲');
    assert.match(hpp, /on_post_3a_int16\(const std::int16_t\* pcm/,
        'post-3A tap 入口必须接收 int16 PCM');

    // ---- post-3A（而非 pre-3A）标注 ---------------------------------------
    assert.match(hpp, /post-3A/, '必须显式标注 post-3A 数据来源');
    assert.match(hpp, /post-3A int16 PCM reaches the model|not the pre-3A/,
        '必须说明 ASR 消费的是 3A 之后的 int16，而非原始麦克风数据');
});

// ---------------------------------------------------------------------------
// 验收标准 3：LLM 以流式方式被消费（增量回调）
// ---------------------------------------------------------------------------
test('LLM以流式方式被消费（增量回调而非一次性拿整段）', () => {
    const hpp = readText(VOICE);

    // ---- 流式接口：stream(prompt, on_token)，on_token 逐 token 调用 -------
    assert.match(hpp, /class ILLM/, '缺少 ILLM 流式接口');
    assert.match(hpp, /TokenSink/, '缺少增量 token 回调类型');
    assert.match(hpp,
        /virtual void stream\(const std::string& prompt, const TokenSink& on_token\)/,
        'LLM 必须以流式 stream(prompt, 回调) 消费，而非一次性返回整段');
    assert.match(hpp, /on_token\(std::string_view\(word\)/,
        'mock LLM 必须逐词（逐 token）调用回调');
    assert.match(hpp, /on_token\(std::string_view\(\), \/\*final\*\/ true\)/,
        '流式结束必须以 final=true 回调收尾');

    // ---- 运行验证：自检断言增量 token 确实到达 -----------------------------
    const { status, out } = voiceSelfTestRun();
    assert.equal(status, 0,
        `demo-agent-realtime_voice_self_test 未通过（exit=${status}）：\n${out}`);
    const m = out.match(/llm_tokens=(\d+)/);
    assert.ok(m, `缺少 llm_tokens 报告：\n${out}`);
    assert.ok(Number(m[1]) >= 1,
        `LLM 必须至少增量产出一个 token（实际 ${m[1]}）：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 4：TTS 走 Opus 编码器；Tap 音频 100ms 攒批后送模型
// ---------------------------------------------------------------------------
test('TTS走Opus编码器，Tap音频按100ms攒批后送模型', () => {
    const hpp    = readText(VOICE);
    const ophpp  = readText(OPUS_H);
    const opuscpp = readText(OPUS_CP);

    // ---- Opus 编码器调用：demo 侧 OpusTts -> Encoder::encode_int16 -------
    assert.match(hpp, /class OpusTts/, '缺少 Opus TTS 回传封装');
    assert.match(hpp, /encode_int16\(pcm, num_samples, output, output_capacity\)/,
        'TTS int16 PCM 必须经 Encoder::encode_int16 编码');
    assert.match(hpp, /enc_\.encode_int16/, '必须真正调用 Opus 编码器');

    // ---- opus 模块侧：int16 编码路径存在（native opus_encode） -------------
    assert.match(ophpp, /encode_int16\(const std::int16_t\* pcm/,
        'opus.hpp 必须声明 int16 编码入口');
    assert.match(opuscpp, /opus_encode\(/, 'opus.cpp 必须调用 native opus_encode');

    // ---- 100ms 批大小常量 + 攒批逻辑 ---------------------------------------
    assert.match(hpp, /kTapFrameMs = 10/,   '10ms tap 帧常量缺失');
    assert.match(hpp, /kTapBatchMs = 100/,  '100ms 批大小常量缺失');
    assert.match(hpp, /accumulated_ms_ \+= frame_ms/, '必须有帧时长累加');
    assert.match(hpp, /accumulated_ms_ >= kTapBatchMs/, '必须攒满 100ms 才 flush');
    assert.match(hpp, /batches_flushed/, '必须有批次数统计（证明不是逐帧调模型）');

    // ---- 运行验证：300 帧 10ms = 30 个 100ms 批；Opus 包 >= 1 --------------
    const { status, out } = voiceSelfTestRun();
    assert.equal(status, 0,
        `demo-agent-realtime_voice_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out, /tap_batches=30 asr_samples_fed=144000/,
        `300 帧×10ms 应攒成 30 批、喂 144000 个 int16 样本：\n${out}`);
    const m = out.match(/opus_packets=(\d+)/);
    assert.ok(m, `缺少 opus_packets 报告：\n${out}`);
    assert.ok(Number(m[1]) >= 1,
        `TTS 必须产出 Opus 编码包（实际 ${m[1]}）：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 5：打断到停播的耗时被记录下来
// ---------------------------------------------------------------------------
test('打断到停播的耗时被记录（barge-in -> stop delta）', () => {
    const hpp = readText(VOICE);

    // ---- 记录点：打断指令时刻 + 停播帧时刻 + 差值 --------------------------
    assert.match(hpp, /class BargeInMeter/, '缺少打断度量器');
    assert.match(hpp, /record_barge_in\(std::int64_t ts_us\)/,
        '缺少「打断指令时刻」记录点');
    assert.match(hpp, /record_stop\(std::int64_t ts_us\)/,
        '缺少「停播帧时刻」记录点');
    assert.match(hpp, /last_delta_us\(\)/, '缺少差值访问器');
    assert.match(hpp, /stop_ts_us_ - barge_in_ts_us_/,
        '必须计算 stop − barge_in 差值');
    assert.match(hpp, /on_barge_in\(std::int64_t ts_us\)/,
        '管线必须暴露 barge-in 入口');

    // ---- 运行验证：固定 barge-in 时刻，差值被如实记录 -----------------------
    const { status, out } = voiceSelfTestRun();
    assert.equal(status, 0,
        `demo-agent-realtime_voice_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out, /barge_in_events=1 barge_in_to_stop_us=10000/,
        `打断→停播差值（10ms 帧内停播策略）应被记录：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 6 & 7：缺模型时明确报错，不静默退回 mock（除非显式要求退回）
// ---------------------------------------------------------------------------
test('缺模型时明确报错而不是静默退回mock（除非显式要求退回）', () => {
    const hpp = readText(VOICE);
    const cpp = readText(CPP);

    // ---- 缺模型分支的错误输出存在 ------------------------------------------
    assert.match(hpp, /unavailable/, '缺模型分支必须输出 unavailable 错误');
    assert.match(hpp, /allow_mock_fallback/, '必须存在显式退回开关');
    assert.match(cpp, /--allow-mock-fallback/, '必须暴露 --allow-mock-fallback 开关');
    assert.match(hpp, /if \(!opts\.allow_mock_fallback\) return fail\(why\)/,
        '未显式要求退回时，缺模型必须是失败而不是静默退回 mock');

    // ---- 运行验证 1：whisper 缺模型且未显式退回 → 明确失败（exit != 0） -----
    const noFallback = runBinary(['--voice-self-test', '--asr', 'whisper',
                                  '--frames', '300', '--barge-in-at', '150']);
    assert.notEqual(noFallback.status, 0,
        `缺 whisper 模型时自检应明确失败（exit=${noFallback.status}）：\n${noFallback.out}`);
    assert.match(noFallback.out, /backend 'whisper' unavailable/,
        `缺模型错误信息应输出：\n${noFallback.out}`);
    assert.match(noFallback.out, /no_silent_fallback=1/,
        `应声明没有静默退回：\n${noFallback.out}`);

    // ---- 运行验证 2：显式 --allow-mock-fallback → 退回 mock 且链路通过 -------
    const withFallback = runBinary(['--voice-self-test', '--asr', 'whisper',
                                    '--allow-mock-fallback',
                                    '--frames', '300', '--barge-in-at', '150']);
    assert.equal(withFallback.status, 0,
        `显式退回 mock 后自检应通过（exit=${withFallback.status}）：\n${withFallback.out}`);
    assert.match(withFallback.out, /whisper -> mock/,
        `应输出显式退回声明：\n${withFallback.out}`);
    assert.match(withFallback.out, /mock_fallback_optin=1/,
        `应记录显式退回：\n${withFallback.out}`);

    // ---- 运行验证 3：vosk 同样有接入点与错误分支 ---------------------------
    const vosk = runBinary(['--voice-self-test', '--asr', 'vosk',
                            '--frames', '300', '--barge-in-at', '150']);
    assert.notEqual(vosk.status, 0,
        `缺 vosk 模型时自检应明确失败（exit=${vosk.status}）：\n${vosk.out}`);
    assert.match(vosk.out, /backend 'vosk' unavailable/,
        `vosk 缺模型错误信息应输出：\n${vosk.out}`);
});

// ---------------------------------------------------------------------------
// 验收标准（整体）：门禁同款命令跑的自检全绿
// ---------------------------------------------------------------------------
test('demo-agent-realtime_voice_self_test 整题通过（exit=0 + self-test PASS）', () => {
    const { status, out } = voiceSelfTestRun();
    assert.equal(status, 0, `exit=${status}：\n${out}`);
    assert.match(out, /\[voice-agent\] self-test PASS/, `自检未 PASS：\n${out}`);
});

// ---------------------------------------------------------------------------
// 写集边界：本片只改写集内的路径
// ---------------------------------------------------------------------------
test('本片改动仅限写集（examples/demo-agent-realtime/**、src/modules/opus/**、本验收目录）', () => {
    // 写集边界判据（v5-writeset-acceptance/writeset-boundary.cjs，四个套件共用）：
    // 三源并集 —— 工作区未提交差异 ∪ 本片提交差异 ∪ 未跟踪文件。断言从
    // 「读工作区差异」改为「读提交历史」后，在已提交的干净树上同样成立（D-68），
    // 越界检测不削弱：并集里任何落在写集外的路径仍判 failed。
    const verdict = ws.evaluateSliceWriteSet({ root: ROOT, sliceId: 'v4-voice-agent' });
    assert.deepEqual(verdict.violations, [],
        `改动超出写集：${verdict.violations.join(', ')}`);
    assert.deepEqual(verdict.missing, [],
        `本片产物缺失（写集内必须存在改动）：${verdict.missing.join(', ')}`);
    assert.ok(verdict.changed.length >= 1,
        '本片改动必须非空（工作区差异 ∪ 提交历史 ∪ 未跟踪文件的并集）');
});

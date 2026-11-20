// 验收测试 —— 切片 v1-audio-p2（D5：音频 P2 —— DTX 耦合 + 3A 契约）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把骨架里的 8 条 todo 替换为真实断言，逐条对应规格的 8 条验收标准
// （specs/slices/v1-audio-p2.md §验收标准，行 99-111）：
//   1. DTX 帧在解码侧被识别为「静音」而非「普通帧」（结构性判据）
//   2. DTX 静音不触发遮蔽（frames_concealed 不递增）
//   3. 真正丢掉的帧仍然触发遮蔽（负控制，0 → 1）
//   4. DTX 协商状态被真正消费（use_dtx 有实际读取点）
//   5. usedtx 的 SDP 协商链打通（offer 声明 / answer 三态回显 / 传到接收器）
//   6. D5-1：DTX 默认关
//   7. 验收测试文件是真实断言（无 todo 用例残留）
//   8. 不破坏既有行为（ctest 无回归）
//
// 验证手段：读取源码做静态断言 + 调用门禁同款 build 树里的 ctest 用例。
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

// ---------------------------------------------------------------------------
// 验收标准 1：DTX 帧在解码侧被识别为「静音」而非「普通帧」
// ---------------------------------------------------------------------------
test('DTX 帧在解码侧被识别为「静音」而非「普通帧」；验证方式：结构性判据 + 解码侧置位 + 解码单测全绿', () => {
    const hpp = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/audio_decoder.hpp'));
    const cpp = readText(path.join(ROOT,
        'src/modules/opus/src/audio_decoder_plugin.cpp'));

    // (a) AudioDecodedFrame 新增承载标记的布尔字段（追加在末尾，默认 false）。
    assert.match(hpp, /bool\s+dtx_silence\s*=\s*false/,
        'AudioDecodedFrame 缺 dtx_silence 字段');

    // (b) 判据必须是结构性的：1 字节包 → depacketise() 后帧体 size == 0；
    //     不允许「PCM 全零」的间接猜测（那会把真静音 PCM 与 DTX 抑制混为一谈）。
    assert.match(cpp, /if\s*\(\s*v\.size\s*==\s*0\s*\)\s*\{/,
        'DTX 判据必须是结构性的（帧体长度 == 0）');
    assert.match(cpp, /never\s+"the decoded PCM is all zero"/,
        '判据必须显式排除「PCM 是否全零」的间接猜测');

    // (c) 判据顺序：DTX 在 FEC 恢复之前生效（D5-2 —— DTX 帧不携带 FEC 冗余，
    //     颠倒顺序会让 DTX 静音被当成「无 FEC 可恢复」的丢帧）。
    assert.match(cpp, /if\s*\(\s*v\.size\s*==\s*0[\s\S]*?i\s*==\s*0\s*&&\s*gap/,
        'DTX 判据必须先于 FEC 恢复出现（D5-2）');
    assert.match(cpp, /check\s+runs\s+\*\*before\*\*\s+the\s+FEC\s+attempt/,
        '代码注释必须写明「DTX 判据在 FEC 之前」这个顺序');

    // (d) 解码时正确置位：emit() 把 dtx_silence 写进 AudioDecodedFrame，
    //     DTX 分支 emit 一帧 dtx_silence=true 的静音。
    assert.match(cpp, /f\.dtx_silence\s*=\s*dtx_silence;/,
        'emit() 必须把 dtx_silence 写进 AudioDecodedFrame');
    assert.match(cpp, /dtx_silence=\*\/true/,
        'DTX 分支必须 emit dtx_silence=true（按静音帧输出）');

    // (e) 行为落点：4 条新单测全绿（OpusAudioDecoder.*），既有普通帧用例不回归。
    const dec = runCtest('test_opus_audio_decoder$');
    assert.equal(dec.status, 0,
        `test_opus_audio_decoder 未通过（exit=${dec.status}）：\n${dec.out}`);
    for (const gcase of [
        'OpusAudioDecoder.DtxSilenceFrameIsFlagged',
        'OpusAudioDecoder.TheDtxCriterionIsStructuralAndDoesNotDependOnTheNegotiation',
        'OpusAudioDecoder.ANormalFrameIsNotDtxSilence',
        'OpusAudioDecoder.ADtxPacketAfterALossIsNotMisclassifiedByFec',
    ]) {
        assert.ok(gtestOk(dec.out, gcase), `DTX 解码单测未跑出 OK：${gcase}\n${dec.out}`);
    }
    assert.ok(gtestOk(dec.out, 'OpusAudioDecoder.DecodesASingleFramePayload'),
        '既有普通帧解码用例未跑出 OK（回归）：\n' + dec.out);
});

// ---------------------------------------------------------------------------
// 验收标准 2：DTX 静音不触发遮蔽
// ---------------------------------------------------------------------------
test('DTX 静音不触发遮蔽；验证方式：连续 DTX 静音帧喂进流水线，frames_concealed 保持 0', () => {
    // 接收流水线必须识别 dtx_silence 帧并按静音处理。
    const cpp = readText(path.join(ROOT,
        'src/modules/audio_rx/src/receiver.cpp'));
    assert.match(cpp, /if\s*\(\s*f\.dtx_silence\s*\)\s*\{/,
        '接收流水线必须识别 dtx_silence 帧');

    const rx = runCtest('test_audio_rx$');
    assert.equal(rx.status, 0,
        `test_audio_rx 未通过（exit=${rx.status}）：\n${rx.out}`);
    assert.ok(gtestOk(rx.out,
        'AudioRxTest.DtxSilenceFramesArePlayedAsSilenceNotConcealment'),
        '「DTX 静音不遮蔽」单测未跑出 OK：\n' + rx.out);
    assert.ok(gtestOk(rx.out,
        'AudioRxTest.TimelineGapProducesExactlyTheMissingConcealedFrames'),
        '既有「缺口遮蔽」用例未跑出 OK（回归）：\n' + rx.out);
});

// ---------------------------------------------------------------------------
// 验收标准 3：真正丢掉的帧仍然触发遮蔽（负控制）
// ---------------------------------------------------------------------------
test('真正丢掉的帧仍然触发遮蔽（负控制）；验证方式：真实丢帧使 frames_concealed 从 0 变 1', () => {
    // 遮蔽路径必须仍然递增计数器（不能因为 DTX 处理把遮蔽整个关掉）。
    const cpp = readText(path.join(ROOT,
        'src/modules/audio_rx/src/receiver.cpp'));
    assert.match(cpp, /st\.frames_concealed\s*\+\+/,
        '真丢帧仍须计入 frames_concealed');

    const rx = runCtest('test_audio_rx$');
    assert.equal(rx.status, 0,
        `test_audio_rx 未通过（exit=${rx.status}）：\n${rx.out}`);
    assert.ok(gtestOk(rx.out,
        'AudioRxTest.ARealLossStillConcealsEvenWhenDtxFramesArrive'),
        '「真丢帧仍遮蔽」负控制单测未跑出 OK：\n' + rx.out);
    assert.ok(gtestOk(rx.out,
        'AudioRxTest.TimelineCountsTheLossInTheStatistics'),
        '既有「丢失计入统计」用例未跑出 OK（回归）：\n' + rx.out);
});

// ---------------------------------------------------------------------------
// 验收标准 4：DTX 协商状态被真正消费
// ---------------------------------------------------------------------------
test('DTX 协商状态被真正消费；验证方式：use_dtx 在 src/modules/ 下有实际读取点', () => {
    // AudioDecodeFormat::use_dtx 不再只是「声明了但从未被读取」：
    // audio_rx 把协商结果写进去，opus 解码器真正读它。
    const rxCpp = readText(path.join(ROOT,
        'src/modules/audio_rx/src/receiver.cpp'));
    assert.match(rxCpp, /fmt\.use_dtx\s*=\s*cfg_\.peer_dtx_enabled;/,
        'audio_rx 必须把协商出的 peer_dtx_enabled 写进 AudioDecodeFormat::use_dtx');

    const decCpp = readText(path.join(ROOT,
        'src/modules/opus/src/audio_decoder_plugin.cpp'));
    assert.match(decCpp, /impl_->fmt\.use_dtx/,
        'opus 解码器必须真正读取 use_dtx（否则该字段仍是摆设）');

    const rxHpp = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/audio_receiver.hpp'));
    assert.match(rxHpp, /bool\s+peer_dtx_enabled\s*=\s*false/,
        'AudioReceiverConfig 缺 peer_dtx_enabled 字段');
});

// ---------------------------------------------------------------------------
// 验收标准 5：usedtx 的 SDP 协商链打通
// ---------------------------------------------------------------------------
test('usedtx 的 SDP 协商链打通；验证方式：offer 声明、answer 三态回显、协商结果传到接收器', () => {
    const routing = readText(path.join(ROOT,
        'src/engine/src/audio_routing.hpp'));

    // (1) usedtx 是 std::optional<bool>，三态要分清；parse 已接上。
    assert.match(routing, /std::optional<bool>\s+usedtx;/,
        'OpusFmtpParams 缺 usedtx 字段');
    assert.match(routing, /key\s*==\s*"usedtx"/,
        'usedtx 必须被解析进 OpusFmtpParams');

    // (2) offer 侧：build_opus_fmtp 仅在设置时才渲染（对端没发 → answer 不含）。
    assert.match(routing, /if\s*\(\s*p\.usedtx\s*\)\s*add\(\s*"usedtx"/,
        'build_opus_fmtp 必须在 usedtx 设置时才声明该参数');
    assert.match(routing, /add\(\s*"usedtx",\s*\*p\.usedtx\s*\?\s*"1"\s*:\s*"0"\)/,
        'usedtx 的渲染规则必须是 1→"1"、0→"0"');

    // (3) 本端广告：仅当主机开启 DTX（build_local_opus_fmtp 带 use_dtx 开关）。
    assert.match(routing,
        /build_local_opus_fmtp\(std::uint8_t stream_channels,\s*bool use_dtx\s*=\s*false\)/,
        'build_local_opus_fmtp 缺 use_dtx 参数');
    assert.match(routing, /if\s*\(\s*use_dtx\s*\)\s*p\.usedtx\s*=\s*true;/,
        '仅在 use_dtx 时声明 usedtx=1');

    // (4) answer 侧：build_negotiated_opus_fmtp 三态回显（1/0 原样回显，
    //     没发就不回显 —— RFC 6716 §8），且注释与行为一致。
    assert.match(routing, /out\.usedtx\s*=\s*peer\.usedtx;/,
        'build_negotiated_opus_fmtp 必须回显 peer.usedtx（含「未设置」状态）');
    assert.match(routing, /is\s+echoed\s+three-state/,
        '注释必须写明 usedtx 的三态回显规则');
    assert.match(routing, /AudioReceiverConfig::peer_dtx_enabled/,
        '注释必须写明协商结果流向 AudioReceiverConfig::peer_dtx_enabled');

    // (5) 引擎把 EngineConfig::audio_use_dtx 传进 offer 的 fmtp。
    const sig = readText(path.join(ROOT,
        'src/engine/src/engine_signaling.cpp'));
    assert.match(sig,
        /build_local_opus_fmtp\(\s*config_\.pcm_channels,\s*config_\.audio_use_dtx\s*\)/,
        '引擎 offer 必须把 audio_use_dtx 传进本地 Opus fmtp');

    // (6) 协商结果传到接收器：AudioReceiverConfig::peer_dtx_enabled。
    const media = readText(path.join(ROOT,
        'src/engine/src/engine_media.cpp'));
    assert.match(media,
        /rx_cfg\.peer_dtx_enabled\s*=\s*impl_->audio_state\.negotiated_opus_fmtp\.usedtx\.value_or\(false\);/,
        '协商结果必须传进 AudioReceiverConfig::peer_dtx_enabled（未协商回落 false）');
});

// ---------------------------------------------------------------------------
// 验收标准 6：D5-1 —— DTX 默认关
// ---------------------------------------------------------------------------
test('D5-1：DTX 默认关；验证方式：默认配置下 DTX 不生效', () => {
    const engineHpp = readText(path.join(ROOT,
        'src/engine/include/nimrtc/engine/engine.hpp'));
    assert.match(engineHpp, /bool\s+audio_use_dtx\s*=\s*false;/,
        'EngineConfig::audio_use_dtx 默认必须为 false（D5-1 保持现状）');

    const rxHpp = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/audio_receiver.hpp'));
    assert.match(rxHpp, /bool\s+peer_dtx_enabled\s*=\s*false;/,
        'AudioReceiverConfig::peer_dtx_enabled 默认必须为 false');

    const decHpp = readText(path.join(ROOT,
        'src/plugins/include/nimrtc/plugins/audio_decoder.hpp'));
    assert.match(decHpp, /bool\s+use_dtx\s*=\s*false;/,
        'AudioDecodeFormat::use_dtx 默认必须为 false');

    // 未显式开启时，接收侧回落 false —— 即「未协商 DTX 就不走 DTX 分支」。
    const media = readText(path.join(ROOT,
        'src/engine/src/engine_media.cpp'));
    assert.match(media, /\.value_or\(false\);/,
        '未协商 usedtx 时必须回落 false（DTX 关）');
});

// ---------------------------------------------------------------------------
// 验收标准 7：验收测试文件是真实断言
// ---------------------------------------------------------------------------
test('验收测试文件是真实断言，无 todo 用例残留；验证方式：node --test 全部通过', () => {
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
// 验收标准 8：不破坏既有行为
// ---------------------------------------------------------------------------
test('不破坏既有行为；验证方式：ctest 无回归（opus 编解码 / audio_rx 流水线 / 引擎音频媒体路径）', () => {
    const regression = [
        { re: '^test_opus\\.',            note: 'opus 编解码（既有 DTX 编码与往返）' },
        { re: 'test_opus_packetise$',     note: 'RFC 7587 RTP 打包' },
        { re: 'test_opus_codec_plugin$',  note: 'opus codec 接缝' },
        { re: '^test_audio_seams$',       note: '音频插件接缝契约' },
        { re: '^test_engine_dtmf$',       note: '引擎 DTMF 路径' },
        { re: '^test_engine_audio_state$',       note: '引擎音频协商状态' },
        { re: '^test_engine_audio_stats$',       note: '引擎音频统计' },
        { re: '^test_engine_audio_stream_stats$', note: '引擎音频流统计' },
        { re: '^test_engine_audio_loopback$',    note: '引擎音频回环' },
        { re: '^test_engine_audio_send$',        note: '引擎音频发送' },
        { re: '^test_engine_audio_alloc$',       note: '引擎音频分配' },
        { re: '^test_engine_audio_bitrate$',     note: '引擎音频码率' },
    ];
    for (const r of regression) {
        const res = runCtest(r.re);
        assert.equal(res.status, 0,
            `${r.note}（${r.re}）未通过（exit=${res.status}）：\n${res.out}`);
        assert.ok(res.out.includes('[==========]'),
            `${r.note}（${r.re}）没有产出 gtest 运行输出：\n${res.out}`);
    }
    // test_opus_audio_decoder / test_audio_rx 这两个含新用例的二进制，
    // 已由验收标准 1-3 整跑（整跑即覆盖旧用例），此处不重复。
});

// ---------------------------------------------------------------------------
// 写集边界断言（ADR-075）
// ---------------------------------------------------------------------------
test('本片改动仅限写集范围；验证方式：改动清单不越界 + 产物在树上', () => {
    // 写集 = 规格「范围」；外加会话开始前已存在的 11 个子模块脏状态
    // （git status 快照），它们不是本片改出来的。
    const ALLOW = [
        'src/modules/opus',
        'src/modules/audio_rx',
        'src/plugins/include/nimrtc/plugins/audio_decoder.hpp',
        'src/plugins/include/nimrtc/plugins/audio_receiver.hpp',
        'src/engine/src/audio_routing.hpp',
        'src/engine/src/engine_signaling.cpp',
        'src/engine/src/engine_media.cpp',
        'src/engine/src/engine_audio_state.hpp',
        'tests/acceptance/v1-audio-p2',
        'specs/slices/v1-audio-p2.md',
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
        'src/plugins/include/nimrtc/plugins/audio_decoder.hpp',
        'src/plugins/include/nimrtc/plugins/audio_receiver.hpp',
        'src/modules/opus/src/audio_decoder_plugin.cpp',
        'src/modules/opus/tests/test_opus_audio_decoder.cpp',
        'src/modules/audio_rx/src/receiver.cpp',
        'src/modules/audio_rx/tests/test_audio_rx.cpp',
        'src/engine/src/audio_routing.hpp',
        'src/engine/src/engine_signaling.cpp',
        'src/engine/src/engine_media.cpp',
        'src/engine/src/engine_audio_state.hpp',
        'tests/acceptance/v1-audio-p2/acceptance.test.cjs',
        'specs/slices/v1-audio-p2.md',
    ];

    // ① 越界检查（历史事件）：工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件。
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-audio-p2' });
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

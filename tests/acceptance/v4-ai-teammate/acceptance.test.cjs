// 验收测试 —— 切片 v4-ai-teammate（本地小模型 AI 队友）
//
// 每条验收标准对应一条真实断言（`test`），无 todo / skip：
//   1. 决策路径上存在本地模型推理的调用点（静态检查推理调用存在）
//   2. 动作集合被限定为三类，集合外的动作被拒绝并计数（静态检查动作白名单与拒绝分支）
//   3. 每条决策的 frame_seq 与 capture_ts_us 非空且来自被观测帧
//      （单元测试构造一次决策，断言两个字段取自传入的帧观测而非当前时刻）
//   4. 回答里引用的帧是历史帧（单元测试在「引用旧帧」的场景下断言引用帧号小于当前帧号）
//   5. 模型不可用时报错并计数（静态检查模型缺失分支的错误输出与计数点）
//
// 验证方式分两层：对 `examples/demo-agent-realtime/ai_teammate.hpp` 与
// `demo-agent-teammate.cpp` 做结构断言（确定性），并运行门禁同款 build 树里注册的
// ctest 用例 `demo-agent-teammate_self_test`（确定性：固定种子，无头运行，
// exit=0 且报告五条标准全绿）。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT  = path.resolve(__dirname, '..', '..', '..');
const BUILD = path.join(ROOT, 'build');

const HPP = path.join(ROOT, 'examples/demo-agent-realtime/ai_teammate.hpp');
const CPP = path.join(ROOT, 'examples/demo-agent-realtime/demo-agent-teammate.cpp');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, 'utf8'); }
function exists(p)   { return fs.existsSync(p); }

/** 跑 ctest 用例（锚定精确名称），返回退出码 + 完整输出。惰性：只跑一次。 */
let _selfTest = null;
function selfTestRun() {
    if (_selfTest) return _selfTest;
    const r = spawnSync('ctest',
        ['--test-dir', BUILD, '--output-on-failure', '-C', 'Debug',
         '-R', '^demo-agent-teammate_self_test$', '-V'],
        { cwd: ROOT, encoding: 'utf8', timeout: 240000 });
    _selfTest = { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
    return _selfTest;
}

// ---------------------------------------------------------------------------
// 验收标准 1：决策路径上存在本地模型推理的调用点
// ---------------------------------------------------------------------------
test('决策路径存在本地模型推理调用点（非 mock 规则、非云端 API）', () => {
    const hpp = readText(HPP);
    const cpp = readText(CPP);

    // ---- 本地小模型本体：真实推理类 + 推理调用点 ---------------------------
    assert.match(hpp, /class LocalPolicyModel/, '缺少本地小模型类');
    assert.match(hpp, /RawOutput infer\(const FrameObservation& obs\)/,
        '模型缺少 infer() 推理调用点');
    assert.match(hpp, /model_\.infer\(obs\)/, '决策路径必须调用模型推理（infer 调用点）');
    // 推理是真实前向传播（矩阵乘 + ReLU + softmax），不是 if/else mock 规则。
    assert.match(hpp, /kW1/, '模型必须包含第一层权重（前向传播的矩阵乘）');
    assert.match(hpp, /std::exp\(/, '模型输出必须走 softmax（概率分布）');
    assert.match(hpp, /z > 0\.0f \? z : 0\.0f/, '模型隐藏层必须走 ReLU 激活');

    // ---- 决策路径在 teammate 上：decide() 内经模型产出 ---------------------
    assert.match(hpp, /Decision decide\(const FrameObservation& obs\)/,
        '决策路径（decide）必须接收帧观测');

    // ---- 运行验证：自检跑 100 次模型推理并报告推理耗时 ---------------------
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `demo-agent-teammate_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out,
        /\[teammate\] model: name=local_mlp_policy available=1 inference_point=infer/,
        `本地模型推理调用点未报告：\n${out}`);
    assert.match(out,
        /\[teammate\] inference_latency_us: decisions=100 p50_us=[0-9]+ max_us=[0-9]+/,
        `推理耗时统计（风险 1）缺失：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 2：动作集合限定为三类，集合外动作被拒绝并计数
// ---------------------------------------------------------------------------
test('动作白名单限定 move_left/move_right/stop 三类，集合外动作被拒绝并计数', () => {
    const hpp = readText(HPP);

    // ---- 动作白名单：恰好三类 -----------------------------------------------
    assert.match(hpp, /enum class Action : std::uint8_t/, '缺少窄动作枚举');
    assert.match(hpp, /kMoveLeft = 0/, '动作集必须包含 move_left');
    assert.match(hpp, /kMoveRight = 1/, '动作集必须包含 move_right');
    assert.match(hpp, /kStop = 2/, '动作集必须包含 stop');
    const tokens = hpp.match(/kActionTokens\s*=\s*\{\s*"move_left",\s*"move_right",\s*"stop",?\s*\}/);
    assert.ok(tokens, '动作白名单必须是恰好 {move_left, move_right, stop}');
    assert.match(hpp, /is_narrow_action\(std::string_view token\)/,
        '缺少动作白名单校验函数');

    // ---- 拒绝分支 + 计数：集合外 token 如实拒绝，不改写、不静默 -----------------
    assert.match(hpp, /\+\+rejected_/, '拒绝必须计数（rejected 计数点）');
    assert.match(hpp, /d\.rejected = true/, '拒绝决策必须带 rejected 标记');
    assert.match(hpp, /"model emitted out-of-set action/,
        '拒绝时必须如实报告模型输出（不静默改写成别的动作）');

    // ---- 运行验证：两个集合外 token 被拒绝，无静默替代 -----------------------
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `demo-agent-teammate_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out,
        /\[teammate\] whitelist: allowed=3 move_left,move_right,stop rejected=2 no_silent_substitute=1 pass=1/,
        `动作白名单/拒绝分支未全绿：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 3：决策的 frame_seq 与 capture_ts_us 来自被观测帧而非当前时刻
// ---------------------------------------------------------------------------
test('决策携带的 frame_seq/capture_ts_us 来自被观测帧而非当前时刻', () => {
    const hpp = readText(HPP);

    // ---- 决策必须原样携带被观测帧的锚点（不是 now()） -----------------------
    assert.match(hpp, /d\.frame_seq = obs\.frame_seq/, '决策 frame_seq 必须取自帧观测');
    assert.match(hpp, /d\.capture_ts_us = obs\.capture_ts_us/,
        '决策 capture_ts_us 必须取自帧观测');
    // 帧观测本身带两个锚点字段（plan §5.2 契约）。
    assert.match(hpp, /std::int64_t frame_seq = 0;/, '帧观测缺少 frame_seq');
    assert.match(hpp, /std::int64_t capture_ts_us = 0;/, '帧观测缺少 capture_ts_us');

    // ---- 运行验证：构造一次决策，断言两字段取自传入帧观测且不是当前时刻 -------
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `demo-agent-teammate_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out,
        /\[teammate\] anchors: observed_frame=42 observed_ts=1710000000000000 decision_frame=42 decision_ts=1710000000000000 match=1 not_now=1 pass=1/,
        `决策锚点未取自被观测帧或取了当前时刻：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 4：时光作证的回答 100% 带 frame_seq + capture_ts，且引用已滚出画面
// 的旧帧（帧号小于当前帧号）
// ---------------------------------------------------------------------------
test('时光作证回答带 frame_seq+capture_ts 且引用旧帧（引用帧号小于当前帧号）', () => {
    const hpp = readText(HPP);

    // ---- 回答必须内嵌被引用帧的 frame_seq 与 capture_ts_us ------------------
    assert.match(hpp, /time_witness_answer/, '缺少时光作证回答接口');
    assert.match(hpp, /frame_seq=/, '回答必须嵌入 frame_seq');
    assert.match(hpp, /capture_ts_us=/, '回答必须嵌入 capture_ts_us');
    // 引用的必须是已滚出当前画面的帧，而不是当前帧。
    assert.match(hpp, /kScrollOutFrames/, '必须存在「滚出当前画面」的帧距判定');
    assert.match(hpp, /current\.frame_seq - kScrollOutFrames/,
        '引用帧必须按帧距判定为已滚出当前画面');

    // ---- 运行验证：构造「引用旧帧」场景，断言引用帧号小于当前帧号 ----------
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `demo-agent-teammate_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out,
        /\[teammate\] witness: referenced_frame=42 current_frame=50 older=1 has_ts=1 pass=1/,
        `时光作证回答未引用旧帧或未带锚点：\n${out}`);
    assert.match(out, /\[teammate\] witness_answer: .*frame_seq=42.*capture_ts_us=1710000000000000/,
        `回答文本必须实际包含 frame_seq 与 capture_ts_us：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 5：模型不可用时报错并计数，不静默退回 mock 规则
// ---------------------------------------------------------------------------
test('模型不可用时报错并计数；仅在显式 opt-in 时退回安全动作', () => {
    const hpp = readText(HPP);

    // ---- 模型缺失分支：错误输出 + 计数点 -------------------------------------
    assert.match(hpp, /bool available\(\) const/, '模型必须可查询可用性');
    assert.match(hpp, /\+\+unavailable_/, '模型缺失必须计数（unavailable 计数点）');
    assert.match(hpp, /model unavailable/, '模型缺失分支必须有明确错误输出');
    assert.match(hpp, /d\.unavailable = true/, '缺失决策必须带 unavailable 标记');
    assert.match(hpp, /allow_fallback/, '只有调用方显式 opt-in 才可退回到安全动作');
    assert.match(hpp, /d\.fallback = true/, '显式退回必须带 fallback 标记');

    // ---- 运行验证：缺省无动作 + 错误已报告 + 显式 opt-in 才 fallback ----------
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `demo-agent-teammate_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out,
        /\[teammate\] unavailable: default_no_action=1 error_reported=1 unavailable_count=1 fallback_opt_in=stop fallback_count=1 pass=1/,
        `模型缺失分支未报错/未计数/静默退回：\n${out}`);
});

// ---------------------------------------------------------------------------
// 附加：决策输入是画面语义（该帧上有什么），不是「用户说了什么」
// ---------------------------------------------------------------------------
test('决策输入是帧语义（帧上有什么），不是用户话语', () => {
    const hpp = readText(HPP);
    // 帧观测必须携带画面内容字段（每车道威胁/危险度），决策 reason 描述帧内容。
    assert.match(hpp, /struct FrameObservation/, '缺少帧观测结构');
    assert.match(hpp, /threat/, '帧观测必须携带画面语义（threat）');
    assert.match(hpp, /avatar_lane/, '帧观测必须携带画面位置（avatar_lane）');
    assert.match(hpp, /reason_for\(const FrameObservation& obs, Action a\)/,
        '决策理由必须基于帧观测生成');
    // 决策输入没有任何「用户话语」通道：decide 只收帧观测。
    const decide = hpp.match(/Decision decide\(const FrameObservation& obs\)/);
    assert.ok(decide, 'decide 必须只接收帧观测作为输入');
});

// ---------------------------------------------------------------------------
// 写集边界：本片只改 examples/demo-agent-realtime/** 与 v4 验收目录
// ---------------------------------------------------------------------------
test('本片改动仅限写集（examples/demo-agent-realtime/** 与 tests/acceptance/v4-ai-teammate/）', () => {
    const HPP_PATH = 'examples/demo-agent-realtime/ai_teammate.hpp';
    const CPP_PATH = 'examples/demo-agent-realtime/demo-agent-teammate.cpp';

    // 追踪文件改动：只允许 examples/demo-agent-realtime/ 下的文件。
    const diff = spawnSync('git', ['diff', '--name-only'], { cwd: ROOT, encoding: 'utf8' });
    assert.equal(diff.status, 0, 'git diff --name-only 失败');
    const changed = (diff.stdout || '').split(/\r?\n/).filter(Boolean);
    assert.ok(changed.length >= 1, '写集内必须存在已追踪文件的改动（CMakeLists.txt）');
    for (const p of changed) {
        assert.ok(p.startsWith('examples/demo-agent-realtime/'),
            `追踪文件改动超出写集：${p}`);
    }
    assert.ok(changed.includes('examples/demo-agent-realtime/CMakeLists.txt'),
        '本片必须注册 demo-agent-teammate（CMakeLists.txt 应有改动）');

    // 未追踪文件：切片规格 + v4 新源码 + v4 验收目录。
    const status = spawnSync('git', ['status', '--porcelain'], { cwd: ROOT, encoding: 'utf8' });
    assert.equal(status.status, 0, 'git status --porcelain 失败');
    const untracked = (status.stdout || '')
        .split(/\r?\n/).filter(Boolean)
        .filter(l => l.startsWith('?? '))
        .map(l => l.slice(3));
    for (const p of untracked) {
        const ok = p === 'specs/slices/v4-ai-teammate.md' ||
                   p.startsWith('examples/demo-agent-realtime/') ||
                   p.startsWith('tests/acceptance/v4-ai-teammate/');
        assert.ok(ok, `出现意外的未追踪文件：${p}`);
    }
    assert.ok(untracked.includes(HPP_PATH), `${HPP_PATH} 必须作为本片工件存在`);
    assert.ok(untracked.includes(CPP_PATH), `${CPP_PATH} 必须作为本片工件存在`);
    assert.ok(exists(path.join(ROOT, HPP_PATH)), `${HPP_PATH} 缺失`);
    assert.ok(exists(path.join(ROOT, CPP_PATH)), `${CPP_PATH} 缺失`);
});

// ---------------------------------------------------------------------------
// 验收标准（整体）：门禁同款命令跑的自检全绿
// ---------------------------------------------------------------------------
test('demo-agent-teammate_self_test 整题通过（exit=0 + self-test PASS）', () => {
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `exit=${status}：\n${out}`);
    assert.match(out, /\[teammate\] self-test PASS/, `自检未 PASS：\n${out}`);
});

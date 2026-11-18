// 验收测试 —— 切片 v2-ai-decision（实时AI游戏决策处理）
//
// 每条验收标准对应一条真实断言（`test`），骨架里的占位符已全部替换为真实断言：
//   1. 决策响应时间 ≤ 50ms，100 次连续决策逐次记录耗时（性能监控工具）
//   2. 决策准确率 ≥ 90%，100 次 AI 决策与人工专家决策对比
//   3. 支持 ≥ 3 种决策策略，存在可配置的策略切换逻辑
//   4. debug 模式下输出完整决策过程日志
//
// 验证方式分两层：对 `examples/demo-agent-realtime/` 源码做结构断言（确定性），
// 并运行门禁同款 build 树里注册的 ctest 用例 `demo-agent-realtime_self_test`
// （确定性：固定种子，无头运行，exit=0 且报告四条标准全绿）。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT  = path.resolve(__dirname, '..', '..', '..');
const BUILD = path.join(ROOT, 'build');

const HPP = path.join(ROOT, 'examples/demo-agent-realtime/ai_decision.hpp');
const CPP = path.join(ROOT, 'examples/demo-agent-realtime/demo-agent-realtime.cpp');

// 写集边界判据：四个套件共用同一份实现（v5-writeset-acceptance/）。
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, 'utf8'); }

/** 跑一条 ctest 用例（锚定精确名称），返回退出码 + 完整输出。惰性：只跑一次。 */
let _selfTest = null;
function selfTestRun() {
    if (_selfTest) return _selfTest;
    const r = spawnSync('ctest',
        ['--test-dir', BUILD, '--output-on-failure', '-C', 'Debug',
         '-R', '^demo-agent-realtime_self_test$', '-V'],
        { cwd: ROOT, encoding: 'utf8', timeout: 240000 });
    _selfTest = { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
    return _selfTest;
}

// ---------------------------------------------------------------------------
// 验收标准 1：决策响应时间 ≤ 50ms，100 次连续决策逐次记录耗时
// ---------------------------------------------------------------------------
test('决策响应时间≤50ms：100次连续决策用性能监控工具逐次记录耗时', () => {
    const hpp = readText(HPP);
    const cpp = readText(CPP);

    // ---- 性能监控工具：逐次记录每次决策耗时 -------------------------------
    assert.match(hpp, /class LatencyInstrument/, '缺少逐次记录耗时的性能监控工具');
    assert.match(hpp, /void record\(/,           'LatencyInstrument 缺少 record()');
    assert.match(hpp, /percentile_ns\(/,         '缺少 p50/p95 统计');
    assert.match(hpp, /max_ns\(\)/,              '缺少 max 统计');
    assert.match(hpp, /all_within_us\(/,         '缺少「是否全部在预算内」判定');

    // ---- 50ms 预算常量 = 50000us；100 次连续决策默认值 ---------------------
    assert.match(cpp, /kLatencyBudgetUs\s*=\s*50000/, '50ms 预算（50000us）缺失');
    assert.match(cpp, /kDefaultDecisionCount\s*=\s*100/, '100 次连续决策默认值缺失');

    // ---- 100 次连续决策逐次喂给引擎并记录（一次一条，非批量） --------------
    assert.match(cpp, /for \(int i = 0; i < n; \+\+i\) \{\s*engine\.decide\(latency_sc\.next\(\)\)/,
        '100 次连续决策循环必须逐次调用 decide() 并记录耗时');
    // 冷启动不算数：先热身一次再清空仪器，保证 100 条都是真实决策耗时。
    assert.match(cpp, /engine\.reset_latency\(\);\s*\/\/ drop the warmup sample/,
        '热身样本必须从统计中剔除');

    // ---- 运行验证：ctest 自检里 100 次全部 ≤ 50ms --------------------------
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `demo-agent-realtime_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out,
        /latency: decisions=100 p50_us=[0-9.]+ p95_us=[0-9.]+ max_us=[0-9.]+ limit_us=50000 all_within_limit=1/,
        `100 次决策逐次记录的报告缺失或未全绿：\n${out}`);
    assert.ok(!out.includes('FAIL: latency'),
        `延迟预算超标的 FAIL 行不应出现：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 2：决策准确率 ≥ 90%，100 次 AI 决策与人工专家决策对比
// ---------------------------------------------------------------------------
test('决策准确率≥90%：100次AI决策与人工专家决策结果对比', () => {
    const hpp = readText(HPP);
    const cpp = readText(CPP);

    // ---- 人工专家规则：对比基准必须存在 -----------------------------------
    assert.match(hpp, /struct ExpertOracle/, '缺少人工专家（对比基准）');
    assert.match(hpp, /ExpertOracle::decide/, '缺少专家决策规则');

    // ---- 100 次 AI 决策逐条与专家对比，逐条计数正确数 ----------------------
    assert.match(cpp,
        /for \(int i = 0; i < n; \+\+i\) \{\s*const GameState s = acc_sc\.next\(\);/,
        '100 次对比循环缺失');
    assert.match(cpp, /const Action expert = ExpertOracle::decide\(s\)/, '缺少专家决策');
    assert.match(cpp, /const Action ai = engine\.decide\(s\)/, '缺少 AI 决策');
    assert.match(cpp, /if \(ai == expert\) \+\+correct;/, '缺少正确数计数');

    // ---- 90% 阈值 -----------------------------------------------------------------
    assert.match(cpp, /kAccuracyThreshold\s*=\s*90\.0/, '90% 准确率阈值缺失');

    // ---- 运行验证：100 次对比 rate_pct ≥ 90，且 pass=1 ----------------------
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `demo-agent-realtime_self_test 未通过（exit=${status}）：\n${out}`);
    const m = out.match(/accuracy: decisions=100 correct=(\d+) rate_pct=([0-9.]+) threshold_pct=90\.0 pass=1/);
    assert.ok(m, `准确率对比报告缺失或未 pass：\n${out}`);
    assert.equal(Number(m[1]), 100, `对比次数应为 100（correct=${m[1]}）：\n${out}`);
    assert.ok(Number(m[2]) >= 90.0,
        `AI 决策准确率 ${m[2]}% < 90%：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 3：支持 ≥ 3 种决策策略，存在可配置的策略切换逻辑
// ---------------------------------------------------------------------------
test('支持至少3种决策策略，存在可配置的策略切换逻辑', () => {
    const hpp = readText(HPP);
    const cpp = readText(CPP);

    // ---- ≥ 3 种策略：三种具体策略类 + 注册表 ---------------------------------
    assert.match(hpp, /class HeuristicStrategy/,  '缺少 HeuristicStrategy');
    assert.match(hpp, /class ReactiveStrategy/,   '缺少 ReactiveStrategy');
    assert.match(hpp, /class PredictiveStrategy/, '缺少 PredictiveStrategy');
    assert.match(hpp, /available_strategies\(\)/, '缺少策略列表');
    assert.match(hpp, /return \{"heuristic", "reactive", "predictive"\}/,
        '策略注册表必须恰好注册 heuristic/reactive/predictive 三种');

    // ---- 可配置的策略切换：工厂 + 运行时 switch + CLI 选择 --------------------
    assert.match(hpp, /make_strategy\(std::string_view name\)/, '缺少策略工厂');
    assert.match(hpp, /bool switch_strategy\(std::string_view name\)/, '缺少运行时切换接口');
    assert.match(cpp, /--strategy/, '缺少 --strategy 启动选择开关');
    assert.match(cpp, /--strategies/, '缺少 --strategies 列表开关');
    // 切换处资源释放检查点（plan 风险 2 的落点）。字符串字面量按 100 列折行，
    // 所以分两段断言。
    assert.match(hpp, /strategy switch %s -> %s: previous strategy/,
        '切换处缺少资源释放检查点日志');
    assert.match(hpp, /released \(no leaked resources\)/, '检查点缺少释放声明');

    // ---- 运行验证：3 种策略可列出、可切换（含释放检查点） --------------------
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `demo-agent-realtime_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out, /strategies: 3 \(heuristic,reactive,predictive\)/,
        `策略列表未报告 3 种：\n${out}`);
    assert.match(out, /switch: ok \(release checkpoints ok=1\)/,
        `策略切换（含释放检查点）未通过：\n${out}`);
    assert.ok((out.match(/\[checkpoint\] strategy switch /g) || []).length >= 3,
        `切换处资源释放检查点应至少出现 3 次：\n${out}`);

    // ---- 三种策略是真实可度量的不同策略：逐策略准确率均 ≥90% 且有差异 --------
    const byStrategy = [...out.matchAll(/accuracy_by_strategy: (\w+)=([0-9.]+)/g)];
    assert.equal(byStrategy.length, 3, `应有 3 条逐策略准确率报告：\n${out}`);
    const names  = byStrategy.map(m => m[1]);
    const values = byStrategy.map(m => Number(m[2]));
    assert.deepEqual(names.sort(), ['heuristic', 'predictive', 'reactive'],
        `逐策略报告的策略名应覆盖注册的三种：\n${out}`);
    for (const v of values) {
        assert.ok(v >= 90.0, `策略准确率 ${v}% < 90%：\n${out}`);
    }
    assert.ok(new Set(values).size >= 2,
        `三种策略的准确率应出现至少两个不同值（证明是可区分的策略）：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准 4：debug 模式下输出完整的决策过程日志
// ---------------------------------------------------------------------------
test('debug模式输出完整决策过程日志', () => {
    const hpp = readText(HPP);
    const cpp = readText(CPP);

    // ---- debug 开关 + 完整决策过程逐条落盘 ------------------------------------
    assert.match(hpp, /void set_debug\(bool on\)/, '缺少 debug 开关');
    assert.match(hpp, /std::int64_t trace_count\(\)/, '缺少 trace 计数');
    assert.match(hpp, /\[decision\] seq=%lld/, '决策过程日志行必须含 [decision] 标记');
    assert.match(hpp, /\[state\] seq=%lld/, '状态观察日志行必须含 [state] 标记');
    assert.match(cpp, /--debug/, '缺少 --debug 命令行开关');

    // ---- 运行验证：debug 模式 100 条决策 trace 齐全，逐条可见 -----------------
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `demo-agent-realtime_self_test 未通过（exit=${status}）：\n${out}`);
    assert.match(out, /debug: traced_records=100 complete=1/,
        `完整决策过程日志（100 条齐全）未报告：\n${out}`);
    const traced = (out.match(/\[decision\] seq=\d+/g) || []).length;
    assert.ok(traced >= 100,
        `ctest 输出里可见的 [decision] 决策过程日志应不少于 100 条（实际 ${traced}）：\n${out}`);
});

// ---------------------------------------------------------------------------
// 验收标准（整体）：门禁同款命令跑的自检全绿
// ---------------------------------------------------------------------------
test('demo-agent-realtime_self_test 整题通过（exit=0 + self-test PASS）', () => {
    const { status, out } = selfTestRun();
    assert.equal(status, 0, `exit=${status}：\n${out}`);
    assert.match(out, /self-test PASS/, `自检未 PASS：\n${out}`);
});

// ---------------------------------------------------------------------------
// 写集边界：本片只改 examples/CMakeLists.txt + 新增 demo-agent-realtime/
// ---------------------------------------------------------------------------
test('本片改动仅限写集（examples/demo-agent-realtime/** 与 examples/CMakeLists.txt）', () => {
    // 写集边界判据（v5-writeset-acceptance/writeset-boundary.cjs，四个套件共用）：
    // 三源并集 —— 工作区未提交差异 ∪ 本片提交差异 ∪ 未跟踪文件。断言从
    // 「读工作区差异」改为「读提交历史」后，在已提交的干净树上同样成立（D-68），
    // 越界检测不削弱：并集里任何落在写集外的路径仍判 failed。
    const verdict = ws.evaluateSliceWriteSet({ root: ROOT, sliceId: 'v2-ai-decision' });
    assert.deepEqual(verdict.violations, [],
        `改动超出写集：${verdict.violations.join(', ')}`);
    assert.deepEqual(verdict.missing, [],
        `本片产物缺失（写集内必须存在改动）：${verdict.missing.join(', ')}`);
    assert.ok(verdict.changed.length >= 1,
        '本片改动必须非空（工作区差异 ∪ 提交历史 ∪ 未跟踪文件的并集）');
});

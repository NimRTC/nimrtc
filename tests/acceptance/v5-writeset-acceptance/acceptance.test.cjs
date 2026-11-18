// 验收测试 —— 切片 v5-writeset-acceptance（写集边界断言改读提交历史）
//
// D-68：四个套件（v1-video-processing / v2-ai-decision / v4-ai-teammate /
// v4-voice-agent）的写集边界断言读 `git diff --name-only`（工作区 vs 暂存区），
// 已提交的干净树上该差异为空，断言必然失败。本片把这些断言改为读提交历史，
// 三源并集判「本片 agent 改了什么」：工作区未提交差异 ∪ 本片提交差异 ∪ 未跟踪文件，
// 四处共用同一份判据实现（writeset-boundary.cjs）。
//
// 每条验收标准对应一条真实断言（`test`），无 todo / skip：
//   1. 已提交干净树 / 未提交工作区两种状态下四个套件都通过（跑四条验收命令，退出码 0）
//   2. 判据的三源并集把「提交历史 ∪ 工作区差异 ∪ 未跟踪文件」都算进本片改动
//   3. 越界检测不削弱：并集里出现写集外路径时判「越界」失败
//   4. 四个套件的写集边界用例共用同一份判据实现（静态检查 require + 调用点）
//   5. 判据确定性：同一仓库状态连跑两次结果一致
//   6. 本片改动仅限验收范围（tests/acceptance/** + 本片规格）
//   7. 判据实现不触网、不调模型（静态检查）
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ROOT  = path.resolve(__dirname, '..', '..', '..');
const BUILD = path.join(ROOT, 'build');
const SHARED = path.join(__dirname, 'writeset-boundary.cjs');

const ws = require('./writeset-boundary.cjs');

const FOUR_SLICES = [
    'v1-video-processing',
    'v2-ai-decision',
    'v4-ai-teammate',
    'v4-voice-agent',
];

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, 'utf8'); }
function exists(p)   { return fs.existsSync(p); }

/** 在 root 下跑命令，成功时返回去尾换行的 stdout。 */
function run(root, cmd, args) {
    const r = spawnSync(cmd, args, { cwd: root, encoding: 'utf8' });
    if (r.status !== 0) {
        throw new Error(
            `${cmd} ${args.join(' ')} 失败（exit=${r.status}）：${r.stderr || r.stdout || ''}`);
    }
    return (r.stdout || '').trim();
}

/** 造一个临时 git 仓库（test 结束自动删除）。 */
function makeTempRepo(t) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'v5-writeset-'));
    t.after(() => {
        try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* noop */ }
    });
    run(dir, 'git', ['init', '-q']);
    run(dir, 'git', ['config', 'user.email', 'v5-test@example.com']);
    run(dir, 'git', ['config', 'user.name', 'V5 Test']);
    return dir;
}

// ---------------------------------------------------------------------------
// 验收标准 4：四个套件共用同一份判据实现
// ---------------------------------------------------------------------------
test('四个套件的写集边界用例共用同一份判据实现（标准 4）', () => {
    for (const id of FOUR_SLICES) {
        const src = readText(path.join(ROOT, 'tests/acceptance', id, 'acceptance.test.cjs'));
        assert.match(src,
            /require\('\.\.\/v5-writeset-acceptance\/writeset-boundary\.cjs'\)/,
            `${id} 未 require 共用判据实现（writeset-boundary.cjs）`);
        assert.match(src,
            /evaluateSliceWriteSet\(\{ root: ROOT, sliceId: 'v\d+[^']*' \}\)/,
            `${id} 未调用 evaluateSliceWriteSet（判据未共用）`);
        assert.ok(!src.includes('git diff --name-only') && !src.includes('git status'),
            `${id} 仍残留「读工作区差异」的本地判据（判据漂移）`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 1 落点：真实仓库里四片产物可从提交历史取到，且全部落在写集内
// ---------------------------------------------------------------------------
test('真实仓库：四片产物从提交历史取到且全部落在写集内（标准 1 落点）', () => {
    for (const id of FOUR_SLICES) {
        const v = ws.evaluateSliceWriteSet({ root: ROOT, sliceId: id });
        assert.ok(v.sliceCommits.length >= 1,
            `${id} 在提交历史里找不到本片自己的提交（写集边界断言无法成立）`);
        assert.deepEqual(v.violations, [],
            `${id} 改动超出写集：${v.violations.join(', ')}`);
        assert.deepEqual(v.missing, [],
            `${id} 写集内必要产物缺失：${v.missing.join(', ')}`);
        assert.ok(v.changed.length >= 1,
            `${id} 改动并集为空（提交历史 ∪ 工作区 ∪ 未跟踪）`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 2（机制）：三源并集把提交历史 ∪ 工作区差异 ∪ 未跟踪文件都算进来
// ---------------------------------------------------------------------------
test('判据三源并集：提交历史 ∪ 工作区差异 ∪ 未跟踪文件（标准 2 机制）', (t) => {
    const dir = makeTempRepo(t);

    // 基线提交（本片开始时的树）。
    fs.writeFileSync(path.join(dir, 'base.txt'), 'base');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'base commit']);
    const baseHash = run(dir, 'git', ['rev-parse', 'HEAD']);

    // 本片签收提交（feat(<id>): slice approved 标题）。
    fs.writeFileSync(path.join(dir, 'mod.txt'), 'one');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'feat(v1-fake): slice approved']);

    // 本片后续提交（带 Refs-Slice trailer）。
    fs.writeFileSync(path.join(dir, 'trailer.txt'), 'two');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'follow-up\n\nRefs-Slice: v1-fake']);

    // 工作区未提交改动 + 未跟踪文件。
    fs.writeFileSync(path.join(dir, 'mod.txt'), 'changed');
    fs.writeFileSync(path.join(dir, 'new-untracked.txt'), 'untracked');

    const r = ws.collectSliceChanges({ root: dir, sliceId: 'v1-fake' });
    assert.equal(r.base, baseHash, '基点应为 feat(<id>): slice approved 提交的父提交');
    assert.equal(r.sliceCommits.length, 2, '标题惯例与 trailer 两种 marker 都该识别');
    assert.deepEqual(r.committed, ['mod.txt', 'trailer.txt'], '提交历史来源应包含两片提交的差异');
    assert.deepEqual(r.workingTree, ['mod.txt'], '工作区差异来源应包含未提交改动');
    assert.deepEqual(r.untracked, ['new-untracked.txt'], '未跟踪文件来源应包含新建文件');
    assert.deepEqual(r.changed, ['mod.txt', 'new-untracked.txt', 'trailer.txt'],
        '三源并集应覆盖提交历史 ∪ 工作区差异 ∪ 未跟踪文件');

    // 未提交状态下（工作区 + 未跟踪也在并集里）判写集：写集内改动通过。
    const dirty = ws.evaluateChanged({
        changed: r.changed,
        allow: ['mod.txt', 'trailer.txt', 'new-untracked.txt'],
        required: ['mod.txt'],
    });
    assert.deepEqual(dirty, { ok: true, violations: [], missing: [] },
        '未提交状态写集内改动应通过（标准 2）');

    // 把全部改动提交后：干净树状态下提交历史来源仍非空，写集内改动仍通过。
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'commit everything']);
    const clean = ws.collectSliceChanges({ root: dir, sliceId: 'v1-fake' });
    assert.deepEqual(clean.workingTree, [], '干净树上工作区差异应为空');
    assert.deepEqual(clean.untracked, [], '干净树上未跟踪文件应为空');
    assert.deepEqual(clean.changed, ['mod.txt', 'trailer.txt'],
        '干净树状态下提交历史来源必须非空（D-68 的核心修复）');
    const cleanCheck = ws.evaluateChanged({
        changed: clean.changed,
        allow: ['mod.txt', 'trailer.txt'],
        required: ['mod.txt'],
    });
    assert.deepEqual(cleanCheck, { ok: true, violations: [], missing: [] },
        '已提交干净树状态写集内改动应通过（标准 1）');
});

// ---------------------------------------------------------------------------
// 验收标准 3：越界检测不削弱
// ---------------------------------------------------------------------------
test('越界检测不削弱：并集里出现写集外路径时判「越界」（标准 3）', (t) => {
    const dir = makeTempRepo(t);

    fs.writeFileSync(path.join(dir, 'base.txt'), 'base');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'base commit']);

    // 本片提交：写集内文件。
    fs.mkdirSync(path.join(dir, 'examples', 'demo-agent-realtime'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'examples/demo-agent-realtime/ai_decision.hpp'), 'ok');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'feat(v2-fake): slice approved']);

    // 本片提交：写集外文件（模拟越界改动混进本片提交历史）。
    fs.mkdirSync(path.join(dir, 'src', 'engine'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'src/engine/engine_impl.hpp'), 'oops');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'oops\n\nRefs-Slice: v2-fake']);

    const r = ws.collectSliceChanges({ root: dir, sliceId: 'v2-fake' });
    const verdict = ws.evaluateChanged({
        changed: r.changed,
        allow: ['examples/demo-agent-realtime'],
        required: ['examples/demo-agent-realtime/ai_decision.hpp'],
    });
    assert.equal(verdict.ok, false, '写集外路径出现时必须判失败');
    assert.deepEqual(verdict.violations, ['src/engine/engine_impl.hpp'],
        '越界路径必须被如实列出');
    assert.deepEqual(verdict.missing, [], '写集内必要产物仍在时应视为不缺');
});

// ---------------------------------------------------------------------------
// 验收标准 5：判据确定性
// ---------------------------------------------------------------------------
test('判据确定性：同一仓库状态连跑两次结果一致（标准 5）', () => {
    const first = ws.evaluateSliceWriteSet({ root: ROOT, sliceId: 'v1-video-processing' });
    const second = ws.evaluateSliceWriteSet({ root: ROOT, sliceId: 'v1-video-processing' });
    assert.deepEqual(second, first, '同状态连跑两次必须逐字段一致（changed/提交集/越界/缺失）');
});

// ---------------------------------------------------------------------------
// 验收标准 7：判据实现不触网、不调模型
// ---------------------------------------------------------------------------
test('判据实现不触网、不调模型（标准 7）', () => {
    const src = readText(SHARED);
    for (const token of [
        'http://', 'https://',
        'fetch(', 'XMLHttpRequest', 'WebSocket',
        'require("http")', "require('http')",
        'require("https")', "require('https')",
        'require("net")', "require('net')",
        'child_process.exec(', 'child_process.execSync(',
        'process.env',
    ]) {
        assert.ok(!src.includes(token),
            `共用判据实现含网络/环境痕迹：${token}`);
    }
});

// ---------------------------------------------------------------------------
// 验收标准 6：本片改动仅限验收范围
// ---------------------------------------------------------------------------
test('本片改动仅限验收范围（tests/acceptance/** 与本片规格）（标准 6）', () => {
    const v = ws.evaluateSliceWriteSet({ root: ROOT, sliceId: 'v5-writeset-acceptance' });
    assert.deepEqual(v.violations, [],
        `本片改动超出验收范围：${v.violations.join(', ')}`);
    assert.deepEqual(v.missing, [],
        `本片必要产物缺失：${v.missing.join(', ')}`);
    assert.ok(v.changed.length >= 1, '本片改动必须非空');
});

// ---------------------------------------------------------------------------
// 验收标准 1 & 2（验证方式）：四种状态各跑一条验收命令，退出码均为 0
// ---------------------------------------------------------------------------
test('四个套件在未提交工作区/已提交干净树两种状态下都通过（标准 1/2 验证方式）', { timeout: 1800000 }, () => {
    assert.ok(exists(path.join(BUILD, 'CMakeCache.txt')),
        '门禁应先在 build 树构建（四个套件的 ctest 用例依赖构建产物）');
    // 门禁跑本套件时 node 测试运行器会给子进程设 NODE_TEST_CONTEXT=child-v8，
    // 嵌套 `node --test` 会被递归守卫直接跳过（不跑任何用例）——剥离它再派生。
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    for (const id of FOUR_SLICES) {
        const file = path.join(ROOT, 'tests/acceptance', id, 'acceptance.test.cjs');
        const r = spawnSync(process.execPath, ['--test', file],
            { cwd: ROOT, encoding: 'utf8', timeout: 1800000, env });
        assert.equal(r.status, 0,
            `${id} 套件未通过（exit=${r.status}）：\n${r.stdout}\n${r.stderr}`);
    }
});

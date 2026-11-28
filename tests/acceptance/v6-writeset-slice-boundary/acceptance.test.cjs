// 验收测试 —— 切片 v6-writeset-slice-boundary（共享判据的签收边界，ADR-076）
//
// 背景：共享写集判据（v5-writeset-acceptance/writeset-boundary.cjs）认「本片的
// 提交」只凭两样东西 —— 标题是 `feat(<id>): slice approved`，或提交带
// `Refs-Slice: <id>` trailer。两者都**不看提交发生在签收之前还是之后**。于是
// 签收之后写的**事务性**提交（`docs(plan): X is signed`，误带 Refs-Slice
// trailer）的文件会被算进本片的 `changed` → 假越界（N6 的 v1.4-plan.md 越界
// 就是这么来的）。
//
// 本片加一条**签收边界**：边界（合并提交 / 签收提交 / 最早的 `feat(<id>): ...`
// 提交，按此阶梯 fallback；三步都找不到 → null）及其**全部祖先**才算本片的工作，
// 边界之后的提交不算。过滤只发生在「来源 2：本片自己的提交」这一路；越界检查
// 的强度不变（签收**之前**的越界照旧被拦）。
//
// 每条验收标准对应一条真实断言（`test`），无 todo / skip：
//   1-4.  边界判定（Merge / approved / 最早 feat / null 不判红）
//   5-10. 六个被误伤的切片：changed 不再含 docs/plan/v1.4-plan.md
//   11-12.过滤没把本片真正的工作滤掉（v1-sdk-sample 的 14 个文件、
//         v1-install-tree-video-deps 的 cmake/NimRTCConfig.cmake.in）
//   13-15.越界检查不削弱：签收之前的越界仍被拦；签收之后的不算本片
//   16-17.判据确定性（连跑两次一致）+ 不触网 / 不调模型
//   18-19.ADR-076 与 CHANGELOG 记录本片
//   20.   本片改动仅限写集范围
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const SHARED = path.join(ROOT, 'tests/acceptance/v5-writeset-acceptance/writeset-boundary.cjs');

const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

// 被事务性提交误伤的 6 个已签字切片（签收之后有一个 docs(plan): ... 提交
// 误带 Refs-Slice trailer，把 docs/plan/v1.4-plan.md 算进了本片 changed）。
const AFFECTED = [
    'v1-sdk-sample',
    'v1-ice-recovery',
    'v1-path-change-report',
    'v1-turn-credentials',
    'v1-video-render-engine',
    'v1-turn-tls-trust',
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
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'v6-writeset-'));
    t.after(() => {
        try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* noop */ }
    });
    run(dir, 'git', ['init', '-q']);
    run(dir, 'git', ['config', 'user.email', 'v6-test@example.com']);
    run(dir, 'git', ['config', 'user.name', 'V6 Test']);
    return dir;
}

// ---------------------------------------------------------------------------
// 一、边界判定（验收标准 1-4）
// ---------------------------------------------------------------------------
test('边界判定：v1-sdk-sample = Merge slice 提交；v1-ice-recovery = 签收提交；'
    + 'v1-audio-device-engine = 最早的 feat；不存在的切片 → null（标准 1-4）', () => {
    // 标准 1：v1-sdk-sample 有合并提交 `Merge slice v1-sdk-sample`（阶梯 1）。
    assert.equal(
        ws.sliceBoundaryCommit(ROOT, 'v1-sdk-sample'),
        '657941be895896f472abcf247f486cf2a495f68e',
        'v1-sdk-sample 的边界应为 Merge slice v1-sdk-sample（阶梯 1）');

    // 标准 2：v1-ice-recovery 没有合并提交 → 签收提交（阶梯 2）。
    assert.equal(
        ws.sliceBoundaryCommit(ROOT, 'v1-ice-recovery'),
        '8abb815a8ee9febc1df6e22a7810f31c6285435e',
        'v1-ice-recovery 的边界应为 feat(v1-ice-recovery): slice approved（阶梯 2）');

    // 标准 3：v1-audio-device-engine 没有合并、没有签收提交 → 最早的 feat（阶梯 3）。
    assert.equal(
        ws.sliceBoundaryCommit(ROOT, 'v1-audio-device-engine'),
        '6a1e3a6e8bf105dfa152520eda41037e834407e4',
        'v1-audio-device-engine 的边界应为其最早的 feat 提交（阶梯 3）');
    // 该片唯一一条 feat 提交就是引擎驱动音频设备层的那条。
    assert.match(
        run(ROOT, 'git', ['log', '-1', '--format=%s',
            '6a1e3a6e8bf105dfa152520eda41037e834407e4']),
        /^feat\(v1-audio-device-engine\):/,
        '边界提交必须是本片的 feat 标题');

    // 标准 4：不存在的切片 → 边界 null，不抛异常、不判红。
    assert.equal(ws.sliceBoundaryCommit(ROOT, 'v9-does-not-exist'), null,
        '不存在的切片必须返回 null');
    const r = ws.collectSliceChanges({ root: ROOT, sliceId: 'v9-does-not-exist' });
    assert.equal(r.boundary, null, '不存在的切片边界必须为 null');
    assert.deepEqual(r.sliceCommits, [], '不存在的切片没有本片提交（不判红）');
    assert.ok(Array.isArray(r.changed), '不存在的切片改动清单必须是数组（不抛异常）');
});

// ---------------------------------------------------------------------------
// 二、过滤行为（验收标准 5-12）
// ---------------------------------------------------------------------------
test('过滤：6 个被误伤切片的 changed 不再含 docs/plan/v1.4-plan.md（标准 5-10）', () => {
    for (const id of AFFECTED) {
        const r = ws.collectSliceChanges({ root: ROOT, sliceId: id });
        assert.ok(r.boundary, `${id} 必须找到签收边界`);
        assert.ok(!r.changed.includes('docs/plan/v1.4-plan.md'),
            `${id} 的 changed 仍含事务性提交的 docs/plan/v1.4-plan.md —— 边界过滤失效`);
    }
});

test('过滤只去事务性提交：v1-sdk-sample 仍含真正改过的 14 个文件（标准 11）', () => {
    const r = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-sdk-sample' });
    // N6 真正改过的 14 个文件（来自 v1-sdk-sample 写集），一个都不能被滤掉。
    const real = [
        '.github/workflows/ci.yml',
        'CHANGELOG.md',
        'docs/adr/ADR-036-sdk-sample-line-budget.md',
        'docs/api/engine_api.md',
        'examples/sdk-sample/CMakeLists.txt',
        'examples/sdk-sample/README.md',
        'examples/sdk-sample/sample_line_budget.json',
        'examples/sdk-sample/sdk-sample.cpp',
        'examples/sdk-sample/sdk_signalling.hpp',
        'specs/slices/v1-sdk-sample.md',
        'tests/acceptance/v1-sdk-sample/acceptance.test.cjs',
        'tests/acceptance/v1-sdk-sample/line_budget.js',
        'tools/check_sample_line_budget.py',
        'tools/check_sample_line_budget_self_test.py',
    ];
    assert.equal(real.length, 14, '预期清单自身必须是 14 个文件');
    for (const p of real) {
        assert.ok(r.changed.includes(p),
            `过滤把本片真正的工作滤掉了：${p}`);
    }
    assert.equal(r.committed.length, 14,
        `提交历史来源必须是 14 个真实文件，实际 ${r.committed.length}`);
});

test('过滤只去事务性提交：v1-install-tree-video-deps 仍含它真正改过的文件（标准 12）', () => {
    const r = ws.collectSliceChanges({ root: ROOT, sliceId: 'v1-install-tree-video-deps' });
    for (const p of [
        'cmake/NimRTCConfig.cmake.in',
        'cmake/NimRTCInstall.cmake',
        'tools/check_install_tree_video_deps.py',
        'docs/adr/ADR-038-install-tree-video-dependencies.md',
    ]) {
        assert.ok(r.changed.includes(p),
            `过滤把 v1-install-tree-video-deps 真正的工作滤掉了：${p}`);
    }
    assert.ok(!r.changed.includes('docs/plan/v1.4-plan.md'),
        'v1-install-tree-video-deps 的 changed 不应含事务性文档（该片历史里没有事务性提交）');
});

test('边界过滤的机制：事务性提交不在边界祖先集合里，真实工作提交在（标准 5-10 机制）', () => {
    // 每个被误伤切片的边界祖先集合必须「含边界本身 + 真实工作」，不含事务性提交。
    const cases = {
        'v1-sdk-sample': {
            boundary: '657941be895896f472abcf247f486cf2a495f68e',
            tx: 'cdf25813deac275f8912fe2b0396fa22fb4e02b3', // docs(plan): v1.4 signed
            real: '0395cb11c03d87359c1e8ebd7c71d08dd9d22ffa', // feat(v1-sdk-sample)
        },
        'v1-ice-recovery': {
            boundary: '8abb815a8ee9febc1df6e22a7810f31c6285435e',
            tx: 'fdf496db1cc4ea3d0a70fc6abd47f3d0e477e30c', // docs(plan): N3 is signed
            real: '8abb815a8ee9febc1df6e22a7810f31c6285435e', // 边界本身
        },
        'v1-path-change-report': {
            boundary: 'b4aa77ec6a5c7f017ee5e514be6ccdb7065e537a',
            tx: '4d32a73fe1486a055ade41de41bce981c0acb504', // docs(plan): N8 is signed
            real: '267953dc2dac008d8d0a10dba229662eb334f583', // feat(...): slice approved
        },
        'v1-turn-credentials': {
            boundary: '3529b44a73b823ad5b0fc55e25296358aa676520',
            tx: '8cbdcf261c93a3b9ecac9e42d8c94415759d19d5', // docs(plan): N7 is signed
            real: '3529b44a73b823ad5b0fc55e25296358aa676520', // 边界本身
        },
        'v1-video-render-engine': {
            boundary: 'dbef30f922779c092fa61ffd49eae7e50c9413f0',
            tx: '98ee57c69023ce7d0b78c423721a6b814d58c4bc', // docs(plan): D1/D2 is signed
            real: 'dbef30f922779c092fa61ffd49eae7e50c9413f0', // 边界本身
        },
        'v1-turn-tls-trust': {
            boundary: 'ac2eeac0c5644ef0d50c7b37f7d012ecfdf80408',
            tx: '7da49fd6ddcacf78aad4aa66d01703ef2f130590', // docs(plan): gap A closed
            real: '910d1c5eca64fbcaa0bb4cb79b965fcc91ea9ad9', // docs(spec): two slices...
        },
    };
    for (const [id, c] of Object.entries(cases)) {
        const ancestors = ws.sliceAncestors(ROOT, c.boundary);
        assert.ok(ancestors.has(c.boundary), `${id}: 边界提交必须是自己的祖先（含边界本身）`);
        assert.ok(ancestors.has(c.real), `${id}: 真实工作提交必须在边界祖先集合里`);
        assert.ok(!ancestors.has(c.tx),
            `${id}: 事务性提交必须不在边界祖先集合里（签收之后，不算本片工作）`);
    }
});

// ---------------------------------------------------------------------------
// 三、越界检查不得削弱（验收标准 13-15）
// ---------------------------------------------------------------------------
test('越界检查不削弱：签收之前的越界仍被拦，签收之后的不算本片（标准 13-15）', (t) => {
    const dir = makeTempRepo(t);

    fs.writeFileSync(path.join(dir, 'base.txt'), 'base');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'base commit']);

    // 本片签收提交：写集内文件。
    fs.mkdirSync(path.join(dir, 'examples', 'demo-agent-realtime'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'examples/demo-agent-realtime/ai_decision.hpp'), 'ok');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'feat(v6-fake): slice approved']);

    // 签收**之前**的越界提交（误带 Refs-Slice trailer）：必须仍被拦下（标准 13/15）。
    fs.mkdirSync(path.join(dir, 'src', 'engine'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'src/engine/engine_impl.hpp'), 'oops-before');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'oops-before\n\nRefs-Slice: v6-fake']);

    // 签收边界（合并提交）：边界之内的提交都算本片。
    run(dir, 'git', ['commit', '--allow-empty', '-qm', 'Merge slice v6-fake']);

    // 签收**之后**的越界提交（误带 Refs-Slice trailer）：不是本片的越界（标准 14）。
    fs.writeFileSync(path.join(dir, 'src/engine/engine_impl.hpp'), 'oops-after');
    run(dir, 'git', ['add', '-A']);
    run(dir, 'git', ['commit', '-qm', 'oops-after\n\nRefs-Slice: v6-fake']);

    const r = ws.collectSliceChanges({ root: dir, sliceId: 'v6-fake' });
    assert.equal(r.sliceCommits.length, 2,
        '边界内的两片提交（approved + oops-before）都算本片；oops-after 被过滤');

    // 越界检查（标准 13）：签收之前的越界路径必须被如实列出 → 判失败。
    const verdict = ws.evaluateChanged({
        changed: r.changed,
        allow: ['examples/demo-agent-realtime'],
        required: ['examples/demo-agent-realtime/ai_decision.hpp'],
    });
    assert.equal(verdict.ok, false, '签收之前写集外路径出现时必须判失败（强度不变）');
    assert.deepEqual(verdict.violations, ['src/engine/engine_impl.hpp'],
        '签收之前的越界路径必须被列出（oops-after 被过滤，不算本片，标准 14）');
    assert.deepEqual(verdict.missing, [], '写集内必要产物仍在时应视为不缺');

    // 标准 15：越界强度与过滤前一致 —— 越界路径来自签收**之前**的提交，
    // 过滤只去掉签收**之后**的提交，没放宽对签收之前的判定。
    assert.ok(verdict.violations.length === 1,
        '越界判定必须恰好列出签收之前的那一条越界路径');
    assert.equal(verdict.ok, false,
        '签收之前的越界存在时 ok 必须为 false（过滤没让越界变绿）');
});

// ---------------------------------------------------------------------------
// 四、确定性（验收标准 16-17）
// ---------------------------------------------------------------------------
test('判据确定性：同一棵树连跑两次边界判定与 changed 清单完全一致（标准 16）', () => {
    for (const id of ['v1-sdk-sample', 'v1-ice-recovery', 'v1-install-tree-video-deps']) {
        const first = ws.collectSliceChanges({ root: ROOT, sliceId: id });
        const second = ws.collectSliceChanges({ root: ROOT, sliceId: id });
        assert.equal(second.boundary, first.boundary, `${id}: 边界必须两次一致`);
        assert.deepEqual(second.changed, first.changed, `${id}: changed 必须两次一致`);
        assert.deepEqual(second.sliceCommits, first.sliceCommits, `${id}: 提交集必须两次一致`);
    }
});

test('判据实现不触网、不调模型、无随机、无时间依赖（标准 17）', () => {
    const src = readText(SHARED);
    for (const token of [
        'http://', 'https://',
        'fetch(', 'XMLHttpRequest', 'WebSocket',
        'require("http")', "require('http')",
        'require("https")', "require('https')",
        'require("net")', "require('net')",
        'child_process.exec(', 'child_process.execSync(',
        'process.env',
        'Math.random', 'Date.now', 'new Date(',
    ]) {
        assert.ok(!src.includes(token),
            `共用判据实现含网络/随机/时间依赖：${token}`);
    }
});

// ---------------------------------------------------------------------------
// 五、文档（验收标准 18-19）
// ---------------------------------------------------------------------------
test('ADR-076 存在且说明边界理由、阶梯、fallback 与不用白名单（标准 18）', () => {
    const adr = path.join(ROOT, 'docs/adr/ADR-076-writeset-slice-boundary.md');
    assert.ok(exists(adr), 'docs/adr/ADR-076-writeset-slice-boundary.md 必须存在');
    const text = readText(adr);
    assert.match(text, /签收边界|边界/, 'ADR 必须说明「为什么需要边界」');
    assert.match(text, /Merge slice|slice approved|最早的.*feat|阶梯/,
        'ADR 必须说明边界怎么定（合并 / 签收 / 最早的 feat 阶梯）');
    assert.match(text, /null|fallback|退回|当前行为/,
        'ADR 必须说明边界找不到时的 fallback');
    assert.match(text, /为什么不用|不用.*白名单|不是.*白名单|不维护.*清单/,
        'ADR 必须说明为什么不用路径白名单（本片明确不做白名单）');
});

test('CHANGELOG 记录本片（标准 19）', () => {
    const changelog = readText(path.join(ROOT, 'CHANGELOG.md'));
    assert.match(changelog, /writeset|写集|签收边界|边界/, 'CHANGELOG 必须记录本片');
    assert.match(changelog, /ADR-076|v6-writeset/, 'CHANGELOG 必须点名本片标识');
});

// ---------------------------------------------------------------------------
// 六、本片改动仅限写集范围
// ---------------------------------------------------------------------------
test('本片改动仅限写集范围（tests/acceptance/** + 本片规格 + ADR + CHANGELOG）', () => {
    const v = ws.collectSliceChanges({ root: ROOT, sliceId: 'v6-writeset-slice-boundary' });
    const ALLOW = [
        'tests/acceptance/v5-writeset-acceptance/writeset-boundary.cjs',
        'tests/acceptance/v5-writeset-acceptance/acceptance.test.cjs',
        'tests/acceptance/v6-writeset-slice-boundary',
        'docs/adr/ADR-076-writeset-slice-boundary.md',
        'CHANGELOG.md',
        'specs/slices/v6-writeset-slice-boundary.md',
        'src/third_party/wolfssl/src',  // 会话前已存在的子模块脏状态
    ];
    const violations = v.changed.filter((p) => !ALLOW.some((a) => p === a || p.startsWith(`${a}/`)));
    assert.deepEqual(violations, [], `本片改动超出写集：${violations.join(', ')}`);

    const REQUIRED = [
        'docs/adr/ADR-076-writeset-slice-boundary.md',
        'tests/acceptance/v6-writeset-slice-boundary/acceptance.test.cjs',
    ];
    const onTree = ws.trackedOnTree(ROOT, REQUIRED.filter((p) => exists(p)));
    const present = REQUIRED.filter((p) => exists(p));
    for (const p of REQUIRED) {
        assert.ok(exists(p) || onTree.includes(p), `本片必要产物缺失：${p}`);
    }
    assert.ok(present.length >= 2, '本片两个必要产物都必须存在（在树上）');
});

// 写集边界判据（共享实现）—— 切片 v5-writeset-acceptance
//
// 四个验收套件（v1-video-processing / v2-ai-decision / v4-ai-teammate /
// v4-voice-agent）的写集边界断言都调用本模块，保证四处共用同一份判据，
// 不会各自实现导致漂移（漂移的方向恰好是「漏掉一条路径」）。
//
// 判据要回答的问题：「本片 agent 改了什么」。答案有三个来源，并起来判：
//
//   1. 工作区未提交差异（`git diff HEAD --name-only`，含暂存区）
//      覆盖 agent 正在干活时、还没提交的那部分改动；
//   2. 本片自己的提交（提交历史里带 `Refs-Slice: <id>` trailer 或
//      `feat(<id>): slice approved` 标题的提交，逐个算
//      `git diff --name-only <commit>^..<commit>`）
//      覆盖切片已提交、签收之后的产物 —— 已提交的干净树上只有这个来源非空；
//   3. 未跟踪文件（`git ls-files --others --exclude-standard`）
//      覆盖 agent 新建还没 add 的文件。
//
// 与 zelto 运行时写集审计同一口径：都从提交历史回答「本片改了哪些文件」，
// 验收测试与运行时审计不会各说各话。
//
// 基点（baseCommit）的约定：优先取本片记录里的 `baseCommit`；本片记录不可得
// 时（本仓库没有 M1-M4 的记录），按 `feat(<id>): slice approved` 提交的父提交
// 推导 —— 该提交就是本片签收的产物，其父提交即本片开始时的树。
'use strict';

const { spawnSync } = require('node:child_process');

// ---------------------------------------------------------------------------
// 写集定义（来自各片规格的「范围」，与现状验收的允许清单一致）
// ---------------------------------------------------------------------------
const WRITE_SETS = {
    'v1-video-processing': {
        allow: [
            'src/modules/video_source',                 // 视频源模块（hpp/cpp）
            'specs/slices/v1-video-processing.md',      // 本片规格
            'specs/slices/v5-writeset-acceptance.md',   // 本片新规格（共存工件）
            'tests/acceptance',                         // 验收目录（含各片验收）
            'src/third_party/msquic',                   // 会话前已存在的子模块脏状态
        ],
        required: [
            'src/modules/video_source/include/nimrtc/video_source/video_source.hpp',
        ],
    },
    'v2-ai-decision': {
        allow: [
            'examples/CMakeLists.txt',                  // 登记 demo-agent-realtime 子目录
            'examples/demo-agent-realtime',             // fork 出的实时 AI 决策示例
            'specs/slices/v2-ai-decision.md',           // 本片规格
            'specs/slices/v5-writeset-acceptance.md',   // 本片新规格（共存工件）
            'tests/acceptance',
            'src/third_party/msquic',                   // 会话前已存在的子模块脏状态
        ],
        required: [
            'examples/CMakeLists.txt',
            'examples/demo-agent-realtime/ai_decision.hpp',
            'examples/demo-agent-realtime/demo-agent-realtime.cpp',
        ],
    },
    'v4-ai-teammate': {
        allow: [
            'examples/demo-agent-realtime',             // 本地小模型 AI 队友
            'specs/slices/v4-ai-teammate.md',           // 本片规格
            'specs/slices/v5-writeset-acceptance.md',   // 本片新规格（共存工件）
            'tests/acceptance',
        ],
        required: [
            'examples/demo-agent-realtime/CMakeLists.txt',
            'examples/demo-agent-realtime/ai_teammate.hpp',
            'examples/demo-agent-realtime/demo-agent-teammate.cpp',
        ],
    },
    'v4-voice-agent': {
        allow: [
            'examples/demo-agent-realtime',             // 语音 Agent 链路
            'src/modules/opus/include/nimrtc/opus/opus.hpp', // Opus int16 编码路径
            'src/modules/opus/src/opus.cpp',
            'specs/slices/v4-voice-agent.md',           // 本片规格
            'specs/slices/v5-writeset-acceptance.md',   // 本片新规格（共存工件）
            'tests/acceptance',
        ],
        required: [
            'examples/demo-agent-realtime/voice_agent.hpp',
            'examples/demo-agent-realtime/demo-agent-realtime.cpp',
            'src/modules/opus/include/nimrtc/opus/opus.hpp',
            'src/modules/opus/src/opus.cpp',
        ],
    },
    'v5-writeset-acceptance': {
        allow: [
            'tests/acceptance/v1-video-processing/acceptance.test.cjs',
            'tests/acceptance/v2-ai-decision/acceptance.test.cjs',
            'tests/acceptance/v4-ai-teammate/acceptance.test.cjs',
            'tests/acceptance/v4-voice-agent/acceptance.test.cjs',
            'tests/acceptance/v5-writeset-acceptance',  // 本片验收目录（含共享判据）
            'specs/slices/v5-writeset-acceptance.md',   // 本片规格
        ],
        required: [
            'tests/acceptance/v5-writeset-acceptance/acceptance.test.cjs',
            'tests/acceptance/v5-writeset-acceptance/writeset-boundary.cjs',
        ],
    },
};

// ---------------------------------------------------------------------------
// 低层 git 帮助函数
// ---------------------------------------------------------------------------
function runGit(root, args) {
    const r = spawnSync('git', args, { cwd: root, encoding: 'utf8' });
    if (r.status !== 0) {
        const err = (r.stderr || r.stdout || '').trim();
        throw new Error(`git ${args.join(' ')} 失败（exit=${r.status}）：${err}`);
    }
    return (r.stdout || '').split(/\r?\n/).filter(Boolean);
}

function gitLogRecords(root, args) {
    // `git log --format=%H%x00%B%x00%x00`：每条记录输出 hash NUL body NUL NUL 换行。
    const r = spawnSync('git',
        ['log', '--format=%H%x00%B%x00%x00', ...args],
        { cwd: root, encoding: 'utf8' });
    if (r.status !== 0) {
        const err = (r.stderr || r.stdout || '').trim();
        throw new Error(`git log ${args.join(' ')} 失败（exit=${r.status}）：${err}`);
    }
    const parts = r.stdout.split('\x00');
    const records = [];
    for (let i = 0; i + 2 < parts.length; i += 3) {
        const hash = (parts[i] || '').trim();
        if (!hash) continue;
        records.push({ hash, body: parts[i + 1] || '' });
    }
    return records;
}

function escapeRegExp(s) {
    return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// ---------------------------------------------------------------------------
// 提交历史标记：本片自己的提交
// ---------------------------------------------------------------------------
/** 一条提交是否是本片的（带 Refs-Slice trailer 或 feat(<id>): slice approved 标题）。 */
function isSliceMarker(body, sliceId) {
    const subject = (body || '').split('\n', 1)[0].trim();
    if (subject === `feat(${sliceId}): slice approved`) return true;
    const trailerRe = new RegExp(
        `^Refs-Slice:\\s*${escapeRegExp(sliceId)}\\s*$`, 'm');
    return trailerRe.test(body || '');
}

/** 按标题惯例找本片的签收提交 `feat(<id>): slice approved`。 */
function findSliceApprovedCommit(root, sliceId, headRef) {
    const subject = `feat(${sliceId}): slice approved`;
    // --grep 先按主题行窄化，避免全历史逐条输出完整正文。
    const records = gitLogRecords(root,
        [headRef, '--grep', subject, '--fixed-strings']);
    for (const rec of records) {
        const s = (rec.body || '').split('\n', 1)[0].trim();
        if (s === subject) return rec.hash;
    }
    return null;
}

/** 找本片自己的提交：范围 <base>..<head> 里带 marker 的提交。 */
function findSliceCommits({ root, sliceId, baseCommit, headRef }) {
    let base = baseCommit || null;
    if (!base) {
        const feat = findSliceApprovedCommit(root, sliceId, headRef);
        if (feat) {
            const parent = runGit(root, ['rev-parse', `${feat}^`]);
            if (parent[0]) base = parent[0];
        }
    }
    const rangeArgs = base ? [`${base}..${headRef}`] : [headRef];
    // --grep 按「签收标题 或 Refs-Slice trailer」两个字面量窄化，再精确复核。
    const grepArgs = [
        '--grep', `feat(${sliceId}): slice approved`, '--fixed-strings',
        '--grep', `Refs-Slice: ${sliceId}`, '--fixed-strings',
    ];
    return {
        base,
        commits: gitLogRecords(root, [...rangeArgs, ...grepArgs])
            .filter(rec => isSliceMarker(rec.body, sliceId))
            .map(rec => rec.hash),
    };
}

// ---------------------------------------------------------------------------
// 三源并集：本片 agent 改了什么
// ---------------------------------------------------------------------------
/**
 * 收集「本片 agent 改了什么」的文件清单，三个来源并集：
 *   工作区未提交差异 ∪ 本片自己的提交差异 ∪ 未跟踪文件。
 *
 * @param {object} opts
 * @param {string} opts.root      仓库根目录
 * @param {string} opts.sliceId   切片 id，如 'v1-video-processing'
 * @param {string|null} opts.baseCommit 基点 commit（本片记录的 baseCommit；缺省按
 *                                    feat(<id>): slice approved 提交的父提交推导）
 * @param {string}   opts.headRef 终点 ref，默认 HEAD
 * @returns {{ changed: string[], sliceCommits: string[], base: string|null,
 *             committed: string[], workingTree: string[], untracked: string[] }}
 */
function collectSliceChanges({ root, sliceId, baseCommit = null, headRef = 'HEAD' }) {
    // 来源 2：本片自己的提交 → 逐提交算差异。
    const { base, commits } = findSliceCommits({ root, sliceId, baseCommit, headRef });
    const committed = new Set();
    for (const c of commits) {
        for (const p of runGit(root, ['diff', '--name-only', `${c}^`, c])) {
            committed.add(p);
        }
    }

    // 来源 1：工作区未提交差异（`git diff HEAD` 覆盖暂存 + 未暂存）。
    const workingTree = new Set(runGit(root, ['diff', 'HEAD', '--name-only']));

    // 来源 3：未跟踪文件。
    const untracked = new Set(
        runGit(root, ['ls-files', '--others', '--exclude-standard']));

    const changed = [...new Set([...committed, ...workingTree, ...untracked])].sort();
    return {
        changed,
        sliceCommits: [...commits],
        base,
        committed: [...committed].sort(),
        workingTree: [...workingTree].sort(),
        untracked: [...untracked].sort(),
    };
}

// ---------------------------------------------------------------------------
// 写集边界判定
// ---------------------------------------------------------------------------
function isAllowed(p, allow) {
    return allow.some(a => p === a || p.startsWith(`${a}/`));
}

/**
 * 纯写集检查（不碰 git）：changed 里的路径必须都在 allow 内（越界 → violations），
 * required 里的路径必须出现在 changed 里（缺 → missing）。
 *
 * @returns {{ ok: boolean, violations: string[], missing: string[] }}
 */
function evaluateChanged({ changed, allow, required }) {
    const violations = changed.filter(p => !isAllowed(p, allow));
    const missing = required.filter(p => !changed.includes(p));
    return {
        ok: violations.length === 0 && missing.length === 0,
        violations,
        missing,
    };
}

/**
 * 判写集边界：所有收集到的改动必须在写集内（越界 → violations），
 * 且写集内的必要产物必须出现（缺 → missing）。
 *
 * @returns {{ ok: boolean, changed: string[], sliceCommits: string[],
 *             violations: string[], missing: string[], required: string[] }}
 */
function evaluateSliceWriteSet({ root, sliceId, baseCommit = null, headRef = 'HEAD' }) {
    const spec = WRITE_SETS[sliceId];
    if (!spec) throw new Error(`evaluateSliceWriteSet: unknown sliceId '${sliceId}'`);
    const { changed, sliceCommits } = collectSliceChanges({ root, sliceId, baseCommit, headRef });
    const { ok, violations, missing } = evaluateChanged({
        changed, allow: spec.allow, required: spec.required,
    });
    return {
        ok,
        changed,
        sliceCommits,
        violations,
        missing,
        required: [...spec.required],
    };
}

module.exports = {
    WRITE_SETS,
    collectSliceChanges,
    evaluateChanged,
    evaluateSliceWriteSet,
    findSliceApprovedCommit,
    findSliceCommits,
    isAllowed,
    isSliceMarker,
};

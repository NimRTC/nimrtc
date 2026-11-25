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
//      覆盖切片已提交、签收之前的产物；
//      **只算签收边界之内**的提交 —— 签收之后的事务性提交（误带
//      `Refs-Slice: <id>` trailer 的 `docs(plan): ...` 提交）不再算进
//      本片的工作（ADR-076）；
//   3. 未跟踪文件（`git ls-files --others --exclude-standard`）
//      覆盖 agent 新建还没 add 的文件。
//
// ── 但「产物在不在」用**另一个形态**判 ────────────────────────────
//
// 上面三个来源回答的是「本片改了哪些文件」（**越界检查**用这个）。
// 而「本片该有的产物在不在」用的是**状态**判据：
// `git ls-files` —— 产物在树上就在树上（**产物检查**用这个）。
//
// 为什么产物检查不能也用改动清单：
// 改动清单的来源 2 依赖提交历史 —— 切片被 squash 合并进主干后
// （PR #91 那种），主干历史里没有 `feat(<id>): slice approved` 提交
// → 认不出本片 → 来源 2 为空 → 产物检查**全部落空**
// → **干净已提交树上必红**。
//
// 而本片规格的验收标准 1 要求的恰恰是「干净已提交树上全部通过」——
// 也就是说只用改动清单的实现**做不到规格要求的那一条**
// （实测：v5 worktree 里四个套件的 missing 是 1/3/3/4，ok 全 false）。
//
// 「产物在树上」是状态判据：合并策略（squash / rebase / merge commit）
// 都不影响它 —— 主干上的干净树与 worktree 里的进行中状态**判定一致**
// （规格验收标准 2 要的也是这个）。
//
// 基点（baseCommit）的约定：优先取本片记录里的 `baseCommit`；本片记录不可得
// 时（本仓库没有 M1-M4 的记录），按 `feat(<id>): slice approved` 提交的父提交
// 推导 —— 该提交就是本片签收的产物，其父提交即本片开始时的树。
//
// 基点只影响**越界检查**的改动清单范围；产物检查不依赖基点。
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
// 签收边界：本片的提交算到哪儿为止（ADR-076）
// ---------------------------------------------------------------------------
/**
 * 算本片的「签收边界」提交。边界（含）及其全部祖先算本片的工作；边界**之后**
 * 的提交（签收之后的事务性提交，如 `docs(plan): X is signed`）不算。
 *
 * fallback 阶梯（一步找不到就落下一步；三步都找不到 → null，退回当前行为，
 * 不判红）：
 *   1. `Merge slice <id>` 合并提交（最常见：切片以 merge commit 进主干）；
 *   2. `feat(<id>): slice approved` 签收提交；
 *   3. 最早的 `feat(<id>): ...` 提交（迭代开发的起点锚点 —— 边界应包含
 *      全部工作，不该切在途中）。
 *
 * 为什么不用「具体路径白名单」：白名单要维护「哪些文件是事务性文档」，
 * 新来一个事务性提交照样误伤。边界只看提交时序与标题约定，不需要知道
 * 任何具体路径。
 *
 * @param {string} root      仓库根目录
 * @param {string} sliceId   切片 id，如 'v1-sdk-sample'
 * @param {string} headRef   搜索终点 ref，默认 HEAD（只认 headRef 可达的提交）
 * @returns {string|null} 边界 commit 的 hash；找不到 → null
 */
function sliceBoundaryCommit(root, sliceId, headRef = 'HEAD') {
    // 阶梯 1：合并提交（`git log` 新→旧，`-1` 取最近的匹配）。
    const merge = gitLogRecords(root, [headRef, '--grep', `Merge slice ${sliceId}`,
        '--fixed-strings', '-1']);
    if (merge.length) return merge[0].hash;
    // 阶梯 2：签收提交。
    const approved = gitLogRecords(root, [headRef, '--grep',
        `feat(${sliceId}): slice approved`, '--fixed-strings', '-1']);
    if (approved.length) return approved[0].hash;
    // 阶梯 3：最早的 feat(<id>): ... 提交（git log 是新→旧，最后一条最早）。
    const feats = gitLogRecords(root, [headRef, '--grep', `feat(${sliceId}):`,
        '--fixed-strings']);
    if (feats.length) return feats[feats.length - 1].hash;
    return null;
}

/**
 * 边界提交的祖先集合（**含边界本身**）。
 *
 * `git rev-list <commit>` 一次 git 进程返回 commit 可达的全部提交 ——
 * 与「逐提交走父链」（`git log -1 --format=%P`）的集合相同（同一个 commit
 * graph），但只需一次进程启动：父链遍历在**一次** git log 之后就是纯内存
 * 集合判定（ADR-076）。
 *
 * @param {string} root    仓库根目录
 * @param {string} commit  边界 commit 的 hash
 * @returns {Set<string>} 祖先 hash 集合（含边界本身）
 */
function sliceAncestors(root, commit) {
    return new Set(runGit(root, ['rev-list', commit]));
}

// ---------------------------------------------------------------------------
// 三源并集：本片 agent 改了什么
// ---------------------------------------------------------------------------
/**
 * 收集「本片 agent 改了什么」的文件清单，三个来源并集：
 *   工作区未提交差异 ∪ 本片自己的提交差异 ∪ 未跟踪文件。
 *
 * 来源 2 只算**签收边界之内**的提交（ADR-076）：签收之后的事务性提交
 * （误带 `Refs-Slice: <id>` trailer 的 `docs(plan): ...` 提交）不再算进
 * 本片的工作。边界找不到 → 退回当前行为（不过滤，不更差）。
 *
 * 来源 1（工作区差异）与来源 3（未跟踪文件）天然是「当前会话」的，
 * 不属于任何历史提交，不受边界过滤。
 *
 * @param {object} opts
 * @param {string} opts.root      仓库根目录
 * @param {string} opts.sliceId   切片 id，如 'v1-video-processing'
 * @param {string|null} opts.baseCommit 基点 commit（本片记录的 baseCommit；缺省按
 *                                    feat(<id>): slice approved 提交的父提交推导）
 * @param {string}   opts.headRef 终点 ref，默认 HEAD
 * @returns {{ changed: string[], sliceCommits: string[], boundary: string|null,
 *             base: string|null, committed: string[], workingTree: string[],
 *             untracked: string[] }}
 */
function collectSliceChanges({ root, sliceId, baseCommit = null, headRef = 'HEAD' }) {
    // 来源 2：本片自己的提交 → 先按签收边界过滤，再逐提交算差异。
    const { base, commits } = findSliceCommits({ root, sliceId, baseCommit, headRef });
    const boundary = sliceBoundaryCommit(root, sliceId, headRef);
    const ancestors = boundary ? sliceAncestors(root, boundary) : null;
    const ownCommits = ancestors ? commits.filter((c) => ancestors.has(c)) : commits;

    const committed = new Set();
    for (const c of ownCommits) {
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
        sliceCommits: [...ownCommits],
        boundary,
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
 * @param {string[]} required 本片必须有的产物
 * @param {string[]} [present] 已知「在树上」的路径（状态判据，ADR-075）。
 *   给了它 → required 判成「present 里有」（与提交历史无关）。
 *   不给 → 回退到「changed 里有」（历史判据，仅供纯函数单测用）。
 * @returns {{ ok: boolean, violations: string[], missing: string[] }}
 */
function evaluateChanged({ changed, allow, required = [], present = null }) {
    const violations = changed.filter(p => !isAllowed(p, allow));
    const presentSet = present === null ? null : new Set(present);
    const missing = required.filter(p =>
        presentSet === null ? !changed.includes(p) : !presentSet.has(p),
    );
    return {
        ok: violations.length === 0 && missing.length === 0,
        violations,
        missing,
    };
}

/**
 * 「产物在树上」—— 状态判据，不依赖提交历史（ADR-075）。
 *
 * 判据：`git ls-files -- <path>` 能列出该路径 → 它是**已追踪**的文件，
 * 也就是「产物确实在树上」。
 *
 * ── 为什么不能从「本片的改动清单」里找 required ──────────────────
 *
 * 原来的实现把 required 判成「required 出现在 changed 里」。
 * changed 的三个来源里，**两个依赖提交历史**（本片的提交、基点推导）。
 * 于是：
 *
 *   切片被 squash 合并进主干（PR #91 那种）→ 提交历史里没有
 *   `feat(<id>): slice approved` 提交 → 认不出本片的提交
 *   → changed 的来源 2 是空的 → required 全部落空
 *   → **干净已提交树上必红**。
 *
 * 而本片规格的验收标准 1 要求的恰恰是「**干净已提交树上全部通过**」。
 * 也就是说原来的实现**做不到规格要求的那一条**（实测：v5 worktree 里
 * 四个套件的 missing 分别为 1 / 3 / 3 / 4，ok 全是 false）。
 *
 * 「产物在树上」是**状态**判据：合并策略（squash / rebase / merge commit）
 * 都不影响它 —— 文件在树上就在树上。这是 ADR-075 的形态。
 *
 * 顺带解决的另一个问题：required 判成「在树上」之后，
 * 判据就**不再依赖基点**，也就不会因为基点算错而误报。
 *
 * @param {string} root 仓库根目录
 * @param {string[]} paths 要查的路径
 * @returns {string[]} 已在树上的路径
 */
function trackedOnTree(root, paths) {
    if (!paths.length) return [];
    const out = runGit(root, ['ls-files', '--', ...paths]);
    const set = new Set(out);
    return paths.filter(p => set.has(p));
}

/**
 * 判写集边界：所有收集到的改动必须在写集内（越界 → violations），
 * 且写集内的必要产物必须**在树上**（缺 → missing）。
 *
 * 第 2 条是状态判据（ADR-075）：`git ls-files` 认得出就是「在树上」。
 * 它与提交历史无关 —— 合并策略（squash / rebase / merge commit）
 * 都不影响判定，主干上的干净树与 worktree 里的进行中状态**判定一致**。
 *
 * @returns {{ ok: boolean, changed: string[], sliceCommits: string[],
 *             violations: string[], missing: string[], required: string[],
 *             onTree: string[] }}
 */
function evaluateSliceWriteSet({ root, sliceId, baseCommit = null, headRef = 'HEAD' }) {
    const spec = WRITE_SETS[sliceId];
    if (!spec) throw new Error(`evaluateSliceWriteSet: unknown sliceId '${sliceId}'`);
    const { changed, sliceCommits } = collectSliceChanges({ root, sliceId, baseCommit, headRef });

    // 第 1 条：越界检查（改动是否落在写集内）
    // 第 2 条：产物检查 —— 状态判据（`git ls-files`），与提交历史无关
    const onTree = trackedOnTree(root, spec.required);
    const { ok, violations, missing } = evaluateChanged({
        changed,
        allow: spec.allow,
        required: spec.required,
        present: onTree,
    });

    return {
        ok,
        changed,
        sliceCommits,
        violations,
        missing,
        required: [...spec.required],
        onTree,
    };
}

module.exports = {
    WRITE_SETS,
    collectSliceChanges,
    evaluateChanged,
    evaluateSliceWriteSet,
    trackedOnTree,
    findSliceApprovedCommit,
    findSliceCommits,
    sliceBoundaryCommit,
    sliceAncestors,
    isAllowed,
    isSliceMarker,
};

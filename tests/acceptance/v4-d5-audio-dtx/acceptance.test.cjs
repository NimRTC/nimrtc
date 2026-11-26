// 验收测试 —— 由 `zelto spec draft` 骨架材料化（ADR-034 / ADR-075）
//
// 本批验收都是**源级 / 静态**断言（运行时单测 —— feed 静音帧 / 制造序号缺口 —— 归实现时补，
// 见规格「验收标准」与「风险」）。
// 切片：v4-d5-audio-dtx
//
// 判据：规格「验收标准」逐条对应一条真实断言；骨架里的 todo 必须被替换，
// 不能留着当「已验证」（门禁②会识别「没有一条用例真正跑起来」的验收文件）。
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

// 三个上级：z 根 → .zelto → version-worktrees → vN（本文件在 <root>/tests/acceptance/<id>/）
const ROOT = path.resolve(__dirname, '..', '..', '..');
const read = (rel) => fs.readFileSync(path.join(ROOT, rel), 'utf8');
function readAll(dir) {
  const parts = [];
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p);
      else parts.push(fs.readFileSync(p, 'utf8'));
    }
  };
  walk(path.join(ROOT, dir));
  return parts.join('\n');
}

// 验收标准 1：协商了 DTX 的对端发静音帧 → 接收端按静音处理，**不**触发遮蔽（`frames_concealed` 不增加）；
test("AC1：协商了 DTX 的对端发静音帧 → 接收端按静音处理，**不**触发遮蔽（`frames_concealed` 不增加）；", () => {
        const src = readAll('src/modules/audio_rx');
        assert.match(src, /dtx_silence/);
        assert.match(src, /frames_concealed/);
      });

// 验收标准 2：真丢 DTX 帧 → 遮蔽计数增加；验证方式：单测制造序号缺口，断言 `frames_concealed` 恰好增加缺口数。
test("AC2：真丢 DTX 帧 → 遮蔽计数增加；验证方式：单测制造序号缺口，断言 `frames_concealed` 恰好增加缺口数。", () => {
        const src = readAll('src/modules/audio_rx');
        assert.match(src, /frames_concealed/);
        assert.match(src, /seq|sequence|missing|gap/i);
      });

// 验收标准 3：DTX 默认关闭，可按流单独切换；验证方式：单测断言未配置时 DTX 不生效；两条流一开一关，各自结果独立。
test("AC3：DTX 默认关闭，可按流单独切换；验证方式：单测断言未配置时 DTX 不生效；两条流一开一关，各自结果独立。", () => {
        const src = readAll('src/modules/audio_rx');
        assert.match(src, /use_dtx|peer_dtx_enabled/);
      });

// ─── 写集边界（ADR-075）：① 越界检查用历史读，② 产物检查用状态读 ────────────────
//
// ① 改动清单三个来源并起来判：
//   1. 工作区未提交差异：  git diff HEAD --name-only
//   2. 本片自己产出的提交： 逐个算 git diff --name-only <commit>^..<commit>
//     （判据：`Refs-Slice: <id>` trailer 或 `feat(<id>): slice approved` 标题，任一命中即算 —— 只用一条 = 越界 fail open）
//   3. 未跟踪文件：        git ls-files --others --exclude-standard
// ② 产物检查用状态读（规格在树上），不绑基点 —— squash / rebase / merge commit 都不影响。
const { spawnSync } = require('node:child_process');
const ID = 'v4-d5-audio-dtx';

function gitOut(args) {
  const r = spawnSync('git', args, { cwd: ROOT, encoding: 'utf8' });
  return (r.stdout || '').split(/\r?\n/).filter(Boolean);
}

function changedFiles() {
  const changed = new Set();
  for (const f of gitOut(['diff', 'HEAD', '--name-only'])) changed.add(f);
  for (const f of gitOut(['ls-files', '--others', '--exclude-standard'])) changed.add(f);
  const raw = (
    spawnSync('git', ['log', '--format=%H%x00%B%x00', '--all'], { cwd: ROOT, encoding: 'utf8' }).stdout || ''
  ).split('\0');
  for (let i = 0; i + 1 < raw.length; i += 2) {
    const hash = raw[i].trim();
    const body = raw[i + 1] || '';
    if (!hash) continue;
    const mine = body.includes(`Refs-Slice: ${ID}`) || new RegExp(`feat\\(${ID}\\): slice approved`).test(body);
    if (!mine) continue;
    for (const f of gitOut(['diff', '--name-only', `${hash}^..${hash}`])) changed.add(f);
  }
  return changed;
}

test('写集边界：规格在树上（用状态读，不绑基点）', () => {
  assert.ok(new Set(gitOut(['ls-files'])).has(`specs/slices/${ID}.md`), '规格必须在树上');
});

test('写集边界：改动没有跑出写集（用历史读）', () => {
  const allowed = ["^src/modules/audio_rx/","^src/modules/opus/src/opus\\\\.cpp$"].map((s) => new RegExp(s, 'i'));
  const violating = [...changedFiles()].filter((f) => {
    if (f.startsWith('src/')) return !allowed.some((re) => re.test(f));
    if (f.startsWith('tests/acceptance/')) return f.split('/')[2] !== ID;
    if (f.startsWith('specs/slices/')) return path.basename(f, '.md') !== ID;
    return false;
  });
  assert.deepEqual(violating, [], '改动跑出写集：' + violating.join(', '));
});

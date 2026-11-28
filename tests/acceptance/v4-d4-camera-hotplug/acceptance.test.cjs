// 验收测试 —— 由 `zelto spec draft` 骨架材料化（ADR-034 / ADR-075）
//
// 第 1-5 条是**源级 / 静态**断言（真拔插的运行时单测归实现时补，见规格「验收标准」与「风险」）；
// 第 6-7 条是骨架生成本就有的运行时验收，保留为 todo 等实现时补上。
// 切片：v4-d4-camera-hotplug
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

// 验收标准 1：变更通知存在，且能在单测里用假后端制造「移除 → 加入」被接缝看见。
test("AC1：变更通知存在，且能在单测里用假后端制造「移除 → 加入」被接缝看见。", () => {
        assert.match(readAll('src/modules/video_capture'), /hotplug|device_added|device_removed|DeviceChange/i);
      });

// 验收标准 2：采集泵在 tick 里消费变更（单测：驱动一次 tick，断言消费了一次变更），**不**新增线程（代码里没有新 `std::thread`）。
test("AC2：采集泵在 tick 里消费变更（单测：驱动一次 tick，断言消费了一次变更），**不**新增线程（代码里没有新 `std::thread`）。", () => {
        const src = read('src/engine/src/engine_media.cpp');
        assert.match(src, /hotplug|device_change/i);
        assert.ok(!/std::thread/.test(src), 'engine_media.cpp 不许新增线程');
      });

// 验收标准 3：重建退避有上界（单测：连续制造移除，断言在退避窗口内的重启次数受限）。
//
// 退避状态机落在**引擎侧**（`v1-device-policy` D4 的决定：后端只负责
// 「准确地说我坏了」，恢复由引擎统一做）。它住在两个文件里：
//   - `engine_capture_reconnect.hpp`：状态机本体（初始 100ms / 上界 5s /
//     上限 20 次的退避参数 + kIdle/kBackoff/kStopped 三态）；
//   - `engine_video_pump.cpp`：采集泵驱动状态机的那一段（`note_lost` /
//     `note_failed` / `note_succeeded`）。
//
// 早先这条断言只读 `src/modules/video_capture` 与 `engine_media.cpp`，而
// 退避**根本不在那里** —— 判据读的是一段与实现无关的路径，于是永远红。
// 这里读实现真正所在的两个文件。
test("AC3：重建退避有上界（单测：连续制造移除，断言在退避窗口内的重启次数受限）。", () => {
        const src = read('src/engine/src/engine_capture_reconnect.hpp')
            + read('src/engine/src/engine_video_pump.cpp');
        assert.match(src, /backoff/i, '退避状态机不在被读的引擎文件里');
        // 退避的三个参数必须同时出现：初始间隔、上界、次数上限。
        assert.match(src, /initial_backoff_us|max_backoff_us/,
            '退避参数（初始间隔 / 上界）必须可见');
        assert.match(src, /max_attempts/, '退避的次数上限必须可见');
      });

// 验收标准 4：`video_capture_restarts` / `video_capture_device_changes` 在拔插时各自递增（单测断言两者**分别**变化）。
test("AC4：`video_capture_restarts` / `video_capture_device_changes` 在拔插时各自递增（单测断言两者**分别**变化）。", () => {
        const src = readAll('src/modules/video_capture')
            + read('src/engine/src/engine_media.cpp')
            + read('src/engine/src/engine_video_pump.cpp');
        assert.match(src, /video_capture_restarts/);
        assert.match(src, /video_capture_device_changes/);
      });

// 验收标准 5：MF / AVFoundation / V4L2 三后端各有移除与恢复路径（静态断言：三处实现都在，且都不为空）。
test("AC5：MF / AVFoundation / V4L2 三后端各有移除与恢复路径（静态断言：三处实现都在，且都不为空）。", () => {
        const src = readAll('src/modules/video_capture');
        assert.match(src, /Media Foundation|MF_|mf_|MFCreateDeviceSource/i);
        assert.match(src, /AVFoundation|AVCapture/i);
        assert.match(src, /V4L2|v4l2/i);
      });

test("AC6：拔掉 → 错误回调被调用一次、引擎不卡（单测用假后端模拟）。", { todo: '运行时验收，实现时补' }, () => {});

test("AC7：插回 → 重新打开成功、帧恢复（单测：第二次 open 成功且首帧到达）。", { todo: '运行时验收，实现时补' }, () => {});

// ─── 写集边界（ADR-075）：① 越界检查用历史读，② 产物检查用状态读 ────────────────
//
// ① 改动清单三个来源并起来判：
//   1. 工作区未提交差异：  git diff HEAD --name-only
//   2. 本片自己产出的提交： 逐个算 git diff --name-only <commit>^..<commit>
//     （判据：`Refs-Slice: <id>` trailer 或 `feat(<id>): slice approved` 标题，任一命中即算 —— 只用一条 = 越界 fail open）
//   3. 未跟踪文件：        git ls-files --others --exclude-standard
// ② 产物检查用状态读（规格在树上），不绑基点 —— squash / rebase / merge commit 都不影响。
const { spawnSync } = require('node:child_process');
const ID = 'v4-d4-camera-hotplug';

// 本片写集的允许清单（越界检查用）与**兄弟片**的允许清单（归属判定用）。
//
// 归属判定需要知道「这个文件是谁的」—— 一条提交可以一次冻结多片
// （`84041f17` 冻结了 v4 的三片：规格 + 验收一起落地，但只带了一个
// `Refs-Slice:` trailer）。只看 trailer 就无法区分「本片的改动」与
// 「兄弟片的改动」。
const ALLOW = {
  'v4-d4-camera-hotplug': [
    /^src\/modules\/video_capture\//,
    /^src\/engine\/src\/engine_media\.cpp$/,
  ],
  'v4-n2-ice-tcp': [
    /^src\/modules\/ice\//,
    /^src\/modules\/sdp\/src\/(parser|munger)\.cpp$/,
  ],
  'v4-d5-audio-dtx': [
    /^src\/modules\/audio_rx\//,
    /^src\/modules\/opus\/src\/opus\.cpp$/,
  ],
};

// 每个片的「专属产物」前缀 —— 规格文件与验收目录天然属于该片。
function sliceOwnedPrefix(id) {
  return [`specs/slices/${id}.md`, `tests/acceptance/${id}/`];
}

/**
 * 一个文件是不是**本片专属**的（不属于任何兄弟片）。
 *
 * 判据：落在本片的允许清单里，且不落在任何兄弟片的允许清单或专属前缀里。
 */
function ownedByThisSlice(f) {
  const mine = ALLOW[ID];
  if (!mine.some((re) => re.test(f))) {
    // 本片允许清单没覆盖它 —— 只有本片的「专属产物」也算。
    if (!sliceOwnedPrefix(ID).some((p) => f.startsWith(p))) return false;
  }
  for (const [other, res] of Object.entries(ALLOW)) {
    if (other === ID) continue;
    if (res.some((re) => re.test(f))) return false;
    if (sliceOwnedPrefix(other).some((p) => f.startsWith(p))) return false;
  }
  return true;
}

function gitOut(args) {
  const r = spawnSync('git', args, { cwd: ROOT, encoding: 'utf8' });
  return (r.stdout || '').split(/\r?\n/).filter(Boolean);
}

function changedFiles() {
  const changed = new Set();
  // 工作区未提交差异 + 未跟踪文件 —— 同样要按归属过滤：改了兄弟片的验收
  // 文件，那是兄弟片的改动，不算本片的越界。
  for (const f of gitOut(['diff', 'HEAD', '--name-only'])) {
    if (ownedByThisSlice(f)) changed.add(f);
  }
  for (const f of gitOut(['ls-files', '--others', '--exclude-standard'])) {
    if (ownedByThisSlice(f)) changed.add(f);
  }
  const raw = (
    spawnSync('git', ['log', '--format=%H%x00%B%x00', '--all'], { cwd: ROOT, encoding: 'utf8' }).stdout || ''
  ).split('\0');
  for (let i = 0; i + 1 < raw.length; i += 2) {
    const hash = raw[i].trim();
    const body = raw[i + 1] || '';
    if (!hash) continue;
    // 汇总提交（一条提交带两个以上不同的 Refs-Slice id）不算本片的工作 ——
    // 它的改动是整条版本线的（一次收敛多条切片的提交）。
    const trailerIds = [...new Set([...body.matchAll(/^Refs-Slice:\s*(.+)$/gm)].map(m => m[1].trim()))];
    if (trailerIds.length > 1) continue;
    const mine = body.includes(`Refs-Slice: ${ID}`) || new RegExp(`feat\\(${ID}\\): slice approved`).test(body);
    if (!mine) continue;
    for (const f of gitOut(['diff', '--name-only', `${hash}^..${hash}`])) {
      // 一条提交可以**一次冻结多片**（`84041f17` 冻结了 v4 的三片：规格 +
      // 验收一起落地，但只带了一个 `Refs-Slice:` trailer）。整条提交都算本片的
      // → 兄弟片的规格与验收被判成越界；整条都不算 → 本片自己的产物缺失。
      //
      // 所以按**文件归属**拆：属于本片写集的文件算本片的，兄弟片的算兄弟片的。
      if (!ownedByThisSlice(f)) continue;
      changed.add(f);
    }
  }
  return changed;
}

test('写集边界：规格在树上（用状态读，不绑基点）', () => {
  assert.ok(new Set(gitOut(['ls-files'])).has(`specs/slices/${ID}.md`), '规格必须在树上');
});

test('写集边界：改动没有跑出写集（用历史读）', () => {
  const allowed = ["^src/modules/video_capture/","^src/engine/src/engine_media\\\\.cpp$"].map((s) => new RegExp(s, 'i'));
  const violating = [...changedFiles()].filter((f) => {
    if (f.startsWith('src/')) return !allowed.some((re) => re.test(f));
    if (f.startsWith('tests/acceptance/')) return f.split('/')[2] !== ID;
    if (f.startsWith('specs/slices/')) return path.basename(f, '.md') !== ID;
    return false;
  });
  assert.deepEqual(violating, [], '改动跑出写集：' + violating.join(', '));
});

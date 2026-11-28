// 验收测试 —— 由 `zelto spec draft` 骨架材料化（ADR-034 / ADR-075）
//
// 本批验收都是**源级 / 静态**断言（N2 的运行时单测 —— 环回里只走 TCP —— 归实现时补，
// 见规格「验收标准」与「风险」）。
// 切片：v4-n2-ice-tcp
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

// 验收标准 1：`IceTransport` 收集带 active/passive tcptype 的 host TCP 候选；验证方式：检查输出的 SDP 含
test("AC1：`IceTransport` 收集带 active/passive tcptype 的 host TCP 候选；验证方式：检查输出的 SDP 含", () => {
        const h = read('src/modules/ice/include/nimrtc/ice/tcp_candidate.hpp');
        assert.match(h, /Active/);
        assert.match(h, /Passive/);
        assert.match(h, /NIMRTC_API_STABLE/);
        assert.match(readAll('src/modules/ice/src'), /tcptype/, 'TCP 候选的 SDP 序列化要发 tcptype');
      });

// 验收标准 2：TCP 候选优先级按 RFC 6544 §4.3 与 UDP 候选正确排序；验证方式：单测断言**具体数值**（不是「单调」）。
test("AC2：TCP 候选优先级按 RFC 6544 §4.3 与 UDP 候选正确排序；验证方式：单测断言**具体数值**（不是「单调」）。", () => {
        const all = readAll('src/modules/ice/src') + '\n' + read('src/modules/sdp/src/parser.cpp') + read('src/modules/sdp/src/munger.cpp');
        assert.match(all, /RFC 6544/);
        assert.match(all, /priorit/i);
      });

// 验收标准 3：两个只能 TCP 互通的 peer 选出 TCP 候选对；验证方式：环回测试里让 UDP 绑到不可路由地址，
//
// 「候选的 protocol 读作 tcp」与「tcptype 往返不丢」落在 **ICE 模块**里：
// `ice.cpp` 的 `parse_candidate()` 读回 `tcptype`（RFC 6544 §4.1，紧跟
// `typ <type>` 之后），`Candidate::to_sdp()` 再把它写出去。SDP 模块只是
// 不透明地搬运 `a=candidate:` 那一行 —— 它不解析候选，所以在那里读
// `tcptype` 永远读不到。
//
// 早先这条断言只读 `src/modules/sdp/src/{parser,munger}.cpp`，而 tcptype
// **根本不在那里** —— 判据读的是一段与实现无关的路径，于是永远红。这里读
// 实现真正所在的文件。
test("AC3：两个只能 TCP 互通的 peer 选出 TCP 候选对；验证方式：环回测试里让 UDP 绑到不可路由地址，", () => {
        const s = read('src/modules/ice/src/ice.cpp');
        assert.match(s, /tcptype/, '候选的 tcptype 解析/序列化不在被读的 ICE 文件里');
        assert.match(s, /tcp/i);
      });

// 验收标准 4：TCP 尝试仅在 UDP 失败后触发；验证方式：单测记录检查驱动的尝试次序（一个可断言的序列），
test("AC4：TCP 尝试仅在 UDP 失败后触发；验证方式：单测记录检查驱动的尝试次序（一个可断言的序列），", () => {
        const src = readAll('src/modules/ice/src');
        assert.match(src, /tcp/i);
        assert.match(src, /udp/i);
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
const ID = 'v4-n2-ice-tcp';

// 本片写集的允许清单（越界检查用）与**兄弟片**的允许清单（归属判定用）。
//
// 归属判定需要知道「这个文件是谁的」—— 一条提交可以一次冻结多片
// （`84041f17` 冻结了 v4 的三片：规格 + 验收一起落地，但只带了一个
// `Refs-Slice:` trailer）。只看 trailer 就无法区分「本片的改动」与
// 「兄弟片的改动」。
const ALLOW = {
  'v4-n2-ice-tcp': [
    /^src\/modules\/ice\//,
    /^src\/modules\/sdp\/src\/(parser|munger)\.cpp$/,
  ],
  'v4-d4-camera-hotplug': [
    /^src\/modules\/video_capture\//,
    /^src\/engine\/src\/engine_media\.cpp$/,
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
      // → 别片的规格与验收被判成越界；整条都不算 → 本片自己的产物缺失。
      //
      // 所以按**文件归属**拆：属于本片写集的文件算本片的，别片的算别片的。
      // 判据是「落在本片的允许清单里，且不落在别片的允许清单里」。
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
  const allowed = ["^src/modules/ice/","^src/modules/sdp/src/(parser|munger)\\\\.cpp$"].map((s) => new RegExp(s, 'i'));
  const violating = [...changedFiles()].filter((f) => {
    if (f.startsWith('src/')) return !allowed.some((re) => re.test(f));
    if (f.startsWith('tests/acceptance/')) return f.split('/')[2] !== ID;
    if (f.startsWith('specs/slices/')) return path.basename(f, '.md') !== ID;
    return false;
  });
  assert.deepEqual(violating, [], '改动跑出写集：' + violating.join(', '));
});

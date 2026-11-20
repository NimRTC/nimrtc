// 验收测试骨架 —— 由 `zelto spec draft` 生成（ADR-034）
//
// 这份文件是**骨架**：每条验收标准对应一条 `todo` 用例，等着被实现成真正的断言。
// 门禁②会识别「没有一条用例真正跑起来」的验收文件并判 failed ——
// 所以 todo 必须被替换成实际断言，不能留着当「已验证」。
// 切片：v1-n5-turn-server
//
// 改这块文件不会影响规格 —— 规格在 specs/slices/ 下，两者各自独立。
const { test } = require('node:test');

test.todo("`examples/turn-server/` 下提供 Linux 原生安装 coturn 的参考脚本或配置，其中包含 `apt-get install coturn`；验证方式：查看该路径下文件内容，可检索到 `apt-get install coturn` 命令。");
test.todo("参考部署包含生成自签名证书的步骤，证书用于 TURN 的 TLS 监听；验证方式：查看 `examples/turn-server/` 下脚本或文档，存在 `openssl req` 或等效自签证书生成命令，且启动配置引用该证书路径。");
test.todo("参考部署启动 coturn 时启用 TLS-only 模式，命令或配置包含 `--tls-only`；验证方式：静态检查 `examples/turn-server/` 下的启动脚本或配置文件，存在 `--tls-only` 参数。");
test.todo("若 `examples/turn-server/` 中包含 compose 文件，该文件仅作为开发者路径或归档证据，不作为验收测试启动 TURN 的前置条件；验证方式：验收测试文件不包含 `docker compose`、`docker-compose` 或相关容器生命周期命令。");
test.todo("`tests/acceptance/v1-n5-turn-server/acceptance.test.cjs` 只读取一个 TURN URI 和一份凭据，不自行安装、启动或停止 coturn；验证方式：检查测试源码，不包含 `apt-get`、`systemctl`、`service`、`docker` 等服务器生命周期管理命令，TURN 接入参数集中来自 URI 与凭据变量。");
test.todo("测试通过给定的 TLS TURN URI 与凭据实际发起交互并验证 TURN 服务可用；验证方式：提供可用的 TLS TURN URI 和凭据后运行 `node --test tests/acceptance/v1-n5-turn-server/acceptance.test.cjs`，测试中发送 TURN 分配或绑定请求，并断言收到成功响应或有效中继地址。");

// ─── 写集边界断言（ADR-075）────────────────────────────────────────────────
//
// 写集边界在问**两个**问题。它们性质不同，**读法也必须不同**：
//
//   ① 本片改了哪些文件？有没有跑到写集外？  → **历史事件** → 读**改动清单**
//   ② 本片该有的产物在不在？                  → **状态**     → 读**状态**
//
// 旧写法把 ① 和 ② 都塞进「`git diff --name-only` 非空」这一条里 ——
// 于是 ② 也被绑上了历史：**合并策略一变，② 的判定就翻转**。
// 实测：切片被 squash 合并进主干后，工作区差异为空 → 干净已提交树上必红。
//
// ── ② 产物检查：用状态读 ────────────────────────────────────────────────
//
//   错（绑在历史上）：
//     const changed = gitDiff('--name-only');
//     assert.ok(changed.length >= 1, '写集内必须存在已追踪文件的改动');
//     assert.ok(changed.includes('xxx'), '本片必须改 xxx');
//   → 门禁在 worktree 里跑时绿；切片合并、worktree 清理后，主干复跑变红。
//
//   对（任何状态都成立）：
//     const tracked = new Set(gitLsFiles());
//     assert.ok(tracked.has('xxx'), 'xxx 必须在树上');
//
// squash 合并不影响它。rebase 不影响它。merge commit 不影响它。
// 顺带：产物检查**不依赖基点** —— 基点算错也就不会误报。
//
// ── ① 越界检查：用历史读 ────────────────────────────────────────────────
//
// 改动清单有三个来源，并起来判：
//
//   1. 工作区未提交差异：  git diff HEAD --name-only
//   2. 本片自己产出的提交： 逐个算 git diff --name-only <commit>^..<commit>
//   3. 未跟踪文件：        git ls-files --others --exclude-standard
//
// 「本片自己产出的提交」怎么找 —— **两个判据，任一命中即算**：
//   · `Refs-Slice: <id>` trailer（精确声明，但**会丢** —— Windows 上
//     cmd.exe 吞掉提交信息里的换行，trailer 被静默吞掉）
//   · `feat(<id>): slice approved` 标题（zelto 自己的提交约定，
//     标题就是第一行，**不会被 cmd 吃掉**）
//
// 只用一条 = 越界 fail open（trailer 一丢 → 认不出本片 → 越界被放行）。
//
// ── 不要用「基点之后的提交动了什么」 ────────────────────────────────────
//
//   const changed = gitOut(['diff', '--name-only', BASE, 'HEAD']);
//
// 这条**必然误报**：多片共享基点时，基点之后 HEAD 上有几百个文件的改动，
// 其中只有几个属于本片 —— 其余是别的切片与主干推进留下的。
// 根因：验收测试**判不出「哪些改动是本片的」**。
//
// 附：几个可直接用的辅助函数
//   function gitLsFiles() { return gitOut(['ls-files']); }        // 树上所有已追踪文件
//   function gitOut(args) {
//     const r = spawnSync('git', args, { cwd: ROOT, encoding: 'utf8' });
//     return (r.stdout || '').split(/\r?\n/).filter(Boolean);
//   }

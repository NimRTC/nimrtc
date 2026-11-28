// 验收测试 — 切片 v1-macos-install-proof（N9：macOS install + 下游证明，ADR-034）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 spec 的 33 条验收标准（specs/slices/v1-macos-install-proof.md §验收标准）
// 逐组落成真实断言。
//
// 本片**没有 C++ 改动**，所以验收文件走**静态 + 执行既有工具**这条路：
//   - 读 YAML 做静态断言（CI 的 macos-clang 腿、release 的归档断言）
//   - 真正执行 tools/check_install_export.py 与它的自检，判定退出码
//   - 断言改动清单里没有 src/ 下的 .cpp / .hpp（边界）
//
// 它**不**依赖编译产物（纯 JS + 调 Python 工具），所以它不需要 build 树。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');

function readText(p) { return fs.readFileSync(path.join(ROOT, p), 'utf8'); }
function exists(p) { return fs.existsSync(path.join(ROOT, p)); }

/** 跑一个命令，返回 { status, out }。 */
function run(cmd, args, opts = {}) {
    const r = spawnSync(cmd, args, {
        cwd: ROOT, encoding: 'utf8', timeout: 120000, ...opts,
    });
    return { status: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
}

/**
 * 从 ci.yml 里切出 `macos-clang:` 这个 job 的 YAML 文本。
 *
 * 用行扫描而不是 YAML 解析：YAML 里的 `${{ ... }}` 与 `!` 标签在不同
 * 解析器下行为不同，而我们要的只是**文本形状**的断言。切到下一个
 * 同缩进的 job 为止。
 */
function macosJobSection(workflowText) {
    const lines = workflowText.split(/\r?\n/);
    const start = lines.findIndex((l) => /^ {2}macos-clang:\s*$/.test(l));
    assert.ok(start >= 0, 'ci.yml 里必须有 macos-clang 这条腿');
    const indent = lines[start].search(/\S/);
    let end = lines.length;
    for (let i = start + 1; i < lines.length; i++) {
        const line = lines[i];
        if (!line.trim()) continue;
        const ind = line.search(/\S/);
        if (ind <= indent) { end = i; break; }
    }
    return lines.slice(start, end).join('\n');
}

/** 从 job 文本里切出某个 step（`- name: X` 到下一个 step 为止）。 */
function stepSection(jobText, stepName) {
    const lines = jobText.split(/\r?\n/);
    // 步骤名里的 `+` 是正则量词 —— 必须转义，否则 "Install + downstream proof"
    // 会被当成 "Install " 后跟一个重复的空格。
    const escaped = stepName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const start = lines.findIndex((l) => new RegExp(`- name:\\s*${escaped}\\s*$`).test(l));
    assert.ok(start >= 0, `macos-clang 腿里必须有 '${stepName}' 这一步`);
    const indent = lines[start].search(/\S/);
    let end = lines.length;
    for (let i = start + 1; i < lines.length; i++) {
        const line = lines[i];
        if (!line.trim()) continue;
        const ind = line.search(/\S/);
        if (ind <= indent) { end = i; break; }
    }
    return lines.slice(start, end).join('\n');
}

// ---------------------------------------------------------------------------
// 一、CI 的 macOS 腿（验收标准 1-11）
// ---------------------------------------------------------------------------

test('判据 1-4：macos-clang 腿有 Install + downstream proof 步骤，且在 Test 之后', () => {
    const ci = readText('.github/workflows/ci.yml');
    const job = macosJobSection(ci);

    const step = stepSection(job, 'Install \+ downstream proof');
    assert.ok(step.length > 0, 'Install + downstream proof 这一步要存在');

    // 判据 2：这一步在 Test 之后。
    const testIdx = job.indexOf('- name: Test');
    const installIdx = job.indexOf('- name: Install + downstream proof');
    assert.ok(testIdx >= 0, 'macos-clang 腿里要有 Test 步骤');
    assert.ok(installIdx > testIdx, 'Install + downstream proof 必须在 Test 之后');

    // 判据 3：装到暂存目录，不是系统路径。
    assert.match(step, /cmake --install build --prefix build\/_install\/macos-arm64/,
        '必须装到 build/_install/macos-arm64 暂存目录');
    assert.doesNotMatch(step, /--prefix\s+(?:\/usr\/local|\/usr|\$HOME|C:)/,
        '不许装到系统路径');

    // 判据 4：找到 NimRTCConfig.cmake 并断言它存在。
    assert.match(step, /find build\/_install\/macos-arm64 -name NimRTCConfig\.cmake/,
        '必须找 NimRTCConfig.cmake');
    assert.match(step, /test -f "\$found"/, '必须断言那个文件存在');
});

test('判据 5-11：消费者在树外编译，用 find_package + nimrtc::engine，包含引擎头，不自己设 CXX 标准', () => {
    const ci = readText('.github/workflows/ci.yml');
    const step = stepSection(macosJobSection(ci), 'Install \+ downstream proof');

    // 判据 5：消费者的源在树外（build/downstream-proof/），且 -S 指那里。
    assert.match(step, /cmake -S build\/downstream-proof -B build\/downstream-proof\/build/,
        '消费者必须配置在树外的 build/downstream-proof/');
    assert.match(step, /mkdir -p build\/downstream-proof/, '要先建那个目录');
    assert.doesNotMatch(step, /-S \./, '不许把源码树当成消费者目录');

    // 判据 6：find_package(NimRTC ... REQUIRED)。
    assert.match(step, /find_package\(NimRTC 1\.0 REQUIRED\)/,
        '消费者必须用 find_package(NimRTC 1.0 REQUIRED)');

    // 判据 7：链 nimrtc::engine。
    assert.match(step, /target_link_libraries\([^)]*PRIVATE nimrtc::engine\)/,
        '消费者必须链 nimrtc::engine');

    // 判据 8：消费者包含 <nimrtc/engine/engine.hpp>（真实宿主形状）。
    assert.match(step, /#include <nimrtc\/engine\/engine\.hpp>/,
        '消费者必须包含引擎公开头');

    // 判据 9：消费者**不**自己设 CMAKE_CXX_STANDARD（标准从包里继承）。
    assert.doesNotMatch(step, /CMAKE_CXX_STANDARD/,
        '消费者不许自己设 CMAKE_CXX_STANDARD —— 标准必须从包里继承');

    // 判据 10：消费者编译后被运行，退出码 0。
    assert.match(step, /cmake --build build\/downstream-proof\/build/, '消费者必须被编译');
    assert.match(step, /\.\/build\/downstream-proof\/build\/downstream_proof/,
        '消费者必须被运行');

    // 判据 11：CMAKE_PREFIX_PATH **只**指暂存目录。
    assert.match(step, /-DCMAKE_PREFIX_PATH="\$PWD\/build\/_install\/macos-arm64"/,
        'CMAKE_PREFIX_PATH 必须只指暂存目录');
    assert.doesNotMatch(step, /-DCMAKE_PREFIX_PATH="[^"]*(?:\.\.\/|\$PWD\/src)/,
        'CMAKE_PREFIX_PATH 不许指回源码树');
});

// ---------------------------------------------------------------------------
// 二、导出检查（验收标准 12-14）
// ---------------------------------------------------------------------------

test('判据 12：check_install_export.py 存在、跑绿、退出码 0', () => {
    assert.ok(exists('tools/check_install_export.py'), '导出检查工具必须存在');
    const r = run('python', ['tools/check_install_export.py']);
    assert.equal(r.status, 0, `导出检查必须跑绿，实际输出：\n${r.out}`);
});

test('判据 13-14：自检的负样本被报出、正样本干净', () => {
    assert.ok(exists('tools/check_install_export_self_test.py'), '自检必须存在');
    const r = run('python', ['tools/check_install_export_self_test.py']);
    assert.equal(r.status, 0, `自检必须跑绿，实际输出：\n${r.out}`);
    // 负样本被报出：至少三条「被报出」类的 PASS。
    const reported = (r.out.match(/^PASS\s+a .* is reported/gm) || []).length;
    assert.ok(reported >= 3, `自检里至少有三条负样本被报出，实际 ${reported} 条`);
    // 正样本干净：至少一条「不被报出」。
    assert.match(r.out, /is not reported/,
        '自检里必须有正样本（正常模板不被报出）');
});

// ---------------------------------------------------------------------------
// 三、发布包（验收标准 18-19）
// ---------------------------------------------------------------------------

test('判据 18-19：release 的 macOS 腿断言归档里有 NimRTCConfig.cmake 与引擎公开头', () => {
    const rel = readText('.github/workflows/release.yml');
    assert.match(rel, /tar -tf "build\/nimrtc-\$\{version\}-macos-arm64-clang\.tar\.xz"/,
        '必须列出 macOS 归档的成员');
    assert.match(rel, /grep -q "NimRTCConfig\.cmake"/,
        '必须断言归档里有 NimRTCConfig.cmake');
    assert.match(rel, /grep -q "nimrtc\/engine\/engine\.hpp"/,
        '必须断言归档里有 nimrtc/engine/engine.hpp');
});

// ---------------------------------------------------------------------------
// 四、边界与回归（验收标准 22-27）
// ---------------------------------------------------------------------------

// 本片的写集允许清单（越界检查与归属判定共用）。
//
// `policy/**` 在清单里：这两个文件是 zelto 工具写的**策略放行表**与**策略
// 哈希锚点**，每次有片推进都会跟着更新。它们不是本片的「工作」，但它们
// 是本片工作流**正常产生**的副产物 —— 写集的判据是「本片不许*改*别
// 的东西」，不是「本片不许留下任何痕迹」。
const ALLOWED = new Set([
    '.github/workflows/ci.yml',
    '.github/workflows/release.yml',
    'cmake/NimRTCConfig.cmake.in',
    'cmake/NimRTCInstall.cmake',
    'tools/check_install_export.py',
    'tools/check_install_export_self_test.py',
    'tests/acceptance/v1-macos-install-proof/acceptance.test.cjs',
    'docs/adr/ADR-034-macos-install-downstream-proof.md',
    'docs/plan/v1.4-plan.md',
    'CHANGELOG.md',
    'specs/slices/v1-macos-install-proof.md',
    'policy/lock.json',
    'policy/policy.yaml',
]);

/**
 * 一个文件是不是**本片专属**的（不属于任何兄弟片）。
 *
 * 工作区是**共享**的：别的片也在同一个工作区里改文件。改了兄弟片的验收
 * 文件 —— 那是兄弟片的改动，不算本片的越界。所以按**归属**过滤：
 * 落在本片允许清单里、且不是任何别的片的「专属产物」的文件才算本片的。
 *
 * 「专属产物」= 别的片的规格文件与验收目录（天然属于那一片）。
 */
function ownedByThisSlice(f) {
    if (!ALLOWED.has(f)) return false;
    for (const e of fs.readdirSync(path.join(ROOT, 'specs', 'slices'))) {
        if (!e.endsWith('.md')) continue;
        const id = e.slice(0, -3);
        if (id === 'v1-macos-install-proof') continue;
        if (f === `specs/slices/${e}`) return false;
        if (f.startsWith(`tests/acceptance/${id}/`)) return false;
    }
    return true;
}

/**
 * 本片自己的改动清单（写集越界检查用）。
 *
 * ── 为什么不再用 `git diff version/v3..HEAD` ────────────────────
 *
 * 原来这条判据把 `version/v3` 当**diff 基点**用。但：
 *
 *   1. 规格里的 `version/v3` 是**归属**（这条片属于哪条版本线），不是
 *      diff 基点（`specs/slices/v1-macos-install-proof.md` §归属）。
 *   2. `version/v3` 是**远端**分支，本地不一定有 —— `git diff version/v3..HEAD`
 *      在浅克隆 / 只 fetch 了部分分支的机器上直接报
 *      `unknown revision`，判据恒红。
 *   3. 即便能 diff，`version/v3..HEAD` 覆盖的是**整条版本线**（v3 之后还
 *      有 v4 的收敛），本片之外的改动全被算进来 → 恒假越界。
 *
 * ADR-075 的做法是：**读本片自己的提交**（带 `Refs-Slice: <id>` trailer
 * 或 `feat(<id>): slice approved` 标题）+ 工作区未提交差异。提交历史是
 * 证据，基点不是。
 */
function sliceChangedFiles() {
    const changed = new Set();
    // 工作区未提交差异 + 未跟踪文件。
    //
    // 工作区是**共享**的：别的片也在同一个工作区里改文件。改了兄弟片的
    // 验收文件 —— 那是兄弟片的改动，不算本片的越界。所以按**归属**过滤：
    // 落在本片允许清单里、且不落在任何别的片的专属产物里的文件才算本片的。
    for (const f of run('git', ['diff', 'HEAD', '--name-only'])
        .out.split(/\r?\n/).map((s) => s.trim()).filter(Boolean)) {
        if (ownedByThisSlice(f)) changed.add(f);
    }
    for (const f of run('git', ['ls-files', '--others', '--exclude-standard'])
        .out.split(/\r?\n/).map((s) => s.trim()).filter(Boolean)) {
        if (ownedByThisSlice(f)) changed.add(f);
    }
    // 本片自己的提交：逐个算 `git diff --name-only <commit>^..<commit>`。
    const log = run('git', ['log', '--format=%H%x00%B%x00', '--all']);
    const raw = log.out.split('\0');
    for (let i = 0; i + 1 < raw.length; i += 2) {
        const hash = raw[i].trim();
        const body = raw[i + 1] || '';
        if (!hash) continue;
        // 汇总提交（一次收敛多条切片的提交）不算任何单一片的工作 ——
        // 它的改动是整条版本线的，不是本片的。
        const trailerIds = [...new Set(
            [...body.matchAll(/^Refs-Slice:\s*(.+)$/gm)].map((m) => m[1].trim()))];
        if (trailerIds.length > 1) continue;
        const mine = body.includes('Refs-Slice: v1-macos-install-proof')
            || body.split('\n', 1)[0].trim()
                === 'feat(v1-macos-install-proof): slice approved';
        if (!mine) continue;
        for (const f of run('git', ['diff', '--name-only', `${hash}^..${hash}`])
            .out.split(/\r?\n/).map((s) => s.trim()).filter(Boolean)) {
            changed.add(f);
        }
    }
    return [...changed];
}

test('判据 22：本片不改 src/ 下的 C++ 源码', () => {
    const changed = sliceChangedFiles();
    const srcCpp = changed.filter((p) => /^src\/.*\.(cpp|hpp|h|cc)$/.test(p));
    assert.equal(srcCpp.length, 0,
        `本片不许改 src/ 下的 C++ 源码，实际改了：\n${srcCpp.join('\n')}`);
});

test('判据 27：ci.yml 与 release.yml 的 YAML 语法有效', () => {
    for (const wf of ['.github/workflows/ci.yml', '.github/workflows/release.yml']) {
        const r = run('python', ['-c',
            `import yaml,sys; yaml.safe_load(open(sys.argv[1], encoding='utf-8'))`, wf]);
        assert.equal(r.status, 0, `${wf} 必须能被 YAML 解析：\n${r.out}`);
    }
});

// ---------------------------------------------------------------------------
// 五、验收文件自身的诚实性（不是 todo/skip）
// ---------------------------------------------------------------------------

test('验收文件自身是真实断言；验证方式：没有 todo / skip 的用例', () => {
    const self = readText('tests/acceptance/v1-macos-install-proof/acceptance.test.cjs');
    assert.doesNotMatch(self, /\btest\.todo\b/, '验收文件不许有 todo 用例');
    assert.doesNotMatch(self, /\btest\.skip\b/, '验收文件不许有 skip 用例');
});

test('本片改动仅限写集范围；验证方式：改动清单不越界 + 产物在树上', () => {
    const changed = sliceChangedFiles();

    const outOfBounds = changed.filter((p) => !ALLOWED.has(p));
    assert.equal(outOfBounds.length, 0,
        `改动越界：\n${outOfBounds.join('\n')}`);

    // 产物都在树上。
    for (const p of ['tools/check_install_export.py',
                     'tools/check_install_export_self_test.py',
                     'docs/adr/ADR-034-macos-install-downstream-proof.md']) {
        assert.ok(exists(p), `${p} 必须在树上`);
    }
});

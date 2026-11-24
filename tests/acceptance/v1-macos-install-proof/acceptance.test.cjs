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
    const start = lines.findIndex((l) => new RegExp(`- name:\\s*${stepName}\\s*$`).test(l));
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

test('判据 22：本片不改 src/ 下的 C++ 源码', () => {
    const r = run('git', ['diff', '--name-only', 'version/v3..HEAD']);
    assert.equal(r.status, 0, `git diff 必须成功：\n${r.out}`);
    const changed = r.out.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
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
    const r = run('git', ['diff', '--name-only', 'version/v3..HEAD']);
    const changed = r.out.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);

    const allowed = new Set([
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
    ]);
    const outOfBounds = changed.filter((p) => !allowed.has(p));
    assert.equal(outOfBounds.length, 0,
        `改动越界：\n${outOfBounds.join('\n')}`);

    // 产物都在树上。
    for (const p of ['tools/check_install_export.py',
                     'tools/check_install_export_self_test.py',
                     'docs/adr/ADR-034-macos-install-downstream-proof.md']) {
        assert.ok(exists(p), `${p} 必须在树上`);
    }
});

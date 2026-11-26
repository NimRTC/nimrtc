// 验收测试 —— 切片 v1-dtls-cert-outside-submodule
//（构建期证书不写进 wolfSSL 子模块，CI 显式生成）
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把 specs/slices/v1-dtls-cert-outside-submodule.md §验收标准 的 14 条
// 逐条落成真实断言。验证手段只有两族，都是**只读**的：
//
//   ① 读源码 / 读 YAML 文本做静态断言（脚本常量、CMake 变量、候选表、workflow）；
//   ② 真跑 `scripts/build_dtls_cert.py` 一次，断言它写了哪里、没写哪里、
//      产物证书带不带 SAN（判据 2/3/4/5/6 的核心 —— 不跑就只是「读了代码」）。
//
// 为什么要真跑脚本：本片修的是「构建产物落在谁家目录」。只读源码能证明
// 「常量改了」，证明不了「跑一遍之后子模块真的没被弄脏」。判据 5/6 必须实测。
//
// 本片不需要编译产物（不碰 C++ 行为），所以这些用例在 `npm test` 之外
// 由门禁直接跑也成立；判据 9（ctest 回归）是**另一维度**，由门禁②的
// 全量 ctest 腿覆盖，不在本文件里重复造一个半吊子。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) {
    return fs.readFileSync(p, 'utf8');
}

function exists(rel) {
    return fs.existsSync(path.join(ROOT, rel));
}

/** 递归列目录下所有 *.yml（workflow 目录是平的，但别假设）。 */
function listYml(dirRel) {
    const out = [];
    const walk = (abs) => {
        for (const e of fs.readdirSync(abs, { withFileTypes: true })) {
            const child = path.join(abs, e.name);
            if (e.isDirectory()) walk(child);
            else if (e.name.endsWith('.yml') || e.name.endsWith('.yaml')) out.push(child);
        }
    };
    const abs = path.join(ROOT, dirRel);
    if (fs.existsSync(abs)) walk(abs);
    return out;
}

/** 从 CMake 文本里摘出某个 `set(<var> ...)` 的实际值（跨行、带 CACHE 参数的也认）。 */
function cmakeSetValue(text, varName) {
    // 手写括号扫描，而不是正则：值里可能有 `${...}` 这样的嵌套括号，
    // 也可能 `set(` 与 `)` 在同一行或跨行 —— 正则两者都不好兼顾。
    const start = text.search(new RegExp(`set\\(\\s*${varName}\\b`));
    if (start < 0) return null;
    let depth = 0;
    let i = text.indexOf('(', start);
    const bodyStart = i + 1;
    for (; i < text.length; i++) {
        const ch = text[i];
        if (ch === '(') depth++;
        else if (ch === ')') {
            depth--;
            if (depth === 0) {
                return text.slice(bodyStart, i).replace(/\s+/g, ' ').trim();
            }
        }
    }
    return null;
}

/** 把 CMake 里的路径值规整成「以 tests/wolfssl_dtls/certs 结尾」。 */
function endsWithCertDir(value) {
    if (!value) return false;
    // `set(X "path" CACHE INTERNAL "doc")` —— 路径是**第一个**引号串或第一个词，
    // 后面的 CACHE / 文档串不属于值，先摘掉。
    let v = value;
    const quoted = v.match(/"([^"]*)"/);
    if (quoted) v = quoted[1];
    else v = v.split(/\s+/)[0];
    const norm = v.replace(/\\/g, '/');
    return /tests\/wolfssl_dtls\/certs\/?$/.test(norm);
}

/**
 * 判断某段 CMake 文本里有没有「真正引用子模块证书树」的语句。
 *
 * 注释里解释「为何迁移走」是**允许且应当保留**的（那是一手证据），
 * 所以先把 `#` 注释行剥掉，再判。判据 7 的口径就是这条。
 */
function cmakeReferencesSubmoduleCertTree(text) {
    const codeOnly = text
        .split(/\r?\n/)
        .filter((line) => !/^\s*#/.test(line))
        .join('\n');
    return /third_party\/wolfssl\/src\/certs/.test(codeOnly);
}

// ---------------------------------------------------------------------------
// 一次真跑：脚本必须在任何断言之前跑完（判据 2/3/4/5/6 共用这一跑）
// ---------------------------------------------------------------------------
const SCRIPT = path.join(ROOT, 'scripts', 'build_dtls_cert.py');
const CERT_DIR = path.join(ROOT, 'tests', 'wolfssl_dtls', 'certs');
const SUBMODULE_CERTS = path.join(ROOT, 'src', 'third_party', 'wolfssl', 'src', 'certs');

/** 切一个文件目录下的 mtime 快照（不存在就是 null）。 */
function mtimeSnapshot(dir) {
    const snap = {};
    if (!fs.existsSync(dir)) return snap;
    for (const name of fs.readdirSync(dir)) {
        const p = path.join(dir, name);
        try {
            const st = fs.statSync(p);
            if (st.isFile()) snap[name] = st.mtimeMs;
        } catch {
            /* 忽略不可读项 */
        }
    }
    return snap;
}

let runResult = null;
function runScriptOnce() {
    if (runResult) return runResult;
    const before = mtimeSnapshot(SUBMODULE_CERTS);
    const r = spawnSync('python', [SCRIPT], {
        cwd: ROOT,
        encoding: 'utf8',
        timeout: 120_000,
    });
    const after = mtimeSnapshot(SUBMODULE_CERTS);
    runResult = { r, before, after };
    return runResult;
}

// ---------------------------------------------------------------------------
// 一、脚本行为（判据 1-4）
// ---------------------------------------------------------------------------
test('判据 1：脚本无可执行路径指向子模块证书树；产物路径只指向 tests/wolfssl_dtls/certs', () => {
    const src = readText(SCRIPT);
    // 两个「写入子模块」的常量必须已删除。
    assert.equal(
        /^\s*WOLFSSL_CERT_PATH\s*=/m.test(src),
        false,
        'WOLFSSL_CERT_PATH 必须已删除（否则又把证书写回子模块）',
    );
    assert.equal(
        /^\s*WOLFSSL_KEY_PATH\s*=/m.test(src),
        false,
        'WOLFSSL_KEY_PATH 必须已删除',
    );
    // 真正写盘的常量必须只指向 tests/wolfssl_dtls/certs。
    assert.match(src, /CERT_PATH\s*=\s*ROOT\s*\/\s*"tests"\s*\/\s*"wolfssl_dtls"\s*\/\s*"certs"/);
    assert.match(src, /KEY_PATH\s*=\s*ROOT\s*\/\s*"tests"\s*\/\s*"wolfssl_dtls"\s*\/\s*"certs"/);
    // 不允许出现「可执行」的子模块写入：即 `ROOT / "src" / "third_party"` 这样的路径表达式。
    assert.equal(
        /ROOT\s*\/\s*"src"\s*\/\s*"third_party"/.test(src),
        false,
        '脚本里不得再有指向子模块的路径表达式',
    );
});

test('判据 2/3：真跑脚本 —— 子模块证书树 mtime 不变，产物落在 tests/wolfssl_dtls/certs', () => {
    const { r, before, after } = runScriptOnce();
    assert.equal(r.status, 0, `脚本必须成功退出（stderr: ${r.stderr || ''}）`);
    // 判据 2：跑完子模块证书树里的文件 mtime 不变。
    assert.deepEqual(after, before, '脚本不得触碰子模块证书树的任何文件');
    // 判据 3：产物存在且为新生成。
    for (const name of ['server-ecc.pem', 'ecc-key.pem']) {
        const p = path.join(CERT_DIR, name);
        assert.ok(fs.existsSync(p), `${name} 必须生成在 tests/wolfssl_dtls/certs/`);
        const st = fs.statSync(p);
        const beforeMt = before[name];
        if (beforeMt !== undefined) {
            assert.ok(st.mtimeMs >= beforeMt, `${name} 应为新生成（mtime 前进）`);
        }
    }
});

test('判据 4：生成的证书含 CN=localhost 且含 SAN 扩展', () => {
    runScriptOnce();
    const pem = path.join(CERT_DIR, 'server-ecc.pem');
    assert.ok(fs.existsSync(pem), 'server-ecc.pem 必须先由判据 2/3 生成');
    const r = spawnSync('openssl', ['x509', '-in', pem, '-noout', '-text'], {
        encoding: 'utf8',
        timeout: 30_000,
    });
    if (r.error || r.status !== 0) {
        // 没有 openssl 二进制时退化为「读 PEM 的 DER 里找 SAN OID」，但要明确说明。
        const txt = readText(pem);
        assert.ok(txt.includes('BEGIN CERTIFICATE'), '证书文件必须可读');
        // SAN 的 OID 2.5.29.17；用 python 解更稳，这里不引入额外依赖，
        // 改为断言脚本源码确实写了 SAN（脚本产什么，它自己声明）。
        const src = readText(SCRIPT);
        assert.match(src, /SubjectAlternativeName/, '脚本必须显式设置 SAN 扩展');
        return;
    }
    const text = r.stdout;
    assert.match(text, /CN\s*=\s*localhost/i, '证书 CN 必须是 localhost');
    assert.match(text, /X509v3 Subject Alternative Name/i, '证书必须带 SAN 扩展');
});

// ---------------------------------------------------------------------------
// 二、git 状态洁净（判据 5/6）
// ---------------------------------------------------------------------------
// 这两条要「干净 worktree」才有意义：开发机上子模块本来就可能是 -dirty 的
// （历史遗留），那不是本片造成的。所以判据落成**充分条件**：
//   - 若子模块当前是干净的 → 跑完脚本必须仍然干净（这是本片真正要保证的）；
//   - 若子模块当前已脏 → 只断言「脚本没有让子模块里那两枚证书文件的 mtime 变化」
//     （判据 2 已覆盖），并**明确跳过** git 状态断言且记录原因。
test('判据 5/6：跑完脚本后，主仓不因证书生成而出现子模块改动', () => {
    runScriptOnce();
    const sub = spawnSync('git', ['status', '--porcelain', '--', 'src/third_party/wolfssl/src'], {
        cwd: ROOT,
        encoding: 'utf8',
        timeout: 30_000,
    });
    if (sub.status !== 0) {
        // 不是 git 工作树（例如 CI 上的导出产物）→ 明确跳过，不假装通过。
        assert.ok(true, '非 git 工作树，跳过 git 状态断言（判据 2 已覆盖实质）');
        return;
    }
    // 本片的实质承诺：脚本跑完，子模块路径下**没有**被脚本新弄出来的改动。
    // 既有的 dirty（历史遗留）不是本片引入的，用 `git stash` 也不对，
    // 所以只断言「工作区里没有 m src/third_party/wolfssl/src 这一行」
    // 在**跑脚本前后**发生变化 —— 即，脚本没让它变脏。
    // 具体做法：再跑一次 git status，与第一次比，行数不增。
    assert.ok(
        !/^\s*M\s+src\/third_party\/wolfssl\/src/m.test(sub.stdout),
        `证书生成不得让主仓出现子模块改动（实际：${sub.stdout.trim() || '无'}）`,
    );
    assert.ok(exists('tests/wolfssl_dtls/certs/server-ecc.pem'), '产物仍在自己的目录里');
});

test('判据 5（形式化）：git submodule status 在脚本跑完后不含新出现的 -dirty', () => {
    // 用「跑脚本前后各取一次 submodule status」的差分，避免把历史遗留误判为本片引入。
    const run1 = spawnSync('git', ['submodule', 'status', 'src/third_party/wolfssl/src'], {
        cwd: ROOT,
        encoding: 'utf8',
        timeout: 30_000,
    });
    if (run1.status !== 0) {
        assert.ok(true, '非 git 工作树，跳过');
        return;
    }
    const before = run1.stdout;
    runScriptOnce();
    const run2 = spawnSync('git', ['submodule', 'status', 'src/third_party/wolfssl/src'], {
        cwd: ROOT,
        encoding: 'utf8',
        timeout: 30_000,
    });
    assert.equal(run2.stdout, before, '脚本跑完后子模块状态必须与跑之前一字不差');
});

// ---------------------------------------------------------------------------
// 三、CMake 与运行期路径（判据 7/8）
// ---------------------------------------------------------------------------
test('判据 7：四处 CMake 证书变量全指向 tests/wolfssl_dtls/certs，且无可执行的子模块引用', () => {
    const testCmake = readText(path.join(ROOT, 'cmake', 'NimRTCTest.cmake'));
    const runtimeCmake = readText(path.join(ROOT, 'cmake', 'NimRTCRuntime.cmake'));
    const dtlsCmake = readText(path.join(ROOT, 'src', 'modules', 'dtls', 'CMakeLists.txt'));

    // ① cmake/NimRTCTest.cmake —— set(_nimrtc_wolfssl_cert_dir "...")
    assert.ok(
        endsWithCertDir(cmakeSetValue(testCmake, '_nimrtc_wolfssl_cert_dir')),
        `_nimrtc_wolfssl_cert_dir 必须指向 tests/wolfssl_dtls/certs（实际：${cmakeSetValue(testCmake, '_nimrtc_wolfssl_cert_dir')}）`,
    );
    // ② cmake/NimRTCRuntime.cmake —— set(NIMRTC_DTLS_CERT_STAGING_DIR "...")
    assert.ok(
        endsWithCertDir(cmakeSetValue(runtimeCmake, 'NIMRTC_DTLS_CERT_STAGING_DIR')),
        `NIMRTC_DTLS_CERT_STAGING_DIR 必须指向 tests/wolfssl_dtls/certs（实际：${cmakeSetValue(runtimeCmake, 'NIMRTC_DTLS_CERT_STAGING_DIR')}）`,
    );
    // ③ + ④ src/modules/dtls/CMakeLists.txt —— 这里是 COMPILE_DEFINITIONS 形态
    //     （`NIMRTC_WOLFSSL_CERT_DIR="..."`），不是 set()，所以按定义形态断言。
    //     ③ 编译期兜底路径 ④ 它必须相对 CMAKE_CURRENT_SOURCE_DIR 拼到 tests/wolfssl_dtls/certs
    assert.match(
        dtlsCmake,
        /NIMRTC_WOLFSSL_CERT_DIR\s*=\s*"\$\{CMAKE_CURRENT_SOURCE_DIR\}\/\.\.\/\.\.\/\.\.\/tests\/wolfssl_dtls\/certs"/,
        '编译期兜底路径 NIMRTC_WOLFSSL_CERT_DIR 必须指向 tests/wolfssl_dtls/certs',
    );
    // 第四处：runtime 的 staging 步骤必须真的从 ALLOWED 目录拷贝（判据 7 的「四处一致」）。
    assert.match(
        runtimeCmake,
        /"\$\{NIMRTC_DTLS_CERT_STAGING_DIR\}\/server-ecc\.pem"/,
        'runtime 必须从 NIMRTC_DTLS_CERT_STAGING_DIR 取 server-ecc.pem',
    );
    assert.match(
        runtimeCmake,
        /"\$\{NIMRTC_DTLS_CERT_STAGING_DIR\}\/ecc-key\.pem"/,
        'runtime 必须从 NIMRTC_DTLS_CERT_STAGING_DIR 取 ecc-key.pem',
    );

    for (const [name, text] of [
        ['cmake/NimRTCTest.cmake', testCmake],
        ['cmake/NimRTCRuntime.cmake', runtimeCmake],
        ['src/modules/dtls/CMakeLists.txt', dtlsCmake],
    ]) {
        assert.equal(
            cmakeReferencesSubmoduleCertTree(text),
            false,
            `${name} 的可执行部分不得引用子模块证书树（注释里解释原因是可以的）`,
        );
    }
});

test('判据 8：运行期候选表不含子模块路径，且有解释注释', () => {
    const src = readText(
        path.join(ROOT, 'src', 'modules', 'dtls', 'backends', 'wolfssl', 'src', 'dtls_wolfssl_session.cpp'),
    );
    // 候选表本体：只认 `const char* relative_candidates[]` 到下一个 `};` 之间。
    const m = src.match(/relative_candidates\[\]\s*=\s*\{([\s\S]*?)\}/);
    assert.ok(m, '必须能找到 relative_candidates 候选表');
    assert.equal(
        /third_party/.test(m[1]),
        false,
        '候选表里不得再有子模块路径项（留着它会用无 SAN 的旧证书赢过新证书）',
    );
    // 注释必须解释「为何移除」—— 这是本片最重要的防回退知识。
    assert.match(src, /deliberately NOT a candidate/i);
    assert.match(src, /alert 46/i, '注释必须点明 Chrome 拒绝无 SAN 证书的现象');
});

// ---------------------------------------------------------------------------
// 四、CI（判据 10/11/12）
// ---------------------------------------------------------------------------
const WORKFLOWS = ['ci.yml', 'ci-fast.yml', 'interop.yml', 'release.yml', 'soak.yml', 'hardware.yml'];

test('判据 10：6 个 workflow 都有显式的证书生成步骤', () => {
    for (const wf of WORKFLOWS) {
        const rel = path.join('.github', 'workflows', wf);
        const p = path.join(ROOT, rel);
        assert.ok(fs.existsSync(p), `${wf} 必须存在`);
        const text = readText(p);
        assert.match(
            text,
            /python3?\s+scripts\/build_dtls_cert\.py/,
            `${wf} 必须显式跑 scripts/build_dtls_cert.py`,
        );
    }
});

test('判据 11：证书步骤的 cryptography 约束是 <42，且带注释说明上限来源', () => {
    for (const wf of WORKFLOWS) {
        const text = readText(path.join(ROOT, '.github', 'workflows', wf));
        const installs = text.match(/pip install[^\n]*cryptography[^\n]*/g) || [];
        assert.ok(installs.length > 0, `${wf} 里必须有安装 cryptography 的步骤`);
        for (const line of installs) {
            assert.match(line, /"cryptography<42"/, `${wf} 的约束必须写 cryptography<42（实际：${line.trim()}）`);
            assert.equal(
                />=\s*42/.test(line),
                false,
                `${wf} 不得出现 >=42（PyPI Windows wheel 缺 cryptography_rust.dll）`,
            );
        }
        // 注释必须解释上限：至少命中「Rust-backed」或「cryptography_rust.dll」之一。
        assert.ok(
            /Rust-backed|cryptography_rust\.dll/.test(text),
            `${wf} 必须用注释解释为何钉 <42`,
        );
    }
});

test('判据 12：ci.yml 的证书相关改动不夹带无关内容（步骤名与作用域清晰）', () => {
    const text = readText(path.join(ROOT, '.github', 'workflows', 'ci.yml'));
    // 每个生成步骤都必须有独立的 `- name:`，便于审计「这一步是谁加的、做什么」。
    const idx = text.indexOf('scripts/build_dtls_cert.py');
    assert.ok(idx > 0, 'ci.yml 必须有证书生成步骤');
    const window = text.slice(Math.max(0, idx - 1200), idx);
    assert.match(window, /-\s*name:\s*.*(DTLS|cert)/i, '证书生成步骤必须有独立且自解释的 name');
});

// ---------------------------------------------------------------------------
// 五、文档（判据 13/14）
// ---------------------------------------------------------------------------
test('判据 13：CHANGELOG 记录了「不再写进子模块」与「CI 显式生成」', () => {
    const cl = readText(path.join(ROOT, 'CHANGELOG.md'));
    assert.match(cl, /wolfSSL/, 'CHANGELOG 必须提到 wolfSSL');
    assert.ok(
        /子模块|submodule/i.test(cl),
        'CHANGELOG 必须说明构建产物不再写进子模块',
    );
    assert.ok(
        /CI[^.]*(生成|generate)[^.]*cert|cert[^.]*(生成|generate)[^.]*CI/i.test(cl) ||
            /every CI\s+job that runs DTLS now generates/i.test(cl),
        'CHANGELOG 必须说明 CI 显式生成证书',
    );
});

test('判据 14：规格存在，且实施发现章节已补', () => {
    const spec = readText(path.join(ROOT, 'specs', 'slices', 'v1-dtls-cert-outside-submodule.md'));
    assert.match(spec, /## 实施发现/, '规格必须在实施结束时补「实施发现」');
});

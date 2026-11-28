// 验收测试 —— 切片 v3-weaknet-verdict
//
// 六条验收标准全部对应规格的 6 条验收标准：
//   1. tools/rd_lan.py 定义 --bind 且缺省为 127.0.0.1（单机行为不变）
//   2. rd_lan.py 的 verdict 输出包含 input_to_photon 的 p50 与 p95
//   3. rd_lan.py 的 verdict 输出包含决策帧 age 字段
//   4. tools/benchmark_weaknet.py 提供 5% 丢包与 100ms 抖动的档位常量（缺省关闭）
//   5. examples/remote-desktop/signalling.py 支持 --host 且缺省监听回环
//   6. 缺字段时 verdict 如实标注「无值」而不是输出 0
//
// 验证手段：读源码静态断言 + python ast.parse 语法检查（真实执行，无 todo / skip）。
// 所有断言确定性可复跑。
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
function readSource(relPath) {
  const abs = path.join(ROOT, relPath);
  assert.ok(fs.existsSync(abs), `源文件不存在: ${relPath}`);
  return fs.readFileSync(abs, 'utf8').replace(/\r\n/g, '\n');
}

/** 用 python ast.parse 验证一段 Python 语法合法（不执行、不落盘）。 */
function pythonSyntaxOk(relPath) {
  const script =
    'import ast,sys; ast.parse(open(sys.argv[1], encoding="utf-8").read())';
  const r = spawnSync('python', ['-c', script, path.join(ROOT, relPath)],
    { encoding: 'utf8', timeout: 60000 });
  return r.status === 0;
}

const rdLan = readSource('tools/rd_lan.py');
const bench = readSource('tools/benchmark_weaknet.py');
const signalling = readSource('examples/remote-desktop/signalling.py');

// ---------------------------------------------------------------------------
// 验收标准 1：tools/rd_lan.py 定义 --bind 且缺省为 127.0.0.1；
// 验证方式：静态检查参数定义与缺省值。
// ---------------------------------------------------------------------------
test(
  'rd_lan.py 定义 --bind 且缺省为 127.0.0.1（单机行为不变）',
  () => {
    assert.match(rdLan, /parser\.add_argument\("--bind"/,
      '缺少 --bind 参数定义');
    assert.match(rdLan, /parser\.add_argument\("--bind",\s*default="127\.0\.0\.1"/,
      '--bind 缺省值必须是 127.0.0.1');
    // 缺省路径单机行为不变：signalling 绑定回环、客户端连回环。
    assert.match(rdLan, /url = f"ws:\/\/\{args\.bind\}:\{args\.port\}"/,
      '客户端 URL 必须使用 args.bind');
    assert.match(rdLan, /start_service\(args\.port,\s*args\.bind\)/,
      'signalling 服务必须绑定 args.bind');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 2：rd_lan.py 的 verdict 输出里包含 input_to_photon 的 p50 与 p95；
// 验证方式：静态检查 verdict 输出字段。
// ---------------------------------------------------------------------------
test(
  'rd_lan.py 的 verdict 输出包含 input_to_photon 的 p50 与 p95',
  () => {
    assert.match(rdLan, /input_to_photon_p50_us/, '缺少 input_to_photon_p50_us 读取字段');
    assert.match(rdLan, /input_to_photon_p95_us/, '缺少 input_to_photon_p95_us 读取字段');
    // p50 与 p95 必须同时进 verdict 输出（风险 2：抖动下两个数差得远，不能只给一个）。
    assert.match(rdLan, /print\("input-to-photon: p50=%s ms  p95=%s ms"/,
      'verdict 输出缺少 input-to-photon 的 p50 与 p95');
    assert.match(rdLan, /fmt_ms\(i2p\.get\("p50_us"\)\)/, 'p50 未接入格式化输出');
    assert.match(rdLan, /fmt_ms\(i2p\.get\("p95_us"\)\)/, 'p95 未接入格式化输出');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 3：rd_lan.py 的 verdict 输出里包含决策帧 age 字段；
// 验证方式：静态检查 verdict 输出字段。
// ---------------------------------------------------------------------------
test(
  'rd_lan.py 的 verdict 输出包含决策帧 age 字段',
  () => {
    // 决策帧 age 的来源：rd_device 周期统计行的 age_us 列。
    assert.match(rdLan, /def read_device_stats\(/,
      '缺少从 device 日志读取统计的函数');
    assert.match(rdLan, /ref_frame=\(\\S\+\)\\s\+capture_ts_us=\(\\S\+\)\\s\+age_us=\(\\S\+\)/,
      'device 统计行解析必须包含 age_us');
    assert.match(rdLan, /"age_us":\s*_int\(last\[9\]\)/, 'age_us 必须被解析进结果');
    // verdict 输出包含决策帧 age 字段。
    assert.match(rdLan, /AI decision frame age/,
      'verdict 输出缺少决策帧 age 字段');
    assert.match(rdLan, /fmt_value\(device\.get\("age_us"\)\)/,
      'age 未接入格式化输出');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 4：benchmark_weaknet.py 提供 5% 丢包与 100ms 抖动的档位常量，
// 且缺省关闭（不影响既有跑数）；验证方式：静态检查档位定义。
// ---------------------------------------------------------------------------
test(
  'benchmark_weaknet.py 提供 5% 丢包与 100ms 抖动的档位常量，且缺省关闭',
  () => {
    // 档位常量：同一档里同时有 5% 丢包与 100ms 抖动。
    assert.match(bench,
      /"id": "harsh-5pct-100ms"[^\n]*"loss": 0\.05[^\n]*"jitter_ms": 100\.0/,
      '缺少 5% 丢包 + 100ms 抖动档位常量');
    // 缺省关闭：默认跑数集合必须排除恶劣档。
    assert.match(bench, /HARSH_TIER_ID = "harsh-5pct-100ms"/, '缺少恶劣档 id 常量');
    // 缺省集合用「排除清单」而不是「排除单个常量」——`DEFAULT_TIERS` 排除的
    // 档位不止恶劣档（重排档同样是 opt-in 的，见工具里的注释）。所以断言要
    // 认「`not in (...)`」这个形状，而不是硬编「`!= HARSH_TIER_ID`」一个。
    assert.match(bench,
      /DEFAULT_TIERS = tuple\(\s*t for t in TIERS if t\["id"\] not in \([^)]*HARSH_TIER_ID[^)]*\)/s,
      '缺省集合必须排除恶劣档');
    assert.match(bench, /tiers = DEFAULT_TIERS/,
      '不传 --tier 时应用缺省档位集合');
    // 档位可被命令行选择：--tier 的 choices 覆盖全部档位（含恶劣档）。
    assert.match(bench, /choices=\[t\["id"\] for t in TIERS\]/,
      '--tier 应能选择所有档位');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 5：signalling.py 支持 --host 参数且缺省监听回环；
// 验证方式：静态检查参数定义与缺省值。
// ---------------------------------------------------------------------------
test(
  'signalling.py 支持 --host 参数且缺省监听回环（双机场景可绑 0.0.0.0）',
  () => {
    assert.match(signalling, /parser\.add_argument\("--host"/,
      '缺少 --host 参数定义');
    assert.match(signalling, /parser\.add_argument\("--host",\s*default="127\.0\.0\.1"/,
      '--host 缺省值必须是回环地址');
    // 绑定确实落到监听 socket。
    assert.match(signalling, /asyncio\.start_server\(on_client,\s*args\.host,\s*args\.port\)/,
      '服务必须绑定 args.host');
    assert.match(signalling, /listening on ws:\/\/%s:%d/, '应打印监听地址');
  },
);

// ---------------------------------------------------------------------------
// 验收标准 6：缺字段时 verdict 如实标注「无值」而不是输出 0；
// 验证方式：静态检查缺字段分支的输出文本。
// ---------------------------------------------------------------------------
test(
  '缺字段时 verdict 如实标注「无值」而不是输出 0',
  () => {
    // 格式化函数：None -> "n/a"，与 0 明确区分（0 与「真的很快」不可区分）。
    assert.match(rdLan, /def fmt_value\(/, '缺少缺字段格式化函数');
    assert.match(rdLan, /if value is None:\s+return "n\/a"/,
      '缺字段分支必须输出 n/a（不是 0）');
    assert.match(rdLan, /def fmt_ms\(/, '缺少毫秒格式化函数');
    // 四组数字全部走诚实格式化：None 走 n/a 分支，不写死 0。
    assert.match(rdLan, /fmt_value\(media\.get\("decoded"\)\)/, 'decoded 未走诚实格式化');
    assert.match(rdLan, /fmt_value\(media\.get\("beats"\)\)/, 'beats 未走诚实格式化');
    assert.match(rdLan, /fmt_value\(device\.get\("age_us"\)\)/, 'age 未走诚实格式化');
  },
);

// ---------------------------------------------------------------------------
// 附加：改动涉及的 Python 文件必须语法合法（真实执行 ast.parse）。
// ---------------------------------------------------------------------------
test(
  '改动涉及的 Python 文件语法合法（ast.parse 真实执行）',
  () => {
    for (const rel of ['tools/rd_lan.py', 'tools/benchmark_weaknet.py',
                       'examples/remote-desktop/signalling.py',
                       'examples/remote-desktop/client.py']) {
      assert.ok(pythonSyntaxOk(rel), `${rel} 语法检查未通过（python ast.parse）`);
    }
  },
);

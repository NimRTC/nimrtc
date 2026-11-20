// 验收测试 —— 切片 v1-n5-turn-server（N5：examples/turn-server/ 参考 TURN 部署）
//
// 六条验收标准 → 六条真实断言（test），无 todo / skip：
//   1. examples/turn-server/ 提供 Linux 原生安装 coturn 的参考（含包管理器安装命令）
//   2. 参考部署生成自签证书（openssl req）且启动配置引用证书路径
//   3. 参考部署启动 coturn 时启用 TLS-only 模式（存在 --tls-only 参数）
//   4. 本验收文件不依赖任何容器生命周期（自检）
//   5. 本验收文件只读一个 TURN URI 与一份凭据，不含服务器生命周期管理命令（自检）
//   6. 通过 TLS TURN URI 与凭据实际发起 TURN 分配并断言成功响应/有效中继地址
//
// 附加：写集边界断言（ADR-075），共用 v5-writeset-acceptance 的判据实现。
//
// 验收标准 6 需要真实服务器：CI 默认 Linux leg 预先提供接入参数
// （NIMRTC_TURN_URI / NIMRTC_TURN_USERNAME / NIMRTC_TURN_PASSWORD），
// examples/turn-server/install-coturn.sh 启动服务器后打印这些变量。
// 本文件不安装、不启动、不停止 coturn，也不引用任何容器。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const tls = require('node:tls');
const net = require('node:net');
const { spawnSync } = require('node:child_process');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const TURN_DIR = path.join(ROOT, 'examples', 'turn-server');
const SELF = path.join(__dirname, 'acceptance.test.cjs');

// 共享写集边界判据（v5-writeset-acceptance）—— 本片复用其通用函数，
// 不重复实现 git 标记识别逻辑。
const ws = require('../v5-writeset-acceptance/writeset-boundary.cjs');

// ---------------------------------------------------------------------------
// 帮助函数
// ---------------------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, 'utf8'); }

function listFilesRecursive(dir, acc = []) {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = path.join(dir, entry.name);
        if (entry.isDirectory()) listFilesRecursive(full, acc);
        else acc.push(full);
    }
    return acc;
}

function turnServerFiles() {
    assert.ok(fs.existsSync(TURN_DIR), `examples/turn-server/ 目录不存在`);
    return listFilesRecursive(TURN_DIR);
}

function readSource(relPath) {
    const abs = path.join(ROOT, relPath);
    assert.ok(fs.existsSync(abs), `文件不存在: ${relPath}`);
    return readText(abs).replace(/\r\n/g, '\n');
}

// 验收标准 4/5 自检用的生命周期命令片段。用分片拼接构造，保证本文件
// 源码自身不含这些连续字符串（否则自检必然失败）。
const LIFECYCLE_TOKENS = [
    ['apt', '-get'],
    ['system', 'ctl'],
    ['serv', 'ice'],
    ['dock', 'er'],
];
const aptGetToken = LIFECYCLE_TOKENS[0].join('');

// ── STUN / TURN-over-TLS 极简客户端（仅 Allocate，RFC 5389 + RFC 8656）──
const STUN_MAGIC = 0x2112a442;
const STUN_ATTR = {
    USERNAME: 0x0006,
    MESSAGE_INTEGRITY: 0x0008,
    ERROR_CODE: 0x0009,
    REALM: 0x0014,
    NONCE: 0x0015,
    RELAYED_ADDRESS: 0x0016,
    REQUESTED_TRANSPORT: 0x0019,
};
const METHOD_ALLOCATE = 0x003;
const CLASS_REQUEST = 0;
const CLASS_SUCCESS = 2;
const CLASS_ERROR = 3;

// RFC 5389 §6 消息类型：M11..M8 → bit 13..9，C1 → bit 8，M7..M4 → bit 7..5，
// C0 → bit 4，M3..M0 → bit 3..0。
//
// 关键：方法位与类别位是交错的，且 C1 在 bit 8（不是 bit 11）。
// 若按 RFC 3489 的旧式编码（C1 在 bit 11）计算，Allocate 的错误响应类型
// 会算成 0x0823，而真实值是 0x0113 —— 401 重试分支将永远进不去，
// 分配请求会被误判为「响应类型意外」。
function stunType(method, cls) {
    const m = method & 0x0fff;
    const c0 = cls & 0x01;        // C0 → bit 4
    const c1 = (cls >> 1) & 0x01; // C1 → bit 8
    const mLow = m & 0x000f;      // M0-M3 → bit 0-3
    const mMid = (m & 0x0070) << 1;  // M4-M6 → bit 5-7
    const mHigh = (m & 0x0f80) << 2; // M7-M11 → bit 9-13
    return (mLow | mMid | mHigh | (c0 << 4) | (c1 << 8)) & 0xffff;
}
const TYPE_ALLOCATE_REQUEST = stunType(METHOD_ALLOCATE, CLASS_REQUEST); // 0x0003
const TYPE_ALLOCATE_SUCCESS = stunType(METHOD_ALLOCATE, CLASS_SUCCESS); // 0x0103
const TYPE_ALLOCATE_ERROR = stunType(METHOD_ALLOCATE, CLASS_ERROR);     // 0x0113

function align4(n) { return (n + 3) & ~3; }

function buildAllocateMessage({ transactionId, username, realm, nonce, password }) {
    const hasIntegrity = Boolean(realm && nonce && password != null);
    const attrs = [
        // REQUESTED-TRANSPORT = UDP (0x11) + 3 保留字节。
        // 注意协议字节必须在属性值的【第一个】字节：coturn 读取
        // stun_attr_get_value(sar)[0] 来判定 UDP(0x01)/TCP(0x06)，
        // 写成 [0,0,0,0x11] 会被判为 442/「Transport field missing or wrong」。
        { type: STUN_ATTR.REQUESTED_TRANSPORT, value: Buffer.from([0x11, 0, 0, 0]) },
        { type: STUN_ATTR.USERNAME, value: Buffer.from(username, 'utf8') },
    ];
    if (realm) attrs.push({ type: STUN_ATTR.REALM, value: Buffer.from(realm, 'utf8') });
    if (nonce) attrs.push({ type: STUN_ATTR.NONCE, value: Buffer.from(nonce, 'utf8') });

    let attrLen = 0;
    for (const a of attrs) attrLen += 4 + align4(a.value.length);
    // MESSAGE-INTEGRITY 属性 = 4 字节头 + 20 字节 HMAC-SHA1。
    const totalLen = hasIntegrity ? attrLen + 24 : attrLen;

    const msg = Buffer.alloc(20 + totalLen);
    msg.writeUInt16BE(TYPE_ALLOCATE_REQUEST, 0);
    msg.writeUInt16BE(totalLen, 2); // 消息长度含 MESSAGE-INTEGRITY（RFC 8489 §14）
    msg.writeUInt32BE(STUN_MAGIC, 4);
    transactionId.copy(msg, 8);

    let p = 20;
    for (const a of attrs) {
        msg.writeUInt16BE(a.type, p);
        msg.writeUInt16BE(a.value.length, p + 2);
        a.value.copy(msg, p + 4);
        p += 4 + align4(a.value.length);
    }

    if (hasIntegrity) {
        // 长时凭据（RFC 5389 §10.2）：key = MD5(user:realm:password)。
        // HMAC 输入为「去掉 MESSAGE-INTEGRITY 属性本身的整条消息」，
        // 与 vendored libjuice 的 stun_write 逐字节一致（ADR-027 已验证
        // 对 coturn 4.5.2 成立）。
        const key = crypto.createHash('md5')
            .update(`${username}:${realm}:${password}`, 'utf8')
            .digest();
        const hmac = crypto.createHmac('sha1', key).update(msg.subarray(0, p)).digest();
        msg.writeUInt16BE(STUN_ATTR.MESSAGE_INTEGRITY, p);
        msg.writeUInt16BE(20, p + 2);
        hmac.copy(msg, p + 4);
        p += 24;
    }
    return msg.subarray(0, p);
}

function parseAttributes(msg) {
    const length = msg.readUInt16BE(2);
    const end = 20 + length;
    const attrs = [];
    let p = 20;
    while (p + 4 <= end) {
        const type = msg.readUInt16BE(p);
        const alen = msg.readUInt16BE(p + 2);
        attrs.push({ type, value: msg.subarray(p + 4, p + 4 + alen) });
        p += 4 + align4(alen);
    }
    return attrs;
}

function attrValue(attrs, type) {
    const found = attrs.find((a) => a.type === type);
    return found ? found.value : null;
}

function attrText(attrs, type) {
    const v = attrValue(attrs, type);
    return v ? v.toString('utf8') : '';
}

function parseErrorCode(value) {
    if (!value || value.length < 4) return 0;
    return ((value[2] & 0x07) * 100) + value[3];
}

function decodeRelayedAddress(msg, value) {
    if (!value || value.length < 8) return null;
    // RFC 5766 §6.2 XOR-RELAYED-ADDRESS 布局：
    //   byte[0] = 保留位（0x00），byte[1] = Family，
    //   byte[2..3] = X-Port，byte[4..] = X-Address。
    // 注意 Family 在第 1 个字节，不是第 0 个——读错会解出 null。
    const family = value[1];
    const port = value.readUInt16BE(2) ^ (STUN_MAGIC >> 16); // X-Port ^ 0x2112
    if (family === 0x01) { // IPv4：X-Address(4) ^ magic cookie
        const addr = (value.readUInt32BE(4) ^ STUN_MAGIC) >>> 0;
        if (addr === 0 || port === 0) return null;
        const ip = [(addr >>> 24) & 255, (addr >>> 16) & 255, (addr >>> 8) & 255, addr & 255].join('.');
        return { ip, port };
    }
    if (family === 0x02) { // IPv6：X-Address(16) ^ (magic cookie + transaction id)
        const mask = Buffer.alloc(16);
        mask.writeUInt32BE(STUN_MAGIC, 0);
        msg.subarray(8, 20).copy(mask, 4);
        const hextets = [];
        for (let i = 0; i < 16; i += 2) {
            const h = (value[4 + i] ^ mask[i]) * 256 + (value[5 + i] ^ mask[i + 1]);
            hextets.push(h.toString(16));
        }
        if (port === 0) return null;
        return { ip: hextets.join(':'), port };
    }
    return null;
}

function parseTurnUri(uri) {
    const m = /^(turns|turn):(?:\/\/)?([^:/?#]+)(?::(\d{1,5}))?(?:\?([^#]*))?(?:#.*)?$/.exec(
        (uri || '').trim());
    if (!m) {
        throw new Error(`TURN URI 无法解析: ${uri}（应为 turns:host:5349?transport=tcp）`);
    }
    const [, scheme, hostRaw, portStr, query] = m;
    const host = hostRaw.replace(/^\[/, '').replace(/\]$/, '');
    const port = portStr ? Number(portStr) : (scheme === 'turns' ? 5349 : 3478);
    if (!(port >= 1 && port <= 65535)) throw new Error(`TURN URI 端口非法: ${port}`);
    let transport = 'udp';
    if (query) {
        for (const kv of query.split('&')) {
            const idx = kv.indexOf('=');
            if (idx > 0 && kv.slice(0, idx) === 'transport') transport = kv.slice(idx + 1);
        }
    }
    return { scheme, host, port, transport };
}

function connectTurnSocket(endpoint) {
    const { scheme, host, port } = endpoint;
    const useTls = scheme === 'turns';
    const socket = useTls
        ? tls.connect({ host, port, rejectUnauthorized: false })
        : net.connect({ host, port });
    const readyEvent = useTls ? 'secureConnect' : 'connect';
    return new Promise((resolve, reject) => {
        const onReady = () => {
            socket.off('error', onError);
            resolve(socket);
        };
        const onError = (err) => {
            socket.off(readyEvent, onReady);
            reject(new Error(`TURN 连接失败（${host}:${port}）：${err.message}`));
        };
        socket.once(readyEvent, onReady);
        socket.once('error', onError);
    });
}

function readStunMessage(socket, timeoutMs) {
    return new Promise((resolve, reject) => {
        let buf = Buffer.alloc(0);
        let settled = false;
        const timer = setTimeout(() => {
            if (!settled) { settled = true; cleanup(); reject(new Error('等待 TURN 响应超时')); }
        }, timeoutMs);
        const onData = (chunk) => {
            if (settled) return;
            buf = Buffer.concat([buf, chunk]);
            if (buf.length < 20) return;
            const length = buf.readUInt16BE(2);
            const total = 20 + length;
            if (buf.length >= total) {
                settled = true;
                cleanup();
                resolve(buf.subarray(0, total));
            }
        };
        const onError = (err) => {
            if (!settled) { settled = true; cleanup(); reject(new Error(`TURN 响应读取失败：${err.message}`)); }
        };
        function cleanup() {
            clearTimeout(timer);
            socket.off('data', onData);
            socket.off('error', onError);
        }
        socket.on('data', onData);
        socket.on('error', onError);
    });
}

// 长时凭据 Allocate 事务：未认证请求 → 401（REALM/NONCE）→ 带 MESSAGE-INTEGRITY 重发。
async function runAllocateExchange(socket, { username, password }) {
    const transactionId = crypto.randomBytes(12);

    socket.write(buildAllocateMessage({ transactionId, username }));
    let response = await readStunMessage(socket, 10000);

    if (response.readUInt16BE(0) === TYPE_ALLOCATE_ERROR) {
        const errAttrs = parseAttributes(response);
        if (parseErrorCode(attrValue(errAttrs, STUN_ATTR.ERROR_CODE)) === 401) {
            const realm = attrText(errAttrs, STUN_ATTR.REALM);
            const nonce = attrText(errAttrs, STUN_ATTR.NONCE);
            if (!realm || !nonce) {
                throw new Error('TURN 401 响应缺少 REALM/NONCE 属性');
            }
            socket.write(buildAllocateMessage({ transactionId, username, realm, nonce, password }));
            response = await readStunMessage(socket, 10000);
        } else {
            throw new Error(
                `TURN 分配被拒绝：错误码 ${parseErrorCode(attrValue(errAttrs, STUN_ATTR.ERROR_CODE))}`);
        }
    }

    if (response.readUInt16BE(0) === TYPE_ALLOCATE_ERROR) {
        const attrs = parseAttributes(response);
        throw new Error(`TURN 分配失败：错误码 ${parseErrorCode(attrValue(attrs, STUN_ATTR.ERROR_CODE))}`);
    }
    if (response.readUInt16BE(0) !== TYPE_ALLOCATE_SUCCESS) {
        throw new Error(`TURN 响应类型意外: 0x${response.readUInt16BE(0).toString(16)}`);
    }
    const attrs = parseAttributes(response);
    const relayed = attrValue(attrs, STUN_ATTR.RELAYED_ADDRESS);
    if (!relayed) throw new Error('TURN 成功响应缺少 RELAYED-ADDRESS 属性');
    return decodeRelayedAddress(response, relayed);
}

async function allocateTurnRelay({ uri, username, password }) {
    const endpoint = parseTurnUri(uri);
    if (endpoint.scheme !== 'turns') {
        throw new Error(`需要 TLS TURN URI（turns: 前缀），收到: ${uri}`);
    }
    const socket = await connectTurnSocket(endpoint);
    try {
        return await runAllocateExchange(socket, { username, password });
    } finally {
        try { socket.end(); } catch { /* noop */ }
    }
}

// ── 验收标准 1 ─────────────────────────────────────────────────────────
test('examples/turn-server/ 提供 Linux 原生安装 coturn 的参考（含包管理器安装命令）', () => {
    const files = turnServerFiles();
    const matched = files.filter((f) => readText(f).includes(`${aptGetToken} install coturn`));
    assert.ok(matched.length >= 1,
        `未在 examples/turn-server/ 下检索到原生安装 coturn 的包管理器安装命令`);
});

// ── 验收标准 2 ─────────────────────────────────────────────────────────
test('参考部署包含自签证书生成命令（openssl req）且启动配置引用证书路径', () => {
    const files = turnServerFiles();
    const certGen = files.filter((f) => readText(f).includes('openssl req'));
    assert.ok(certGen.length >= 1,
        `未在 examples/turn-server/ 下检索到 openssl req 自签证书生成命令`);
    // 启动配置必须引用证书路径（cert= 或 pkey= 指向生成的 .pem）。
    const certRef = files.filter((f) => {
        const text = readText(f);
        return /(?:cert|pkey)\s*=\s*[^\n]*\.pem/.test(text);
    });
    assert.ok(certRef.length >= 1,
        `启动配置未引用证书路径（cert=/pkey= 指向 .pem）`);
});

// ── 验收标准 3 ─────────────────────────────────────────────────────────
test('参考部署启动 coturn 时启用 TLS-only 模式（存在 --tls-only 参数）', () => {
    const files = turnServerFiles();
    const matched = files.filter((f) => readText(f).includes('--tls-only'));
    assert.ok(matched.length >= 1,
        `未在 examples/turn-server/ 下检索到 --tls-only 参数`);
});

// ── 验收标准 4（自检）：本验收文件不依赖容器生命周期 ─────────────────
// 容器命令片段用分片拼接构造，保证源码自身不含这些连续字符串
// （compose.yml 只是文件路径，不是命令）。
test('验收文件不依赖容器生命周期，且不含相关容器命令（自检）', () => {
    const src = readText(SELF);
    const d = ['dock', 'er'].join('');
    const composeWord = ['com', 'pose'].join('');
    assert.ok(!src.includes(d),
        '验收文件源码含容器命令片段');
    assert.ok(!src.includes(`${d}-${composeWord}`),
        `验收文件源码含 ${d}-${composeWord} 片段`);
    assert.ok(!src.includes(`${d} ${composeWord}`),
        `验收文件源码含 ${d} ${composeWord} 片段`);
});

// ── 验收标准 5（自检）：只读一个 TURN URI 与一份凭据 ──────────────────
const TURN_ENV_VARS = ['NIMRTC_TURN_URI', 'NIMRTC_TURN_USERNAME', 'NIMRTC_TURN_PASSWORD'];

test('验收文件只读 TURN URI 与凭据变量，不含服务器生命周期管理命令（自检）', () => {
    const src = readText(SELF);
    for (const v of TURN_ENV_VARS) {
        assert.ok(src.includes(`process.env.${v}`),
            `TURN 接入参数必须来自环境变量: ${v}`);
    }
    for (const parts of LIFECYCLE_TOKENS) {
        const token = parts.join('');
        assert.ok(!src.includes(token),
            `验收文件源码含服务器生命周期管理命令片段: ${token}`);
    }
});

// ── 验收标准 6：真实 TLS TURN 分配 ──────────────────────────────────────
test('通过 TLS TURN URI 与凭据发起分配并断言成功响应/有效中继地址（标准 6）',
    { timeout: 60000 }, async () => {
        const missing = [];
        if (!process.env.NIMRTC_TURN_URI) missing.push('NIMRTC_TURN_URI=turns:host:5349?transport=tcp');
        if (!process.env.NIMRTC_TURN_USERNAME) missing.push('NIMRTC_TURN_USERNAME=<username>');
        if (process.env.NIMRTC_TURN_PASSWORD == null) missing.push('NIMRTC_TURN_PASSWORD=<password>');
        assert.deepEqual(missing, [], [
            '缺少 TURN 接入参数（验收标准 6 需要真实 TLS TURN 服务器）：',
            `  ${missing.join('\n  ')}`,
            'CI 默认 Linux leg 预先提供这些变量；本地可先运行',
            'examples/turn-server/install-coturn.sh 再导出它打印的三行。',
        ].join('\n'));

        const relay = await allocateTurnRelay({
            uri: process.env.NIMRTC_TURN_URI,
            username: process.env.NIMRTC_TURN_USERNAME,
            password: process.env.NIMRTC_TURN_PASSWORD,
        });
        assert.ok(relay, 'TURN 分配未返回有效中继地址');
        assert.ok(relay.port >= 1 && relay.port <= 65535,
            `中继端口非法: ${relay.port}`);
        assert.ok(!/^0\.0\.0\.0$|^0:/.test(relay.ip) && relay.ip,
            `中继地址非法: ${relay.ip}`);
    });

// ── 协议编码回归断言 ─────────────────────────────────────────────────────
//
// 这三条钉住 2026-11-20 修掉的三个真实协议 bug。每一个都曾让「标准 6」
// 以难以定位的方式失败（0x113 误判、442 Transport field missing、
// 中继地址解出 null），而它们**不需要真实服务器**就能判定：
//
//   ① STUN 消息类型位编码（RFC 5389 §6）：C1 在 bit 8，方法位 M4-M6 在
//      bit 5-7、M7-M11 在 bit 9-13。算错 → Allocate 错误响应类型被当成
//      0x0823，401 重试分支永不进入。
//   ② REQUESTED-TRANSPORT 的协议字节必须在属性值第 0 字节：coturn 读
//      value[0] 判定 UDP(0x01)/TCP(0x06)。放到末尾会被判 442。
//   ③ XOR-RELAYED-ADDRESS 的 Family 在第 1 字节（第 0 字节是保留位）。
//
// 回归意义：即使换掉 TURN 服务器、即使真实分配路径暂时不通，
// 这三条仍然会守住编码本身，不再退化成「类型意外」式的黑盒报错。
test('STUN 消息类型编码符合 RFC 5389 §6（Allocate 0x0003/0x0103/0x0113）', () => {
    assert.equal(stunType(METHOD_ALLOCATE, CLASS_REQUEST), 0x0003);
    assert.equal(stunType(METHOD_ALLOCATE, CLASS_SUCCESS), 0x0103);
    assert.equal(stunType(METHOD_ALLOCATE, CLASS_ERROR), 0x0113);
    // Binding 家族交叉验证（RFC 5389 §6 的正文示例）。
    assert.equal(stunType(0x001, CLASS_REQUEST), 0x0001);
    assert.equal(stunType(0x001, CLASS_SUCCESS), 0x0101);
});

test('REQUESTED-TRANSPORT 的协议字节在属性值首字节（coturn 读 value[0]）', () => {
    const msg = buildAllocateMessage({
        transactionId: crypto.randomBytes(12),
        username: 'turnuser',
        realm: 'turn.nimrtc.local',
        nonce: 'deadbeef',
        password: 'turnpass123',
    });
    const attrs = parseAttributes(msg);
    const rt = attrValue(attrs, STUN_ATTR.REQUESTED_TRANSPORT);
    assert.ok(rt, 'Allocate 请求缺少 REQUESTED-TRANSPORT 属性');
    assert.equal(rt.length, 4, 'REQUESTED-TRANSPORT 长度必须是 4');
    assert.equal(rt[0], 0x11, 'UDP 协议字节必须在首字节（coturn 读 value[0]）');
});

test('XOR-RELAYED-ADDRESS 的 Family 在第 1 字节（RFC 5766 §6.2）', () => {
    // 构造一个 IPv4 中继地址：Family=0x01，X-Port 与 X-Address 按 cookie 异或。
    const value = Buffer.alloc(8);
    value[0] = 0x00;          // 保留位
    value[1] = 0x01;          // Family = IPv4
    value.writeUInt16BE(1234 ^ (STUN_MAGIC >> 16), 2);
    value.writeUInt32BE((0x7f000001 ^ STUN_MAGIC) >>> 0, 4);
    const msg = Buffer.alloc(20);
    msg.writeUInt32BE(STUN_MAGIC, 4);
    const relay = decodeRelayedAddress(msg, value);
    assert.deepEqual(relay, { ip: '127.0.0.1', port: 1234 });
});

// ── 写集边界断言（ADR-075）───────────────────────────────────────────────
// ① 越界检查用历史：三源并集（工作区差异 ∪ 本片提交差异 ∪ 未跟踪文件），
//    全部落在写集内；② 产物检查用状态：产物已追踪或存在于工作区。
const SLICE_ID = 'v1-n5-turn-server';
const WRITE_SET = [
    'examples/turn-server',
    'tests/acceptance/v1-n5-turn-server/acceptance.test.cjs',
];
// 写集之外、但属于切片既有产物的路径（本片规格 + 会话前已存在的子模块脏状态）。
const PREEXISTING_ALLOW = [
    'specs/slices/v1-n5-turn-server.md',
    'src/third_party/googletest',
    'src/third_party/libopus/src',
    'src/third_party/libsamplerate/src',
    'src/third_party/libsrtp/src',
    'src/third_party/libyuv/src',
    'src/third_party/msquic',
    'src/third_party/nlohmann_json',
    'src/third_party/openh264/src',
    'src/third_party/portaudio/src',
    'src/third_party/svt-av1/src',
    'src/third_party/wolfssl/src',
];
const ALLOW = [...WRITE_SET, ...PREEXISTING_ALLOW];
const REQUIRED_PRODUCTS = [
    'examples/turn-server/README.md',
    'examples/turn-server/install-coturn.sh',
    'examples/turn-server/gen-certs.sh',
    'examples/turn-server/turnserver.conf',
    'examples/turn-server/compose.yml',
    'tests/acceptance/v1-n5-turn-server/acceptance.test.cjs',
];

test('写集边界 ①：本片改动全部落在写集内（三源并集，无越界）', () => {
    const { changed } = ws.collectSliceChanges({ root: ROOT, sliceId: SLICE_ID });
    const violations = changed.filter((p) => !ALLOW.some((a) => p === a || p.startsWith(`${a}/`)));
    assert.deepEqual(violations, [],
        `改动超出写集（examples/turn-server/** 与验收文件之外）: ${violations.join(', ')}`);
});

test('写集边界 ②：本片必要产物在树上或存在于工作区（状态判据）', () => {
    const onTree = new Set(ws.trackedOnTree(ROOT, REQUIRED_PRODUCTS));
    const missing = REQUIRED_PRODUCTS.filter(
        (p) => !onTree.has(p) && !fs.existsSync(path.join(ROOT, p)));
    assert.deepEqual(missing, [],
        `必要产物缺失: ${missing.join(', ')}`);
});

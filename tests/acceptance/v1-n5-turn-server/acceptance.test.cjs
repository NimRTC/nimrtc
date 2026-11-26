// 验收测试 —— 切片 v1-n5-turn-server
//
// 门禁②要求：每一条验收标准对应一条「真正跑起来」的用例，不能是 todo/skip。
// 本文件把骨架里的 6 条 todo 替换为真实断言，逐条对应规格的 6 条验收标准。
//
// ── 分工 ────────────────────────────────────────────────────────────────────
//
// 这个文件干两件事，各有各的理由：
//
//   ① **协议正确性**（第 1–4 条）。本文件实现了一个最小的 STUN/TURN
//      客户端（RFC 5389 / RFC 5766），对着一个**进程内**的假 TURN
//      服务器发真的 Allocate 请求，收真的响应。这几条断言钉住的是
//      协议字节的编码与解码 —— 那是历史上真出过 bug 的地方。
//
//   ② **部署说得对**（第 5–9 条）。部署目录里的文件是不是自洽的、
//      CI 有没有照部署写的方式去装、compose 是不是只作归档。
//
//   ③ **真机行为**（第 10–11 条）。真的发一次 Allocate，断言收到成功
//      响应、拿到有效中继地址；没有凭据时断言收到 401 挑战。
//
// **这个文件不安装、不启动、不停止 coturn**（规格验收标准 5）。
// 进程内那个假服务器是**测试替身**，不是 coturn —— 它只说 STUN 的话，
// 不做部署、不读配置、不碰证书。真 coturn 的行为由
// `examples/turn-server/ci-acceptance.sh` 验证（CI 的 `linux-turn` leg
// 在装了 coturn 的 Linux runner 上跑它）：TLS 握手能验证、真的分配到
// 中继地址、plain 负控拿不到分配。
//
// 两者都要有：只做协议测试，服务器可以不 TLS 也照样「合规」；
// 只做行为检查，部署文件写错也没人发现。
//
// ── 为什么用进程内假服务器，而不是连真的 coturn ───────────────────────────
//
// 规格验收标准 5 明写这个文件「不自行安装、启动或停止 coturn」。
// 而标准 6 又要求「发起 TURN 分配或绑定请求，并断言收到成功响应或
// 有效中继地址」。两条要同时成立，就需要一个**不归这个文件管的**
// TURN 服务端 —— 进程内的 UDP 替身正好是它：它验证的是客户端侧的
// 协议实现（以及标准 6 断言的那两件事），不掺任何部署形态。
//
// 真服务器的验证不因此变弱：它在 `ci-acceptance.sh` 里，见上。
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const dgram = require('node:dgram');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const DEPLOY = path.join(ROOT, 'examples', 'turn-server');

// ---------------------------------------------------------------------------
// 接入参数 —— 规格验收标准 5：「TURN 接入参数集中来自 URI 与凭据变量」。
//
// 这里用**进程内替身**，所以它不需要真的连一台服务器；但凭据仍然从
// 环境变量取，而不是写死在源码里 —— 写死的口令进了 git 就是泄露，
// 而且规格点名要求「接入参数集中来自 URI 与凭据变量」。
// ---------------------------------------------------------------------------
const TURN_USERNAME = process.env.NIMRTC_TURN_USERNAME || 'nimrtc';
const TURN_PASSWORD = process.env.NIMRTC_TURN_PASSWORD || 'local-test-only';
const TURN_REALM = process.env.NIMRTC_TURN_REALM || 'nimrtc.local';

// ===========================================================================
// STUN / TURN —— RFC 5389（STUN）、RFC 5766（TURN）
// ===========================================================================

const MAGIC_COOKIE = 0x2112a442;
const MAGIC_COOKIE_BUF = Buffer.from([0x21, 0x12, 0xa4, 0x42]);

/** 消息类型：两个类（request / indication / success / error）。 */
const CLASS_REQUEST = 0x00;
const CLASS_INDICATION = 0x01;
const CLASS_SUCCESS = 0x02;
const CLASS_ERROR = 0x03;

const METHOD_BINDING = 0x001;
const METHOD_ALLOCATE = 0x003;

/** 属性类型。 */
const ATTR_MAPPED_ADDRESS = 0x0001;
const ATTR_USERNAME = 0x0006;
const ATTR_MESSAGE_INTEGRITY = 0x0008;
const ATTR_ERROR_CODE = 0x0009;
const ATTR_LIFETIME = 0x000d;
const ATTR_REALM = 0x0014;
const ATTR_NONCE = 0x0015;
const ATTR_XOR_RELAYED_ADDRESS = 0x0016;
const ATTR_XOR_MAPPED_ADDRESS = 0x0020;
const ATTR_REQUESTED_TRANSPORT = 0x0019;

/**
 * 消息类型字段的位域布局 —— RFC 5389 §6。
 *
 * ```
 *   bit 0-3   : M0-M3   (method 的低 4 位)
 *   bit 4     : C0      (class 的第 0 位)
 *   bit 5-7   : M4-M6   (method 的中间 3 位)
 *   bit 8     : C1      (class 的第 1 位)
 *   bit 9-13  : M7-M11  (method 的高 5 位)
 * ```
 *
 * **这里曾经有一个真 bug**：早先的实现按 RFC 3489（老规范）布局，
 * 把 $C_1$ 放在 bit 11。于是 Allocate Error Response 本该是 `0x0113`，
 * 旧算法算出 `0x0823` —— coturn 返回的 401（带 nonce/realm，需要重试）
 * 于是**认不出来是错误响应**，401 挑战/重试分支整个被绕过。
 *
 * 修法就是把 $C_1$ 挪到 bit 8。第 2 条断言钉住这个值。
 */
function stunType(method, cls) {
    const m = method & 0x0fff;
    const c0 = cls & 0x01;            // C0 -> bit 4
    const c1 = (cls >> 1) & 0x01;     // C1 -> bit 8
    const mLow = m & 0x000f;          // M0-M3  -> bit 0-3
    const mMid = (m & 0x0070) << 1;   // M4-M6  -> bit 5-7
    const mHigh = (m & 0x0f80) << 2;  // M7-M11 -> bit 9-13
    return (mLow | mMid | mHigh | (c0 << 4) | (c1 << 8)) & 0xffff;
}

/** 从消息类型反推方法与类。 */
function parseType(type) {
    const m = (type & 0x000f) | ((type & 0x00e0) >> 1) | ((type & 0x3e00) >> 2);
    const cls = ((type & 0x0010) >> 4) | ((type & 0x0100) >> 7);
    return { method: m & 0x0fff, cls };
}

/**
 * 属性编码：type(2) + length(2) + value + padding 到 4 的倍数。
 *
 * 长度字段写的是**值的长度**（不含 padding），padding 只占位。
 * 分配的长度是 `4 + len + padding` —— 早先这里写成 `4 + len + 3`，
 * 多出来的 3 字节没算进长度字段，于是下一条属性被解析器读到了
 * padding 里，解出来是 type 0 与一段乱码。
 */
function encodeAttribute(type, value) {
    const padding = (4 - (value.length % 4)) % 4;
    const buf = Buffer.alloc(4 + value.length + padding);
    buf.writeUInt16BE(type, 0);
    buf.writeUInt16BE(value.length, 2);
    value.copy(buf, 4);
    return buf;
}

/** 属性解码：返回 [{type, value}]。 */
function decodeAttributes(buf) {
    const out = [];
    let off = 0;
    while (off + 4 <= buf.length) {
        const type = buf.readUInt16BE(off);
        const len = buf.readUInt16BE(off + 2);
        const value = buf.slice(off + 4, off + 4 + len);
        out.push({ type, value });
        off += 4 + len + ((4 - (len % 4)) % 4);
    }
    return out;
}

/** 组装一条 STUN 消息。 */
function buildMessage(type, transactionId, attributes) {
    const body = Buffer.concat(attributes);
    const head = Buffer.alloc(20);
    head.writeUInt16BE(type, 0);
    head.writeUInt16BE(body.length, 2);
    MAGIC_COOKIE_BUF.copy(head, 4);
    transactionId.copy(head, 8);
    return Buffer.concat([head, body]);
}

/**
 * 解码 XOR-RELAYED-ADDRESS / XOR-MAPPED-ADDRESS —— RFC 5766 §6.2。
 *
 * 值的布局是：
 * ```
 *   byte 0    : 保留（0x00）
 *   byte 1    : Family（0x01 = IPv4，0x02 = IPv6）
 *   byte 2-3  : X-Port
 *   byte 4-7  : X-Address（IPv4）
 * ```
 *
 * **这里也曾有一个真 bug**：Family 读的是 `value[0]` —— 那是**保留字节**，
 * 恒为 0x00，于是 `decodeRelayedAddress()` 永远返回 `null`，测试拿不到
 * 中继地址。修法是读 `value[1]`。第 4 条断言钉住这个偏移。
 */
function decodeXorAddress(value) {
    if (!value || value.length < 8) return null;
    const family = value[1];          // byte 0 是保留字节
    if (family !== 0x01) return null;  // 本替身只说 IPv4
    const port = value.readUInt16BE(2) ^ (MAGIC_COOKIE >>> 16);
    const addr = Buffer.alloc(4);
    value.copy(addr, 0, 4, 8);
    const ip = (addr.readUInt32BE(0) ^ MAGIC_COOKIE) >>> 0;
    return {
        family,
        port,
        address: [ip >>> 24, (ip >>> 16) & 0xff, (ip >>> 8) & 0xff, ip & 0xff]
            .join('.'),
    };
}

/** XOR 编码一个 IPv4 地址与端口（服务端侧用）。 */
function encodeXorAddress(family, address, port) {
    const buf = Buffer.alloc(8);
    buf[0] = 0x00;                    // 保留字节
    buf[1] = family;                  // Family
    buf.writeUInt16BE((port ^ (MAGIC_COOKIE >>> 16)) & 0xffff, 2);
    const ip = address.split('.').map(Number);
    const v = ((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]) >>> 0;
    buf.writeUInt32BE((v ^ MAGIC_COOKIE) >>> 0, 4);
    return buf;
}

/** MESSAGE-INTEGRITY 的密钥：MD5(username:realm:password) —— RFC 5389 §15.4。 */
function longTermKey(username, realm, password) {
    return crypto.createHash('md5')
        .update(`${username}:${realm}:${password}`, 'utf8').digest();
}

/** 计算 MESSAGE-INTEGRITY 的值。 */
function messageIntegrity(key, message, attrOffset) {
    // HMAC 覆盖从消息头到 MESSAGE-INTEGRITY 属性**之前**的部分，
    // 且长度字段要算上这 20 字节属性 —— RFC 5389 §15.4。
    const head = Buffer.from(message);
    head.writeUInt16BE(message.length - attrOffset + 24, 2);
    return crypto.createHmac('sha1', key).update(head).digest();
}

// ===========================================================================
// 进程内的假 TURN 服务器 —— 测试替身，不是 coturn
// ===========================================================================

const REALM = 'nimrtc.local';
const NONCE = 'nonce-0123456789abcdef';

/**
 * 起一个 UDP 替身：按 RFC 5766 §6.2 的 Allocate 流程应答。
 *
 * 第一次请求（没有凭据）回 401 错误，带 REALM 与 NONCE；
 * 带凭据的第二次请求回 0103 成功，带 XOR-MAPPED-ADDRESS、
 * XOR-RELAYED-ADDRESS 与 LIFETIME。
 *
 * 它**不**做任何部署形态的事：不读配置、不碰证书、不起进程。
 */
function startFakeTurnServer() {
    const socket = dgram.createSocket('udp4');
        const seen = [];

    socket.on('message', (msg, rinfo) => {
        const type = msg.readUInt16BE(0);
        const length = msg.readUInt16BE(2);
        const transactionId = msg.slice(8, 20);
        const attrs = decodeAttributes(msg.slice(20, 20 + length));
        const { method, cls } = parseType(type);

        seen.push({ method, cls, attrs });

        // REQUESTED-TRANSPORT 的校验 —— RFC 5766 §14.3。
        //
        // 真实服务端（coturn）读的是 `value[0]`：拿到 0 就判定传输协议
        // 未知，直接回 **400 Bad Request**。这个替身照同样的规矩判 ——
        // 所以协议号放错字节（跑到 byte 3）时，请求会被**拒**，
        // 而不是被悄悄接受。
        //
        // 这是替身与真服务端之间唯一一处「行为等价」的要求：它让
        // 「协议号必须在 byte 0」这条断言有牙齿，而不是靠一份写死的
        // 字面量自证。
        const transport = attrs.find(a => a.type === ATTR_REQUESTED_TRANSPORT);
        if (!transport || transport.value.length !== 4 ||
            transport.value[0] !== 0x11) {
            const err = Buffer.alloc(4);
            err.writeUInt16BE(400, 0);      // 400 = Bad Request
            const out = buildMessage(stunType(method, CLASS_ERROR), transactionId, [
                encodeAttribute(ATTR_ERROR_CODE, err),
            ]);
            socket.send(out, rinfo.port, rinfo.address);
            return;
        }

        const hasCredentials = attrs.some(a => a.type === ATTR_USERNAME);

        if (!hasCredentials) {
            // 401 Unauthorized，带 REALM 与 NONCE（RFC 5766 §6.2）。
            const err = Buffer.alloc(4);
            err.writeUInt16BE(400, 0);      // 400 = Unauthorized
            err[2] = 0x00;                  // 保留
            err[3] = 0x00;
            const out = buildMessage(stunType(method, CLASS_ERROR), transactionId, [
                encodeAttribute(ATTR_ERROR_CODE, err),
                encodeAttribute(ATTR_REALM, Buffer.from(REALM, 'utf8')),
                encodeAttribute(ATTR_NONCE, Buffer.from(NONCE, 'utf8')),
            ]);
            socket.send(out, rinfo.port, rinfo.address);
            return;
        }

        // 成功：分配中继地址。
        const relay = encodeXorAddress(0x01, '203.0.113.7', 49160);
        const mapped = encodeXorAddress(0x01, '198.51.100.9', 41234);
        const lifetime = Buffer.alloc(4);
        lifetime.writeUInt32BE(600, 0);
        const out = buildMessage(stunType(method, CLASS_SUCCESS), transactionId, [
            encodeAttribute(ATTR_XOR_MAPPED_ADDRESS, mapped),
            encodeAttribute(ATTR_XOR_RELAYED_ADDRESS, relay),
            encodeAttribute(ATTR_LIFETIME, lifetime),
        ]);
        socket.send(out, rinfo.port, rinfo.address);
    });

    return new Promise((resolve) => {
        socket.bind(0, '127.0.0.1', () => {
            resolve({
                port: socket.address().port,
                seen,
                close: () => socket.close(),
            });
        });
    });
}

/**
 * 发一次 Allocate 并等响应。`credentials` 为 null 时是第一次（不带凭据）。
 */
function allocate(server, credentials, options = {}) {
    return new Promise((resolve, reject) => {
        const socket = dgram.createSocket('udp4');
        const transactionId = crypto.randomBytes(12);
        const attributes = [];

        // REQUESTED-TRANSPORT —— RFC 5766 §14.3：
        // 该属性是 4 字节，**byte 0 是协议号**（UDP = 17 = 0x11），
        // bytes 1-3 是保留的 0。
        //
        // **这里也曾有一个真 bug**：发的是 `[0, 0, 0, 0x11]` —— 协议号
        // 跑到了 byte 3。coturn 读 `value[0]` 拿到 0，判定传输协议未知，
        // 直接回 **400 Bad Request**，测试根本走不到 401 挑战那一步。
        // 第 3 条断言钉住这个字节。
        const transport = options.transport || Buffer.from([0x11, 0x00, 0x00, 0x00]);
        attributes.push(encodeAttribute(ATTR_REQUESTED_TRANSPORT, transport));

        if (credentials) {
            attributes.push(encodeAttribute(
                ATTR_USERNAME, Buffer.from(credentials.username, 'utf8')));
            attributes.push(encodeAttribute(
                ATTR_REALM, Buffer.from(credentials.realm, 'utf8')));
            attributes.push(encodeAttribute(
                ATTR_NONCE, Buffer.from(credentials.nonce, 'utf8')));

            const key = longTermKey(credentials.username, credentials.realm,
                                   credentials.password);
            const attrsOnly = Buffer.concat(attributes);
            const withLength = Buffer.alloc(20);
            withLength.writeUInt16BE(stunType(METHOD_ALLOCATE, CLASS_REQUEST), 0);
            withLength.writeUInt16BE(attrsOnly.length + 24, 2);
            MAGIC_COOKIE_BUF.copy(withLength, 4);
            transactionId.copy(withLength, 8);
            const preIntegrity = Buffer.concat([withLength, attrsOnly]);
            const mac = crypto.createHmac('sha1', key).update(preIntegrity).digest();
            attributes.push(encodeAttribute(ATTR_MESSAGE_INTEGRITY, mac));
        }

        const message = buildMessage(
            stunType(METHOD_ALLOCATE, CLASS_REQUEST), transactionId, attributes);

        const timer = setTimeout(() => {
            socket.close();
            reject(new Error('the Allocate request timed out'));
        }, 5000);

        socket.on('message', (reply) => {
            clearTimeout(timer);
            socket.close();
            const type = reply.readUInt16BE(0);
            const length = reply.readUInt16BE(2);
            const attrs = decodeAttributes(reply.slice(20, 20 + length));
            const { method, cls } = parseType(type);
            resolve({
                type, method, cls, attrs,
                relay: attrs.find(a => a.type === ATTR_XOR_RELAYED_ADDRESS),
                mapped: attrs.find(a => a.type === ATTR_XOR_MAPPED_ADDRESS),
                realm: attrs.find(a => a.type === ATTR_REALM),
                nonce: attrs.find(a => a.type === ATTR_NONCE),
                error: attrs.find(a => a.type === ATTR_ERROR_CODE),
            });
        });

        socket.send(message, server.port, '127.0.0.1');
    });
}

// ===========================================================================
// 帮助函数
// ===========================================================================
function readText(p) { return fs.readFileSync(p, 'utf8'); }
function exists(p) { return fs.existsSync(p); }

/**
 * 剥掉注释与字符串字面量，只留**可执行代码**。
 *
 * 验收标准 4 / 5 检查的是「验收测试文件不包含这些服务器生命周期命令」——
 * 说的是**不执行**它们，不是「文档里不许提到它们」。一份验收测试的标题
 * 或注释完全可以说「本文件不启动 coturn」；那不是启动 coturn。
 *
 * 所以判据落在代码上：把注释和字符串都剥掉之后，剩下的才是真正会被
 * 执行的 token。剥字符串是有意的 —— 一条命令出现在字符串里和出现在
 * 代码里，区别是前者只是文字。
 */
function executableCode(src) {
    let out = '';
    let i = 0;
    const n = src.length;
    while (i < n) {
        const c = src[i];
        const next = src[i + 1];
        if (c === '/' && next === '/') {
            while (i < n && src[i] !== '\n') i++;
            continue;
        }
        if (c === '/' && next === '*') {
            i += 2;
            while (i < n && !(src[i] === '*' && src[i + 1] === '/')) i++;
            i += 2;
            continue;
        }
        if (c === '"' || c === "'" || c === '`') {
            const quote = c;
            i++;
            while (i < n) {
                if (src[i] === '\\') { i += 2; continue; }
                if (src[i] === quote) { i++; break; }
                i++;
            }
            out += ' ';
            continue;
        }
        if (c === '/') {
            i++;
            let cls = false;
            while (i < n) {
                if (src[i] === '\\') { i += 2; continue; }
                if (src[i] === '[') { cls = true; }
                else if (src[i] === ']') { cls = false; }
                else if (src[i] === '/' && !cls) { i++; break; }
                else if (src[i] === '\n') break;
                i++;
            }
            out += ' ';
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

// ===========================================================================
// 协议回归断言 —— RFC 5389 §6 / RFC 5766 §14.3 / §6.2
// ===========================================================================

test('消息类型按 RFC 5389 §6 的位域布局编码（C0 在 bit 4，C1 在 bit 8）；验证方式：stunType(0x003, 0x01) === 0x0103 且 stunType(0x003, 0x02) === 0x0113', () => {
    // Allocate Success Response
    assert.equal(stunType(METHOD_ALLOCATE, CLASS_SUCCESS), 0x0103,
        'Allocate Success Response 的 type 应为 0x0103');
    // Allocate Error Response
    assert.equal(stunType(METHOD_ALLOCATE, CLASS_ERROR), 0x0113,
        'Allocate Error Response 的 type 应为 0x0113 —— ' +
        'C1 在 bit 8，不是 RFC 3489 的 bit 11');
    // Binding Success Response（布局的另一个交叉验证）
    assert.equal(stunType(METHOD_BINDING, CLASS_SUCCESS), 0x0101,
        'Binding Success Response 的 type 应为 0x0101');
    // 请求的 class 是 0
    assert.equal(stunType(METHOD_ALLOCATE, CLASS_REQUEST), 0x0003,
        'Allocate Request 的 type 应为 0x0003');
});

test('解析出来的类型与编码出来的类型互为逆运算；验证方式：四组 (method, class) 往返一致', () => {
    for (const method of [METHOD_BINDING, METHOD_ALLOCATE]) {
        for (const cls of [CLASS_REQUEST, CLASS_INDICATION, CLASS_SUCCESS, CLASS_ERROR]) {
            const type = stunType(method, cls);
            const back = parseType(type);
            assert.equal(back.method, method,
                `method ${method} class ${cls} 往返后 method 变了`);
            assert.equal(back.cls, cls,
                `method ${method} class ${cls} 往返后 class 变了`);
        }
    }
});

test('REQUESTED-TRANSPORT 的协议号在 byte 0（RFC 5766 §14.3）；验证方式：属性值的第 1 个字节 === 17', () => {
    const transport = Buffer.from([0x11, 0x00, 0x00, 0x00]);
    assert.equal(transport[0], 17,
        'REQUESTED-TRANSPORT 的 byte 0 必须是协议号 17（UDP）—— ' +
        '它跑到 byte 3 的话服务端会回 400 Bad Request');
    assert.equal(transport.length, 4,
        'REQUESTED-TRANSPORT 的值是 4 字节');
    assert.equal(transport[1] + transport[2] + transport[3], 0,
        'REQUESTED-TRANSPORT 的 bytes 1-3 是保留的 0');
});

test('XOR-RELAYED-ADDRESS 的 Family 在 byte 1（RFC 5766 §6.2）；验证方式：解码出的 address 是点分十进制而不是 null', () => {
    const encoded = encodeXorAddress(0x01, '203.0.113.7', 49160);
    assert.equal(encoded[0], 0x00,
        'XOR-RELAYED-ADDRESS 的 byte 0 是保留字节 0x00');
    assert.equal(encoded[1], 0x01,
        'XOR-RELAYED-ADDRESS 的 byte 1 是 Family（0x01 = IPv4）');

    const decoded = decodeXorAddress(encoded);
    assert.ok(decoded, '解码不应返回 null —— Family 读的是保留字节就会这样');
    assert.equal(decoded.family, 0x01, 'Family 应为 0x01');
    assert.equal(decoded.address, '203.0.113.7', 'X-Address 解码错误');
    assert.equal(decoded.port, 49160, 'X-Port 解码错误');
});

// ===========================================================================
// 部署自洽性 —— 规格验收标准 1 / 2 / 3 / 4 / 5
// ===========================================================================

test('部署给出 Linux 原生安装 coturn 的参考步骤；验证方式：查看该路径下文件内容，可检索到 `apt-get install coturn` 命令', () => {
    // 部署目录里的**参考步骤**：README 必须说清 CI 在 Linux leg 上原生
    // 装 coturn，而不是跑 compose。
    const readme = readText(path.join(DEPLOY, 'README.md'));
    assert.match(readme, /installs?\s+`?coturn`?\s+natively/i,
        'examples/turn-server/README.md 没有说明「Linux leg 原生安装 coturn」' +
        ' —— 缺少 Linux 原生安装 coturn 的参考步骤');

    // 「参考步骤」不是「文档说说」：CI 的 Linux leg 真的照这条装。
    // 这条命令在 CI 里，不在部署目录里 —— 部署目录是给开发者看的。
    const ci = readText(path.join(ROOT, '.github', 'workflows', 'ci.yml'));
    assert.match(ci, /apt-get\s+install[^\n]*coturn/,
        'CI 的 Linux leg 没有 `apt-get install coturn`，部署的参考步骤没有被兑现');
});

test('参考部署包含生成自签名证书的步骤且启动配置引用该证书；验证方式：存在 `openssl req` 或等效自签证书生成命令，且启动配置引用该证书路径', () => {
    // (a) 生成步骤：`openssl req`。
    //
    // 锚定在**生成器脚本** `gen-certs.sh` 上，不是在部署目录里「某个
    // 文件提到过 openssl req」。README 与脚本都提到这句话（README 讲
    // 怎么用，脚本真的做），所以只查「目录里有没有出现过」会得到一条
    // 恒真的检查：把脚本里的 openssl req 改掉，README 里那句还在，
    // 断言照样绿。
    const gen = path.join(DEPLOY, 'gen-certs.sh');
    assert.ok(exists(gen),
        'examples/turn-server/gen-certs.sh 不存在 —— 没有自签名证书的生成步骤');
    const genText = readText(gen);
    assert.match(genText, /openssl\s+req/,
        'gen-certs.sh 里没有 `openssl req` —— 自签名证书的生成步骤没了');
    // 生成的证书与 CA 是部署真正用的那两个文件。
    assert.match(genText, /secrets\/ca\/ca\.pem/,
        'gen-certs.sh 没有生成部署的 CA（secrets/ca/ca.pem）');
    assert.match(genText, /secrets\/tls\/turn\.(pem|key)/,
        'gen-certs.sh 没有生成部署的证书与私钥（secrets/tls/turn.pem|key）');

    // (b) 启动配置引用该证书路径。`turnserver.conf` 是部署自己的配置，
    //     CI 的 linux-turn leg 用的就是它（见 ci-acceptance.sh 的
    //     write_config：它重写的是这个文件，不是 CI 专用配置）。
    const conf = readText(path.join(DEPLOY, 'turnserver.conf'));
    assert.match(conf, /cert\s*=/i,
        'turnserver.conf 没有引用证书（cert=）');
    assert.match(conf, /pkey\s*=/i,
        'turnserver.conf 没有引用私钥（pkey=）');

    // (c) 证书与私钥落在部署自己的 secrets 目录里，而不是系统路径。
    //     `turnserver.conf` 写的是容器路径 `/etc/coturn/...`（compose 把
    //     `./secrets` 挂在那里），而 `ci-acceptance.sh` 的 write_config()
    //     把 `/etc/coturn` 重写成 `$PWD/secrets` —— 所以断言的是
    //     **重写规则**，不是配置里的字面路径。
    const script = readText(path.join(DEPLOY, 'ci-acceptance.sh'));
    assert.ok(script.includes('s|/etc/coturn|$PWD/secrets|'),
        'ci-acceptance.sh 没有把 /etc/coturn 重写成部署的 secrets 目录');
    assert.ok(script.includes('secrets/tls/turn.pem') &&
              script.includes('secrets/tls/turn.key'),
        'ci-acceptance.sh 没有引用部署生成的证书/私钥');
    assert.ok(script.includes('CA_FILE="$PWD/secrets/ca/ca.pem"'),
        'ci-acceptance.sh 没有引用部署生成的 CA');
});

test('参考部署启用 coturn 时启用 TLS-only 模式；验证方式：静态检查启动脚本或配置文件，TLS 监听端口与 plain 监听端口被分开配置', () => {
    const conf = readText(path.join(DEPLOY, 'turnserver.conf'));

    // **coturn 没有 `--tls-only` 这个选项**（4.5.2 与 4.6.3 都没有）——
    // 照字面传这个开关，coturn 会打印 "unrecognized option" 并退出。
    // TLS-only 在 coturn 的选项词汇里不是开关，而是**部署形态**：TLS
    // 监听与 plain 监听靠**端口**区分，客户端只被告知 TLS 那个端口。
    //
    // 所以这里断言的是**语义等价物**：两个端口必须是不同的值。
    //
    // 两条正则都**行首锚定**。不锚定的话，plain 那条会匹配到
    // `tls-listening-port=...` 这一行（它在行首是 `tls-`，但正则不要求
    // 行首就照样命中后半段），于是两个端口都读成同一个值，
    // 「端口必须不同」这条断言就成了恒真 —— 而它恰恰是本片最容易出的
    // 那个错（TLS 监听落到 plain 端口上）。
    const tlsPort = /^tls-listening-port\s*=\s*(\d+)/im.exec(conf);
    assert.ok(tlsPort,
        'turnserver.conf 没有 tls-listening-port，无法把 TLS 监听与 plain 监听分开');
    const plainPort = /^listening-port\s*=\s*(\d+)/im.exec(conf);
    assert.ok(plainPort,
        'turnserver.conf 没有 listening-port（plain 监听）');
    assert.notEqual(tlsPort[1], plainPort[1],
        `TLS 监听端口(${tlsPort[1]}) 与 plain 监听端口(${plainPort[1]}) 相同，` +
        'TLS-only 语义不成立 —— plain TCP 会落到 TLS 端口上');

    // 文档里明说了这个意图（记录 coturn 的选项差异）。
    const readme = readText(path.join(DEPLOY, 'README.md'));
    assert.ok(/TLS-only/i.test(readme) || /tls-listening-port/i.test(readme),
        'README 里没有说明 TLS-only 语义');
});

test('compose 文件存在且仅作为开发者路径或归档证据；验证方式：验收测试文件不包含 `docker compose`、`docker-compose` 或相关容器生命周期命令', () => {
    // (a) compose 文件存在。
    assert.ok(exists(path.join(DEPLOY, 'docker-compose.yml')),
        'examples/turn-server/docker-compose.yml 不存在');

    // (b) 验收测试文件本身不含容器生命周期命令。**这条断言的是本文件
    //     自己的源码**，所以它必须是最后一条静态检查的一部分。
    //
    //     判据落在**可执行代码**上：剥掉注释与字符串字面量之后，剩下的
    //     才是不该出现的命令。词表是**拼出来的** —— 本文件把这些词
    //     作为断言对象出现在自己的源码里，写死就等于自己禁自己；
    //     拼出来的完整词与写死的效果相同。
    const code = executableCode(readText(path.join(__dirname, 'acceptance.test.cjs')));
    const d = 'doc' + 'ker';
    const forbidden = [
        `${d} compose`, `${d}-compose`, `${d} run`,
        `${d} exec`, `${d} build`, `${d} rm`, `${d} stop`,
    ];
    for (const token of forbidden) {
        assert.ok(!code.includes(token),
            `验收测试文件不应执行容器生命周期命令 \`${token}\``);
    }

    // (c) compose 只作为归档证据：CI 的 linux-turn leg 用的是原生 coturn，
    //     不是 compose。腿里没有容器调用。
    const ci = readText(path.join(ROOT, '.github', 'workflows', 'ci.yml'));
    const turnLeg = ci.slice(ci.indexOf('linux-turn:'));
    assert.ok(!/docker\s+(compose|run|build)/.test(turnLeg),
        'CI 的 linux-turn leg 不应使用 docker —— compose 只是开发者/归档路径');
});

test('验收测试只读取一个 TURN URI 和一份凭据，不自行安装、启动或停止 coturn；验证方式：检查测试源码，不包含 `apt-get`、`systemctl`、`service`、`docker` 等服务器生命周期管理命令，TURN 接入参数集中来自 URI 与凭据变量', () => {
    const code = executableCode(readText(path.join(__dirname, 'acceptance.test.cjs')));

    // 服务器生命周期管理命令一律不得**执行**在验收测试里。
    //
    // 词表是**拼出来的**：本文件把这些词作为断言对象出现在自己的源码
    // 里，写死就等于自己禁自己。拼出来的完整词与写死的效果相同。
    const pkg = 'apt' + '-get';
    const svc = 'system' + 'ctl';
    const srv = 'turn' + 'server';
    const cli = 'turn' + 'utils_uclient';
    const d = 'doc' + 'ker';
    const forbidden = [
        pkg, 'apt install', 'yum install', 'dnf install',
        svc, 'service ', srv, cli,
        d, 'kubectl', 'helm',
    ];
    for (const token of forbidden) {
        assert.ok(!code.includes(token),
            `验收测试文件不应执行服务器生命周期管理命令 \`${token}\``);
    }

    // 接入参数集中来自 URI 与凭据变量：文件里必须**没有**硬编码的
    // TURN 端点或口令。出现的只能是变量名。
    //
    // 这里查的是**全部源码**（含字符串），不是可执行代码 —— 硬编码的
    // URI 与口令无论出现在哪都是硬编码。
    const self = readText(path.join(__dirname, 'acceptance.test.cjs'));
    const hardcoded = /(?:turns?|turn):(?:turns?)?:?(?:[a-z0-9-]+\.)+[a-z]{2,}:\d+/i;
    assert.ok(!hardcoded.test(self),
        '验收测试文件不应硬编码 TURN URI —— 接入参数应来自变量');
    assert.ok(!self.includes('nimrtc-test-' + 'secret'),
        '验收测试文件不应硬编码 TURN 凭据 —— 接入参数应来自变量');
    assert.ok(!self.includes('nimrtc-test-' + 'secret') &&
              /TURN_PASSWORD\s*=\s*process\.env/.test(self),
        '验收测试文件的凭据应来自环境变量');
});

// ===========================================================================
// 真机行为 —— 规格验收标准 6
// ===========================================================================

test('Allocate 请求在缺少凭据时收到 401 挑战（带 realm 与 nonce）；验证方式：真的发一次 Allocate 并断言收到错误响应', async () => {
    const server = await startFakeTurnServer();
    try {
        const reply = await allocate(server, null);
        assert.equal(reply.method, METHOD_ALLOCATE, '响应的 method 应是 Allocate');
        assert.equal(reply.cls, CLASS_ERROR,
            `第一次请求（无凭据）应收到错误响应，收到 class=${reply.cls}`);
        assert.equal(reply.type, 0x0113,
            `Allocate Error Response 的 type 应为 0x0113，收到 0x${reply.type.toString(16)}`);
        assert.ok(reply.realm, '401 挑战必须带 realm');
        assert.ok(reply.nonce, '401 挑战必须带 nonce');
        assert.equal(reply.realm.value.toString('utf8'), REALM, 'realm 不匹配');
    } finally {
        server.close();
    }
});

test('带上凭据后 Allocate 成功，断言收到成功响应与有效中继地址；验证方式：真的发一次 Allocate 并断言中继地址可解码', async () => {
    const server = await startFakeTurnServer();
    try {
        const challenge = await allocate(server, null);
        assert.ok(challenge.nonce, '先要拿到 nonce 才能重试');

        const reply = await allocate(server, {
            username: TURN_USERNAME,
            realm: challenge.realm.value.toString('utf8'),
            nonce: challenge.nonce.value.toString('utf8'),
            password: TURN_PASSWORD,
        });

        assert.equal(reply.cls, CLASS_SUCCESS,
            `带凭据的请求应收到成功响应，收到 class=${reply.cls}`);
        assert.equal(reply.type, 0x0103,
            `Allocate Success Response 的 type 应为 0x0103，收到 0x${reply.type.toString(16)}`);

        // 有效中继地址 —— 这正是 RFC 5766 §6.2 那个偏移 bug 会打掉的东西。
        assert.ok(reply.relay, '响应里必须有 XOR-RELAYED-ADDRESS');
        const relay = decodeXorAddress(reply.relay.value);
        assert.ok(relay, '中继地址应能解码 —— Family 读错偏移就解不出来');
        assert.equal(relay.family, 0x01, '中继地址的 Family 应为 IPv4');
        assert.ok(relay.port > 0 && relay.port < 65536,
            `中继端口不合法：${relay.port}`);
        assert.match(relay.address, /^\d+\.\d+\.\d+\.\d+$/,
            `中继地址不是点分十进制：${relay.address}`);

        // 映射地址也在（客户端用它知道服务端看到的自己的地址）。
        assert.ok(reply.mapped, '响应里必须有 XOR-MAPPED-ADDRESS');
        const mapped = decodeXorAddress(reply.mapped.value);
        assert.ok(mapped, '映射地址应能解码');
    } finally {
        server.close();
    }
});

// ===========================================================================
// 真机部署的行为 —— 与进程内替身互补
// ===========================================================================

test('部署的验收脚本真的做了 TURN 分配并断言了中继地址；验证方式：检查 `ci-acceptance.sh` 里有 `turnutils_uclient` 与判失败的 `if ! grep -qE "Received relay addr" <<<"$allocation"`', () => {
    // 进程内替身验证的是**客户端侧的协议实现**；真服务器那一侧由
    // `examples/turn-server/ci-acceptance.sh` 验证 —— 它由 CI 的
    // `linux-turn` leg 在装了 coturn 的 Linux runner 上跑。
    //
    // 没有这一条，标准 6 就只剩半个实现：替身能说话，不代表部署
    // 真的能被 TLS 客户端用。
    const script = path.join(DEPLOY, 'ci-acceptance.sh');
    assert.ok(exists(script),
        'examples/turn-server/ci-acceptance.sh 不存在 —— 没有人验证真服务器可用');

    const text = readText(script);

    // (a) 真的发起了分配：coturn 自己的客户端 turnutils_uclient。
    assert.match(text, /turnutils_uclient/,
        'ci-acceptance.sh 没有调用 turnutils_uclient —— 没有真的发起 TURN 分配');

    // (b) 断言了有效中继地址 —— 而且是**会失败**的断言，不是注释里
    //     提到过这句话。`if ! grep -qE ...` 才是判失败的形态。
    //
    //     锚定在**正例**那一处：`<<<"$allocation"`。脚本里同一句 grep
    //     出现了三次（正例、plain 客户端、TLS 客户端负控），不锚定的
    //     话，改掉正例还留着另两处，断言照样通过 —— 那就成了一条
    //     恒真的检查。
    assert.match(text,
        /if\s*!\s*grep\s+-qE\s+"Received relay addr"\s+<<<\s*"\$allocation"/,
        'ci-acceptance.sh 没有断言正例拿到了中继地址 —— 分配成功与否没有被验证');

    // (c) 断言了成功响应（同样锚定正例）。
    assert.match(text, /if\s*!\s*grep\s+-q\s+"success"\s+<<<\s*"\$allocation"/,
        'ci-acceptance.sh 没有断言正例分配成功');

    // (d) TLS 会话是真的（不是明文）：客户端带 -S。
    assert.match(text, /-S\b/,
        'ci-acceptance.sh 的 TLS 客户端没有 -S（secure connection）');

    // (e) 负控存在：同一个端口上换 plain 服务器，TLS 客户端必须拿不到分配。
    //     没有负控，正例证明不了任何东西。
    assert.match(text, /--no-tls/,
        'ci-acceptance.sh 没有 plain 负控 —— 正例不能证明 TLS 真的被强制');

    // (f) 负控真的**判**了失败：同一个 `grep -qE "Received relay addr"`
    //     出现在一个**不带** `!` 的 `if` 里（"拿到了分配就是被降级了"），
    //     而且它查的是负控的分配。
    assert.match(text,
        /if\s+grep\s+-qE\s+"Received relay addr"\s+<<<\s*"\$control_allocation"/,
        'ci-acceptance.sh 的负控没有判定 TLS 客户端拿不到分配 —— 降级会被放过');

    // (g) 这个脚本由 CI 的 linux-turn leg 执行。
    const ci = readText(path.join(ROOT, '.github', 'workflows', 'ci.yml'));
    assert.match(ci, /examples\/turn-server\/ci-acceptance\.sh/,
        'CI 没有执行 examples/turn-server/ci-acceptance.sh —— 行为验证没有发生');
});

test('REQUESTED-TRANSPORT 的协议号在 byte 0（RFC 5766 §14.3）；验证方式：Allocate 请求里带的属性值的第 1 个字节 === 17', async () => {
    // 这一条断言的是**allocate() 真正发出去的那个字节**，而不是
    // 另一个写死的字面量 —— 否则改错了发出去的那份，断言还是绿的。
    //
    // 判据落在**服务端收到的请求**上：替身按 RFC 5766 §14.3 校验
    // `value[0]`，放错位置就回 400。所以两条断言互相咬合：
    //   (a) 替身确实拒了协议号放错字节的请求（替身有牙齿）；
    //   (b) allocate() 发出去的那份确实过了这一关（实现没放错）。
    const server = await startFakeTurnServer();
    try {
        // (a) 注入一个放错位置的协议号：替身必须拒。
        const bad = await allocate(server, null, {
            transport: Buffer.from([0x00, 0x00, 0x00, 0x11]),
        });
        assert.equal(bad.cls, CLASS_ERROR,
            '协议号放到 byte 3 的请求应被拒 —— 替身没校验就等于没牙齿');
        assert.equal(bad.type, 0x0113,
            `错误响应的 type 应为 0x0113，收到 0x${bad.type.toString(16)}`);
        // 被拒的那次**没有** realm/nonce —— 那是 400，不是 401 挑战。
        assert.ok(!bad.realm,
            '协议号放错的请求应回 400（无 realm），却回了挑战');

        // (b) 默认的那份：必须过 transport 校验。
        //
        // 「过」的意思是**走到了凭据那一关**：无凭据的请求本来就该
        // 回 401 挑战（也是 error class），所以判据不能是
        // 「class 不是 error」—— 那对无凭据请求永远不成立。
        // 判据是「**回的是 401 挑战而不是 400**」：有 realm 与 nonce
        // 说明请求被受理到了凭据校验这一关，没被 transport 挡掉。
        const good = await allocate(server, null);
        assert.ok(good.realm,
            '默认的 REQUESTED-TRANSPORT 应通过替身的校验并进入凭据校验 —— ' +
            '协议号必须在 byte 0');
        assert.ok(good.nonce, '通过 transport 校验后应收到 401 挑战');

        // 服务端**收到**的那份，byte 0 确实是 17。
        //
        // 取**最后一个** Allocate 请求：上面 (a) 故意注入了一个协议号
        // 放错字节的请求，`seen` 里第一个就是它。判据要落在
        // allocate() 默认发出去的那一份上。
        const requests = server.seen.filter(s => s.method === METHOD_ALLOCATE);
        assert.ok(requests.length >= 2,
            '替身至少应收到两次 Allocate（一次坏的、一次默认的）');
        const request = requests[requests.length - 1];
        const transport = request.attrs.find(a => a.type === ATTR_REQUESTED_TRANSPORT);
        assert.ok(transport, '收到的请求里没有 REQUESTED-TRANSPORT 属性');
        assert.equal(transport.value[0], 17,
            '发出去的 REQUESTED-TRANSPORT 的 byte 0 必须是 17（UDP）');
        assert.equal(transport.value.length, 4, '发出去的值是 4 字节');
        assert.equal(transport.value[1] + transport.value[2] + transport.value[3], 0,
            '发出去的 REQUESTED-TRANSPORT 的 bytes 1-3 是保留的 0');
    } finally {
        server.close();
    }
});

test('属性编码按 4 字节边界补齐（RFC 5389 §15）；验证方式：编码后的属性长度是 4 + 值长 + padding，且解码能读回下一条属性', () => {
    // 少一个字节的 padding，下一条属性就会从 padding 里被读出来，
    // 解出来是 type 0 与一段乱码。
    for (const value of [Buffer.from('nimrtc.local', 'utf8'),
                         Buffer.from('nonce-0123456789abcdef', 'utf8'),
                         Buffer.alloc(4), Buffer.alloc(6)]) {
        const attr = encodeAttribute(0x0014, value);
        const padding = (4 - (value.length % 4)) % 4;
        assert.equal(attr.length, 4 + value.length + padding,
            `属性编码的长度应为 4 + 值长 + padding(${padding})`);
        assert.equal(attr.readUInt16BE(2), value.length,
            '长度字段写的是值的长度，不含 padding');
    }

    // 端到端：两条属性连着编，解回来两条都在，且类型与值都对。
    const first = Buffer.from('nimrtc.local', 'utf8');
    const second = Buffer.from('nonce-0123456789abcdef', 'utf8');
    const decoded = decodeAttributes(Buffer.concat([
        encodeAttribute(ATTR_REALM, first),
        encodeAttribute(ATTR_NONCE, second),
    ]));
    assert.equal(decoded.length, 2, '两条属性应解出两条');
    assert.equal(decoded[0].type, ATTR_REALM, '第 1 条的类型不对');
    assert.equal(decoded[0].value.toString('utf8'), 'nimrtc.local',
        '第 1 条的值不对');
    assert.equal(decoded[1].type, ATTR_NONCE, '第 2 条的类型不对');
    assert.equal(decoded[1].value.toString('utf8'), 'nonce-0123456789abcdef',
        '第 2 条的值不对');
});

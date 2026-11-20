const tls = require('node:tls');
const crypto = require('node:crypto');
const MAGIC = 0x2112A442;
const ATTR = { REQUESTED_TRANSPORT: 0x0019, USERNAME: 0x0006, REALM: 0x0014, NONCE: 0x0015, MESSAGE_INTEGRITY: 0x0008, ERROR_CODE: 0x0009 };
function align4(n) { return (n + 3) & ~3; }
function stunType(method, cls) {
  return ((method & 0xf80) << 2) | ((method & 0x070) << 1) | (method & 0x00f) | ((cls & 0x2) << 7) | ((cls & 0x1) << 4);
}
function build({ username, realm, nonce, password }) {
  const hasIntegrity = Boolean(realm && nonce && password != null);
  const attrs = [
    { type: ATTR.REQUESTED_TRANSPORT, value: Buffer.from([0, 0, 0, 0x11]) },
    { type: ATTR.USERNAME, value: Buffer.from(username, 'utf8') },
  ];
  if (realm) attrs.push({ type: ATTR.REALM, value: Buffer.from(realm, 'utf8') });
  if (nonce) attrs.push({ type: ATTR.NONCE, value: Buffer.from(nonce, 'utf8') });
  let attrLen = 0;
  for (const a of attrs) attrLen += 4 + align4(a.value.length);
  const totalLen = hasIntegrity ? attrLen + 24 : attrLen;
  const msg = Buffer.alloc(20 + totalLen);
  msg.writeUInt16BE(stunType(0x003, 0), 0);
  msg.writeUInt16BE(totalLen, 2);
  msg.writeUInt32BE(MAGIC, 4);
  crypto.randomBytes(12).copy(msg, 8);
  let p = 20;
  for (const a of attrs) {
    msg.writeUInt16BE(a.type, p);
    msg.writeUInt16BE(a.value.length, p + 2);
    a.value.copy(msg, p + 4);
    p += 4 + align4(a.value.length);
  }
  if (hasIntegrity) {
    const key = crypto.createHash('md5').update(`${username}:${realm}:${password}`, 'utf8').digest();
    const hmac = crypto.createHmac('sha1', key).update(msg.subarray(0, p)).digest();
    msg.writeUInt16BE(ATTR.MESSAGE_INTEGRITY, p);
    msg.writeUInt16BE(20, p + 2);
    hmac.copy(msg, p + 4);
    p += 24;
  }
  return msg.subarray(0, p);
}
function parseAttrs(msg) {
  const end = 20 + msg.readUInt16BE(2);
  const out = [];
  let p = 20;
  while (p + 4 <= end) {
    const t = msg.readUInt16BE(p);
    const l = msg.readUInt16BE(p + 2);
    out.push({ type: t, value: msg.subarray(p + 4, p + 4 + l) });
    p += 4 + align4(l);
  }
  return out;
}
const sock = tls.connect({ host: '127.0.0.1', port: 5349, servername: 'turn.nimrtc.local', rejectUnauthorized: false });
let buf = Buffer.alloc(0);
sock.on('data', (d) => {
  buf = Buffer.concat([buf, d]);
  while (buf.length >= 20) {
    const len = buf.readUInt16BE(2);
    if (buf.length < 20 + len) break;
    const msg = buf.subarray(0, 20 + len);
    buf = buf.subarray(20 + len);
    const type = msg.readUInt16BE(0);
    const attrs = parseAttrs(msg);
    const ec = attrs.find((a) => a.type === ATTR.ERROR_CODE);
    console.log('>>> type=0x' + type.toString(16), ec ? `error=${((ec.value[2] & 0x07) * 100) + ec.value[3]} reason="${ec.value.subarray(4).toString('utf8')}"` : '');
    const realm = attrs.find((a) => a.type === ATTR.REALM);
    const nonce = attrs.find((a) => a.type === ATTR.NONCE);
    if (realm && nonce) {
      sock.write(build({ username: process.env.NIMRTC_TURN_USERNAME, realm: realm.value.toString('utf8'), nonce: nonce.value.toString('utf8'), password: process.env.NIMRTC_TURN_PASSWORD }));
    }
  }
});
sock.on('error', (e) => console.log('ERR', e.message));
setTimeout(() => { sock.write(build({ username: process.env.NIMRTC_TURN_USERNAME })); }, 300);
setTimeout(() => { sock.end(); process.exit(0); }, 6000);

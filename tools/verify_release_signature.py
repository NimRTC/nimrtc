#!/usr/bin/env python3
"""Verify NimRTC release artefacts: detached GPG signatures + SHA-256 digests.

This is the offline counterpart of `gpg --verify`: it re-implements the
RFC 4880 §5.2.4 signature construction and RFC 8032 Ed25519 verification
in pure Python, so a release can be checked on a machine that has no
GnuPG installed — or, more importantly, so a *second, independent*
implementation can confirm what GnuPG says.

Why a second implementation matters
-----------------------------------
A release that is only ever checked by the same tool that produced it
proves nothing about interoperability.  Two plausible failure modes are
worth catching independently:

  * a non-canonical MPI length in the public-key packet (accepted by
    some OpenPGP implementations, rejected by others);
  * a signature whose hash input omits the v4 signature prefix or uses
    the wrong trailer length (RFC 4880 §5.2.4).

Both are silent in GnuPG's default output but fatal for a stricter
parser, and they are exactly the kind of defect that stays invisible
until a downstream consumer tries to verify the release.

What it checks
--------------
For every `<artefact>.asc` next to `<artefact>`:

  1. the detached signature is cryptographically valid for the artefact
     (Ed25519 over the RFC 4880 hash of the document), and
  2. the signing key's UID self-signature (packet type 0x13) is valid —
     a key whose self-signature does not verify imports with an
     "unusable" user ID in several implementations, even though the
     detached signatures still verify.

Additionally, if a `<artefact>.sha256` sidecar exists, its digest is
compared against the artefact.

Usage
-----
    python tools/verify_release_signature.py <dir> [...]
    python tools/verify_release_signature.py --key KEY.asc <dir>
    python tools/verify_release_signature.py --self-test

Each directory is scanned for `*.asc` detached signatures; the artefact
is the same path with the `.asc` suffix removed.  Exit code is non-zero
if anything fails to verify.

`--self-test` runs the RFC 8032 §7.1 Ed25519 test vectors through the
implementation in this file.  It needs no network and no artefacts, and
it is the guard against the failure mode that actually bit this tool: a
wrong sign in the twisted-Edwards curve equation, or an inverted
verification equation (`[S]B = R + [k]A` checked as `R == [S]B + [k]A`),
makes every *valid* signature look invalid -- and a verifier that
rejects good signatures is worse than no verifier at all.

No third-party modules are required: Ed25519 is implemented here.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# RFC 8032 Ed25519 (pure Python)
# ---------------------------------------------------------------------------

_P = 2 ** 255 - 19
_Q = 2 ** 252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_I = pow(2, (_P - 1) // 4, _P)
_BY = 4 * pow(5, _P - 2, _P) % _P


def _recover_x(y: int, sign: int):
    if y >= _P:
        return None
    xx = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P)
    x = pow(xx, (_P + 3) // 8, _P)
    if (x * x - xx) % _P != 0:
        x = (x * _I) % _P
    if x % 2 != sign:
        x = _P - x
    return x


def _ed_add(p: tuple, q: tuple) -> tuple:
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = (y1 - x1) * (y2 - x2) % _P
    b = (y1 + x1) * (y2 + x2) % _P
    c = t1 * 2 * _D * t2 % _P
    d = z1 * 2 * z2 % _P
    e = b - a
    f = d - c
    g = d + c
    h = b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _ed_mul(s: int, p: tuple) -> tuple:
    q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            q = _ed_add(q, p)
        p = _ed_add(p, p)
        s >>= 1
    return q


_BX = _recover_x(_BY, 0)
_B = (_BX % _P, _BY % _P, 1, _BX * _BY % _P)


def _decode_point(data: bytes):
    """Decode a 32-byte Ed25519 point (RFC 8032 §5.1.3)."""
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    if y >= _P:
        return None
    x = _recover_x(y, sign)
    if x is None:
        return None
    point = (x, y, 1, x * y % _P)
    if not _is_on_curve(point):
        return None
    return point


def _is_on_curve(point: tuple) -> bool:
    """Check the extended-coordinate point satisfies the curve equation.

    RFC 8032 §5.1 writes the curve as `-x^2 + y^2 = 1 + d*x^2*y^2`.  In
    extended coordinates (X:Y:Z:T) with `T = X*Y/Z` that becomes
    `-X^2 + Y^2 - Z^2 - d*T^2 = 0`, which is the form the RFC's own
    `isoncurve()` uses.
    """
    x, y, z, t = point
    return (-x * x + y * y - z * z - _D * t * t) % _P == 0


def _points_equal(p: tuple, q: tuple) -> bool:
    """Compare two extended-coordinate points projectively."""
    x1, y1, z1, _ = p
    x2, y2, z2, _ = q
    if (x1 * z2 - x2 * z1) % _P != 0:
        return False
    return (y1 * z2 - y2 * z1) % _P == 0


def ed25519_verify(public_key: bytes, message: bytes, signature: bytes) -> bool:
    """Return True iff `signature` is a valid Ed25519 signature.

    Implements the RFC 8032 §5.1.7 verification equation
    `[S]B = R + [k]A`, checked in projective coordinates: the right-hand
    side `R + [k]A` must equal `[S]B` (not the other way round -- adding
    `[k]A` to `R` and comparing against `R` verifies nothing).
    """
    if len(public_key) != 32 or len(signature) != 64:
        return False
    a = _decode_point(public_key)
    if a is None:
        return False
    r_bytes = signature[:32]
    s = int.from_bytes(signature[32:], "little")
    if s >= _Q:
        return False
    r = _decode_point(r_bytes)
    if r is None:
        return False
    h = int.from_bytes(
        hashlib.sha512(r_bytes + public_key + message).digest(), "little") % _Q
    # R + [h]A must equal [S]B.
    check = _ed_add(r, _ed_mul(h, a))
    return _points_equal(check, _ed_mul(s, _B))


# ---------------------------------------------------------------------------
# OpenPGP packet parsing (RFC 4880 §4 / §5.2)
# ---------------------------------------------------------------------------

def dearmor(text: str) -> bytes:
    """Decode an ASCII-armored block into raw OpenPGP bytes."""
    payload: list[str] = []
    in_block = False
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("-----BEGIN PGP"):
            in_block = True
            continue
        if line.startswith("-----END PGP"):
            break
        if not in_block or not line:
            continue
        if ":" in line and not line.startswith("="):
            # Armor header ("Comment: ..."), not payload.
            continue
        if line.startswith("="):
            # CRC-24 checksum line.
            continue
        payload.append(line)
    return base64.b64decode("".join(payload))


def read_packets(data: bytes) -> list:
    """Split an OpenPGP stream into (tag, body) pairs."""
    packets = []
    pos = 0
    while pos < len(data):
        header = data[pos]
        pos += 1
        if not header & 0x80:
            raise ValueError(f"bad packet header at offset {pos - 1}")
        if header & 0x40:  # new-format
            tag = header & 0x3F
            first = data[pos]
            pos += 1
            if first < 192:
                length = first
            elif first < 224:
                length = ((first - 192) << 8) + data[pos] + 192
                pos += 1
            elif first == 255:
                length = struct.unpack(">I", data[pos:pos + 4])[0]
                pos += 4
            else:
                raise ValueError("partial lengths are not supported")
        else:  # old-format
            tag = (header >> 2) & 0x0F
            length_type = header & 0x03
            if length_type == 0:
                length = data[pos]
                pos += 1
            elif length_type == 1:
                length = struct.unpack(">H", data[pos:pos + 2])[0]
                pos += 2
            elif length_type == 2:
                length = struct.unpack(">I", data[pos:pos + 4])[0]
                pos += 4
            else:
                length = len(data) - pos
        packets.append((tag, data[pos:pos + length]))
        pos += length
    return packets


def parse_mpis(blob: bytes) -> list:
    """Split an MPI sequence into the raw (big-endian, unpadded) values."""
    values = []
    pos = 0
    while pos + 2 <= len(blob):
        bits = struct.unpack(">H", blob[pos:pos + 2])[0]
        pos += 2
        nbytes = (bits + 7) // 8
        values.append(blob[pos:pos + nbytes])
        pos += nbytes
    return values


def parse_signature(body: bytes) -> dict:
    """Parse a v4 signature packet body."""
    version, sig_type = body[0], body[1]
    pub_algo, hash_algo = body[2], body[3]
    hashed_len = struct.unpack(">H", body[4:6])[0]
    hashed_area = body[4:6 + hashed_len]
    unhashed_len = struct.unpack(">H", body[6 + hashed_len:8 + hashed_len])[0]
    unhashed_area = body[8 + hashed_len:8 + hashed_len + unhashed_len]
    rest = body[8 + hashed_len + unhashed_len:]
    return {
        "version": version,
        "sig_type": sig_type,
        "pub_algo": pub_algo,
        "hash_algo": hash_algo,
        "hashed_area": hashed_area,
        "hashed_len": hashed_len,
        "unhashed_area": unhashed_area,
        "left16": rest[:2],
        "mpi_blob": rest[2:],
    }


def signature_prefix(sig: dict) -> bytes:
    """The v4 signature prefix: version, type, algos and hashed subpackets."""
    return bytes([sig["version"], sig["sig_type"], sig["pub_algo"],
                  sig["hash_algo"]]) + sig["hashed_area"]


def hash_input(sig: dict, body_data: bytes) -> bytes:
    """The exact byte string an OpenPGP v4 signature is computed over."""
    prefix = signature_prefix(sig)
    # RFC 4880 §5.2.4: the trailer's length counts the signature prefix
    # (not merely the hashed subpacket area).
    return body_data + prefix + b"\x04\xff" + struct.pack(">I", len(prefix))


_HASHES = {
    7: hashlib.sha1,
    8: hashlib.sha256,
    9: hashlib.sha384,
    10: hashlib.sha512,
    11: hashlib.sha224,
}


def key_point(key_body: bytes) -> bytes:
    """Extract the Ed25519 public point from a v4 public-key packet body.

    The point is carried as an MPI whose declared bit length is *not*
    necessarily 256: GnuPG writes the native-point marker 0x40 in front of
    the 32-byte key, which yields a 263-bit MPI.  Slicing at a fixed
    offset therefore truncates the key; parse the MPI instead.
    """
    if key_body[0] != 4:
        raise ValueError(f"unsupported public-key version {key_body[0]}")
    algo = key_body[5]
    if algo != 22:
        raise ValueError(
            f"unsupported public-key algorithm {algo} (want 22/Ed25519)")
    bitlen = struct.unpack(">H", key_body[6:8])[0]
    nbytes = (bitlen + 7) // 8
    point = key_body[8:8 + nbytes]
    if len(point) < 32:
        raise ValueError(f"public key too short ({len(point)} bytes)")
    return point[-32:]


def key_fingerprint(key_body: bytes) -> str:
    """RFC 4880 §12.2 fingerprint of a v4 public-key packet."""
    if key_body[0] != 4:
        raise ValueError(f"unsupported public-key version {key_body[0]}")
    prefixed = b"\x99" + struct.pack(">H", len(key_body)) + key_body
    return hashlib.sha1(prefixed).hexdigest().upper()


def verify_signature(sig: dict, body_data: bytes, point: bytes):
    """Verify a parsed signature over `body_data` with the Ed25519 `point`."""
    hash_fn = _HASHES.get(sig["hash_algo"])
    if hash_fn is None:
        return False, f"unsupported hash algorithm {sig['hash_algo']}"
    if sig["pub_algo"] != 22:
        return False, f"unsupported public-key algorithm {sig['pub_algo']}"

    data = hash_input(sig, body_data)
    digest = hash_fn(data).digest()
    if digest[:2] != sig["left16"]:
        return False, "left-16 digest prefix mismatch"

    mpis = parse_mpis(sig["mpi_blob"])
    if len(mpis) < 2:
        return False, "signature does not carry two MPIs"
    raw = b"".join(m.ljust(32, b"\x00") for m in mpis[:2])

    # GnuPG's EdDSA (public-key algorithm 22) signs the *hash digest*
    # rather than the raw hash input; RFC 4880bis' algorithm 27 signs the
    # raw input.  Try the digest first, then the raw input, so both
    # conventions verify.
    if ed25519_verify(point, digest, raw):
        return True, "Ed25519 valid over the SHA-512 digest"
    if ed25519_verify(point, data, raw):
        return True, "Ed25519 valid over the raw hash input"
    return False, "Ed25519 signature does not verify"


def verify_self_signature(key_path: Path):
    """Verify the UID self-signature of a public-key block.

    Returns (ok, fingerprint, detail).
    """
    packets = read_packets(dearmor(key_path.read_text(encoding="utf-8")))
    key_bodies = [b for t, b in packets if t == 6]
    uid_bodies = [b for t, b in packets if t == 13]
    sig_bodies = [b for t, b in packets if t == 2]
    if not key_bodies:
        return False, "", "no public-key packet"
    if not uid_bodies:
        return False, "", "no user-ID packet"
    if not sig_bodies:
        return False, "", "no self-signature packet"

    key_body = key_bodies[0]
    fingerprint = key_fingerprint(key_body)
    point = key_point(key_body)

    # The self-signature is the type-0x13 certification over the first UID.
    for sig_body in sig_bodies:
        sig = parse_signature(sig_body)
        if sig["sig_type"] != 0x13:
            continue
        key_frame = b"\x99" + struct.pack(">H", len(key_body)) + key_body
        uid_frame = (b"\xb4" + struct.pack(">I", len(uid_bodies[0])) +
                     uid_bodies[0])
        return (verify_signature(sig, key_frame + uid_frame, point) +
                (fingerprint,))
    return False, fingerprint, "no type-0x13 self-signature found"


def verify_detached(sig_path: Path, data_path: Path, key_path: Path):
    """Verify a detached signature over `data_path` using `key_path`."""
    sig_packets = read_packets(dearmor(sig_path.read_text(encoding="utf-8")))
    sig_bodies = [b for t, b in sig_packets if t == 2]
    if not sig_bodies:
        return False, "no signature packet"

    key_packets = read_packets(dearmor(key_path.read_text(encoding="utf-8")))
    key_bodies = [b for t, b in key_packets if t == 6]
    if not key_bodies:
        return False, "no public-key packet"

    point = key_point(key_bodies[0])
    sig = parse_signature(sig_bodies[0])
    return verify_signature(sig, data_path.read_bytes(), point)


def verify_sha256_sidecar(artefact: Path):
    """Compare an artefact against its `.sha256` sidecar."""
    sidecar = artefact.with_name(artefact.name + ".sha256")
    if not sidecar.exists():
        return True, "no sha256 sidecar"
    expected = sidecar.read_text(encoding="utf-8").split()[0].strip().lower()
    actual = hashlib.sha256(artefact.read_bytes()).hexdigest()
    if expected != actual:
        return False, f"sha256 mismatch (sidecar {expected}, actual {actual})"
    return True, "sha256 matches"


def find_key(directory: Path, explicit):
    """Locate the public key to verify against."""
    if explicit is not None:
        return explicit
    for name in ("nimrtc-signing-key.asc", "signing-key.asc"):
        candidate = directory / name
        if candidate.exists():
            return candidate
    return None


def _rfc8032_self_test() -> int:
    """Run the RFC 8032 §7.1 Ed25519 test vectors through `ed25519_verify`."""
    vectors = [
        # (public key, message, signature)
        (
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
            "",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
            "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        ),
        (
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
            "72",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
        ),
        (
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
            "af82",
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
            "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
        ),
    ]
    failures = 0
    for index, (public_key, message, signature) in enumerate(vectors, 1):
        ok = ed25519_verify(
            bytes.fromhex(public_key),
            bytes.fromhex(message),
            bytes.fromhex(signature),
        )
        print(f"[{'OK  ' if ok else 'FAIL'}] RFC 8032 §7.1 test vector {index}")
        failures += 0 if ok else 1

    # A tampered signature must NOT verify.
    tampered = bytearray(bytes.fromhex(vectors[0][2]))
    tampered[0] ^= 0x01
    ok = not ed25519_verify(
        bytes.fromhex(vectors[0][0]),
        bytes.fromhex(vectors[0][1]),
        bytes(tampered),
    )
    print(f"[{'OK  ' if ok else 'FAIL'}] tampered signature is rejected")
    failures += 0 if ok else 1
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify NimRTC release artefacts (GPG detached signatures "
                    "+ SHA-256 sidecars) without requiring GnuPG.")
    parser.add_argument("directories", nargs="*", type=Path,
                        help="directories containing <artefact>[.asc|.sha256]")
    parser.add_argument("--key", type=Path, default=None,
                        help="public key block (default: nimrtc-signing-key.asc "
                             "in each scanned directory)")
    parser.add_argument("--self-test", action="store_true",
                        help="run the RFC 8032 Ed25519 test vectors and exit")
    args = parser.parse_args()

    if args.self_test:
        failures = _rfc8032_self_test()
        print()
        print(f"self-test: {failures} failure(s)")
        return 0 if failures == 0 else 1

    if not args.directories:
        parser.error("at least one directory is required (or use --self-test)")

    failures = 0
    checked = 0

    for directory in args.directories:
        if not directory.is_dir():
            print(f"[FAIL] {directory}: not a directory")
            failures += 1
            continue

        key_path = find_key(directory, args.key)
        if key_path is None:
            print(f"[FAIL] {directory}: no public key found "
                  f"(expected nimrtc-signing-key.asc, or pass --key)")
            failures += 1
            continue

        ok, fingerprint, detail = verify_self_signature(key_path)
        status = "OK  " if ok else "FAIL"
        print(f"[{status}] self-signature {key_path.name}: {detail}")
        print(f"        fingerprint {fingerprint}")
        if not ok:
            failures += 1

        for sig_path in sorted(directory.glob("*.asc")):
            if sig_path == key_path:
                continue
            artefact = Path(str(sig_path)[: -len(".asc")])
            if not artefact.exists():
                continue
            ok, detail = verify_detached(sig_path, artefact, key_path)
            checked += 1
            if not ok:
                failures += 1
            print(f"[{'OK  ' if ok else 'FAIL'}] {artefact.name}: {detail}")

            sha_ok, sha_detail = verify_sha256_sidecar(artefact)
            if not sha_ok:
                failures += 1
            print(f"        {sha_detail}")

    print()
    print(f"checked {checked} artefact signature(s); {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

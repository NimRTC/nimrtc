#!/usr/bin/env python3
"""Manage the fuzz corpus seeds.

The corpus is the set of inputs the fuzz targets are replayed against on
every commit (see `fuzz_replay.cpp`) and the set libFuzzer starts from
in CI.  A corpus seed is a byte blob, but **binary blobs are not
checked into this repository** — so the seeds are stored as *text*.

Two modes:

  * **Generate** (default) — rewrite the checked-in `*.hex` seed files
    from the readable descriptions below.  The hex files are the
    source of truth in git; this script is how they were produced and
    how they stay reproducible.

        python3 tests/fuzz/tools/make_corpus.py

  * **Materialize** — turn the checked-in hex seeds back into the raw
    byte files libFuzzer expects, into a directory of your choosing
    (a build directory, never the source tree):

        python3 tests/fuzz/tools/make_corpus.py --materialize build/fuzz-corpus

    The result has the layout libFuzzer wants:

        build/fuzz-corpus/rtp/simple.bin
        build/fuzz-corpus/sdp/offer.sdp
        build/fuzz-corpus/video_payload/h264_single.bin

    The `.sdp` seeds are already text and are copied verbatim; the
    `.hex` seeds are decoded into `.bin`.

The output is deterministic: the same script always produces the same
bytes, so regenerating the corpus does not churn the diff.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import struct

ROOT = pathlib.Path(__file__).resolve().parents[3]
CORPUS = ROOT / "tests" / "fuzz" / "corpus"

SSRC = 0xDEADBEEF


# ---------------------------------------------------------------------------
# Seed generators
# ---------------------------------------------------------------------------

def rtp(payload: bytes, seq: int = 1, ts: int = 90000, pt: int = 96,
        marker: bool = True, csrc: list[int] | None = None) -> bytes:
    """Build a well-formed RTP packet (RFC 3550 §5.1)."""
    csrc = csrc or []
    b0 = 0x80 | (len(csrc) & 0x0F)
    b1 = (0x80 if marker else 0x00) | (pt & 0x7F)
    out = bytearray([b0, b1])
    out += struct.pack(">H", seq & 0xFFFF)
    out += struct.pack(">I", ts & 0xFFFFFFFF)
    out += struct.pack(">I", SSRC)
    for c in csrc:
        out += struct.pack(">I", c & 0xFFFFFFFF)
    out += payload
    return bytes(out)


def rtp_with_ext(payload: bytes, ext_type: int = 0xBEDE,
                 ext_body: bytes = b"\x00\x00\x00\x00") -> bytes:
    """RTP packet carrying one header extension (RFC 5285)."""
    body = struct.pack(">H", ext_type) + ext_body
    # The extension length counts 32-bit words, not bytes.
    padded = body + b"\x00" * ((4 - len(body) % 4) % 4)
    out = bytearray([0x90, 0x80 | 96])  # X=1, M=1
    out += struct.pack(">H", 1)
    out += struct.pack(">I", 90000)
    out += struct.pack(">I", SSRC)
    out += struct.pack(">H", len(padded) // 4)
    out += padded
    out += payload
    return bytes(out)


def rtcp_sr() -> bytes:
    """Sender Report (RFC 3550 §6.4.1)."""
    body = struct.pack(">II", 0xDEADBEEF, 0xCAFEBABE)
    body += struct.pack(">I", 0xF1E2D3C4)   # NTP timestamp
    body += struct.pack(">I", 90000)        # RTP timestamp
    body += struct.pack(">I", 0)           # packet count
    body += struct.pack(">I", 0)           # octet count
    # Header: V=2, RC=0, PT=200, length in 32-bit words minus one.
    return struct.pack(">BBH", 0x80, 200, (len(body) // 4)) + body


def rtcp_rr() -> bytes:
    """Receiver Report (RFC 3550 §6.4.2) with one report block."""
    body = struct.pack(">I", SSRC)
    body += struct.pack(">I", 0xDEADBEEF)  # report block SSRC
    body += struct.pack(">I", 0xF1E2D3C4)  # fraction lost / total lost
    body += struct.pack(">I", 0xFFFFFFFF)  # extended highest seq
    body += struct.pack(">I", 0)           # jitter
    body += struct.pack(">I", 0)           # LSR
    body += struct.pack(">I", 0)           # DLSR
    return struct.pack(">BBH", 0x80, 201, (len(body) // 4)) + body


def rtcp_nack() -> bytes:
    """Generic NACK (RFC 4585 §6.2.1) for two lost packets."""
    body = struct.pack(">I", SSRC)
    body += struct.pack(">I", 0xDEADBEEF)  # media source
    body += struct.pack(">H", 100)          # PID
    body += struct.pack(">H", 0xB000)       # BLP: two more lost
    return struct.pack(">BBH", 0x81, 206, (len(body) // 4)) + body


def rtcp_compound() -> bytes:
    """SR + SDES compound datagram (RFC 3550 §6.1)."""
    sdes_body = struct.pack(">I", SSRC)
    sdes_body += b"\x01"        # CNAME item type
    sdes_body += b"\x03"        # length
    sdes_body += b"seed"        # value
    sdes_body += b"\x00"        # null terminator
    sdes = struct.pack(">BBH", 0x81, 202, (len(sdes_body) // 4)) + sdes_body
    return rtcp_sr() + sdes


def h264_single(nalu_type: int = 5, body: bytes = b"\x00" * 16) -> bytes:
    """Single NAL unit packet (RFC 6184 §5.2)."""
    return bytes([(0x60 & 0xE0) | nalu_type]) + body


def h264_stap_a() -> bytes:
    """STAP-A aggregation of two NAL units (RFC 6184 §5.7.1)."""
    nalu_a = bytes([0x67]) + b"\x11" * 4   # SPS
    nalu_b = bytes([0x68]) + b"\x22" * 4   # PPS
    out = bytes([0x78])                    # STAP-A, NRI=3
    out += struct.pack(">H", len(nalu_a)) + nalu_a
    out += struct.pack(">H", len(nalu_b)) + nalu_b
    return out


def h264_fu_a(start: bool, end: bool, nalu_type: int = 5,
              body: bytes = b"\x33" * 8) -> bytes:
    """One FU-A fragment (RFC 6184 §5.8)."""
    indicator = 0x60 | 0x1C | 28           # F=0, NRI=3, type=28
    fu_header = nalu_type & 0x1F
    if start:
        fu_header |= 0x80
    if end:
        fu_header |= 0x40
    return bytes([indicator, fu_header]) + body


def vp8_descriptor() -> bytes:
    """VP8 payload with a keyframe descriptor (RFC 7741 §4.2)."""
    # X=0, R=0, N=0, S=1 (keyframe), PID=0, TL0PICIDX=0.
    return bytes([0x10, 0x00]) + b"\x9d\x01\x2a" + b"\x00" * 8


def vp9_descriptor() -> bytes:
    """VP9 payload with a keyframe descriptor (RFC 9559 §4.2)."""
    # I=1 (keyframe), P=0, L=0, F=1, B=0, V=0, Z=0.
    return bytes([0b10001000, 0x00]) + b"\x00" * 8


# ---------------------------------------------------------------------------
# Hex encoding / decoding (the on-disk format of the checked-in seeds)
# ---------------------------------------------------------------------------

def to_hex(data: bytes) -> str:
    """Render bytes as the commented hex text format used in git."""
    lines = ["# nimrtc fuzz corpus seed.",
             "# One seed per file; hex bytes, whitespace separated.",
             "# Regenerate with: python3 tests/fuzz/tools/make_corpus.py"]
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        lines.append(" ".join(f"{b:02x}" for b in chunk))
    return "\n".join(lines) + "\n"


def from_hex(text: str) -> bytes:
    """Parse the hex text format back into bytes.

    Each whitespace-separated token is one byte in two hex digits."""
    out = bytearray()
    for raw_line in text.splitlines():
        line = raw_line.split("#", 1)[0]
        for token in line.split():
            if len(token) != 2:
                raise ValueError(f"bad hex token: {token!r}")
            value = int(token, 16)
            if value > 0xFF:
                raise ValueError(f"hex token out of range: {token!r}")
            out.append(value)
    return bytes(out)


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------

def write(rel: str, data: bytes) -> None:
    path = CORPUS / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    # newline="\n" so the file is byte-identical on every platform:
    # Python would otherwise translate "\n" to "\r\n" on Windows.
    with path.open("w", encoding="ascii", newline="\n") as fh:
        fh.write(to_hex(data))
    print(f"{rel}: {len(data)} bytes")


def generate() -> None:
    # ---- RTP seeds ------------------------------------------------------
    write("rtp/simple.hex", rtp(b"payload bytes"))
    write("rtp/empty_payload.hex", rtp(b""))
    write("rtp/with_csrc.hex", rtp(b"x", csrc=[1, 2, 3]))
    write("rtp/with_extension.hex", rtp_with_ext(b"payload"))

    # ---- RTCP seeds -----------------------------------------------------
    write("rtp/rtcp_sr.hex", rtcp_sr())
    write("rtp/rtcp_rr.hex", rtcp_rr())
    write("rtp/rtcp_nack.hex", rtcp_nack())
    write("rtp/rtcp_compound.hex", rtcp_compound())

    # A compound packet whose second block claims a length that runs
    # past the end of the buffer — the classic truncation bug.
    bad = bytearray(rtcp_sr())
    bad += struct.pack(">BBH", 0x81, 202, 0xFFFF)
    write("rtp/rtcp_truncated_length.hex", bytes(bad))

    # ---- Video payload seeds -------------------------------------------
    write("video_payload/h264_single.hex", h264_single())
    write("video_payload/h264_stap_a.hex", h264_stap_a())
    write("video_payload/h264_fu_a_start.hex", h264_fu_a(start=True, end=False))
    write("video_payload/h264_fu_a_mid.hex", h264_fu_a(start=False, end=False))
    write("video_payload/h264_fu_a_end.hex", h264_fu_a(start=False, end=True))
    write("video_payload/h264_fu_a_whole.hex",
          h264_fu_a(start=True, end=True))
    write("video_payload/vp8.hex", vp8_descriptor())
    write("video_payload/vp9.hex", vp9_descriptor())

    # A STAP-A whose length prefix claims far more than the buffer
    # holds — the out-of-bounds read the parser has to reject.
    write("video_payload/h264_stap_a_overflow.hex",
          bytes([0x78]) + struct.pack(">H", 0xFFFF) + b"\x00" * 4)

    # An FU-A header with the body cut off entirely.
    write("video_payload/h264_fu_a_truncated.hex", bytes([0x7C]))


# ---------------------------------------------------------------------------
# Materialization
# ---------------------------------------------------------------------------

def materialize(out_dir: pathlib.Path) -> None:
    """Write the raw byte corpus libFuzzer expects into `out_dir`."""
    out_dir.mkdir(parents=True, exist_ok=True)
    for target_dir in sorted(p for p in CORPUS.iterdir() if p.is_dir()):
        dest_dir = out_dir / target_dir.name
        dest_dir.mkdir(parents=True, exist_ok=True)
        for seed in sorted(target_dir.iterdir()):
            if not seed.is_file():
                continue
            if seed.suffix == ".hex":
                data = from_hex(seed.read_text(encoding="ascii"))
                dest = dest_dir / (seed.stem + ".bin")
                dest.write_bytes(data)
                print(f"{dest}: {len(data)} bytes")
            else:
                dest = dest_dir / seed.name
                shutil.copyfile(seed, dest)
                print(f"{dest}: copied")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--materialize", metavar="DIR", type=pathlib.Path,
        help="write the raw byte corpus into DIR instead of regenerating "
             "the checked-in hex seeds")
    args = parser.parse_args()

    if args.materialize:
        materialize(args.materialize)
    else:
        generate()


if __name__ == "__main__":
    main()

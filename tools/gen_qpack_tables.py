#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the QPACK static and Huffman tables used by NimRTC's HTTP/3 layer.

The two tables are normative data from RFC 9204 (QPACK):

  * Appendix A - the QPACK static table (99 entries, index 0..98).
  * Appendix B - the Huffman code (257 symbols, EOS included).

The canonical copies in this repository live inside the vendored quiche
checkout (BSD-2-Clause, Cloudflare) at

  src/third_party/msquic/submodules/openssl/cloudflare-quiche/quiche/src/h3/
      qpack/static_table.rs   (STATIC_DECODE_TABLE)
      qpack/huffman/table.rs   (ENCODE_TABLE)

This script parses them and emits

  src/web_transport/src/qpack_tables.hpp

Run from the repository root:

    python tools/gen_qpack_tables.py

The generated header is checked in, so the build never depends on the
vendored quiche sources being present.

Structure of the generated header
---------------------------------

  kStaticTable        RFC 9204 Appendix A, 0-based (index 0 == ":authority").
  kHuffmanTable       (bits, code) per symbol, for encoding.
  kHuffmanCount       number of symbols per code length.
  kHuffmanFirstCode   first code value per code length.
  kHuffmanSymbolStart offset into kHuffmanSymbols per code length.
  kHuffmanSymbols     symbols grouped by code length, ascending within a
                       length, so that the symbol for a code of length L is

                           kHuffmanSymbols[kHuffmanSymbolStart[L]
                                           + (code - kHuffmanFirstCode[L])]
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
QUICHE = (ROOT / "src" / "third_party" / "msquic" / "submodules" / "openssl"
          / "cloudflare-quiche" / "quiche" / "src" / "h3" / "qpack")
OUT = ROOT / "src" / "web_transport" / "src" / "qpack_tables.hpp"


def parse_static_table() -> list[tuple[str, str]]:
    """Extract STATIC_DECODE_TABLE from quiche's static_table.rs."""
    text = (QUICHE / "static_table.rs").read_text(encoding="utf-8")
    marker = "pub const STATIC_DECODE_TABLE:"
    start = text.index(marker)
    body = text[start:]
    body = body[body.index("[") + 1:]
    body = body[:body.index("];")]

    # Every entry contributes exactly two byte-string literals, in order.
    literals = re.findall(r'\bb"((?:[^"\\]|\\.)*)"', body)
    if len(literals) % 2 != 0:
        raise SystemExit("odd number of byte-string literals in static table")
    entries: list[tuple[str, str]] = []
    for i in range(0, len(literals), 2):
        name = literals[i].encode().decode("unicode_escape")
        value = literals[i + 1].encode().decode("unicode_escape")
        entries.append((name, value))
    return entries


def parse_huffman_table() -> list[tuple[int, int]]:
    """Extract ENCODE_TABLE from quiche's huffman/table.rs."""
    text = (QUICHE / "huffman" / "table.rs").read_text(encoding="utf-8")
    marker = "pub const ENCODE_TABLE:"
    start = text.index(marker)
    body = text[start:]
    body = body[body.index("[") + 1:]
    body = body[:body.index("];")]

    entries: list[tuple[int, int]] = []
    for bits, code in re.findall(r"\(\s*(\d+)\s*,\s*0x([0-9a-fA-F]+)\s*\)", body):
        entries.append((int(bits), int(code, 16)))
    return entries


def build_decode_tables(codes: list[tuple[int, int]]):
    """Turn (bits, code) pairs into per-length canonical decode tables.

    RFC 9204 Appendix B is a canonical Huffman code: the codes of one code
    length form a consecutive run of integers, and within one length the
    symbols in ascending order receive ascending codes.  The *symbols* of one
    length are not contiguous - only the codes are - so a decoder indexes by
    code offset, not by symbol offset.
    """
    by_len: dict[int, list[tuple[int, int]]] = {}
    for symbol, (bits, code) in enumerate(codes):
        by_len.setdefault(bits, []).append((symbol, code))

    max_len = max(by_len)
    count = [0] * (max_len + 1)
    first_code = [0] * (max_len + 1)
    symbol_start = [0] * (max_len + 1)
    symbols: list[int] = []

    # next_code tracks the canonical code assignment: the first code of length
    # L is (first code of length L-1 + number of codes of length L-1) << 1.
    # Lengths that carry no symbol still consume a shift, so the running
    # value has to be advanced for every length, not only for populated ones.
    next_code = 0
    for length in range(1, max_len + 1):
        entries = sorted(by_len.get(length, []))
        count[length] = len(entries)

        if entries:
            values = [c for _, c in entries]
            if values != list(range(values[0], values[0] + len(values))):
                raise SystemExit(
                    f"code length {length} codes are not consecutive")
            if values[0] != next_code:
                raise SystemExit(
                    f"code length {length} is not canonical "
                    f"(expected first code {next_code}, got {values[0]})")
            first_code[length] = values[0]
            symbol_start[length] = len(symbols)
            symbols.extend(s for s, _ in entries)
        else:
            first_code[length] = next_code
            symbol_start[length] = len(symbols)

        next_code = (first_code[length] + count[length]) << 1

    return count, first_code, symbol_start, symbols, max_len


def cxx_escape(text: str) -> str:
    out = []
    for ch in text:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append("\\x%02x" % ord(ch))
    return "".join(out)


def main() -> int:
    static = parse_static_table()
    codes = parse_huffman_table()

    if len(static) != 99:
        raise SystemExit(f"expected 99 static entries, got {len(static)}")
    if len(codes) != 257:
        raise SystemExit(f"expected 257 huffman codes, got {len(codes)}")

    count, first_code, symbol_start, symbols, max_len = build_decode_tables(codes)

    lines: list[str] = []
    lines.append("/**")
    lines.append(" * @file src/web_transport/src/qpack_tables.hpp")
    lines.append(" * @brief QPACK static table and Huffman code (RFC 9204).")
    lines.append(" *")
    lines.append(" * GENERATED FILE - do not edit by hand.")
    lines.append(" * Regenerate with: python tools/gen_qpack_tables.py")
    lines.append(" *")
    lines.append(" * Sources (normative data from RFC 9204):")
    lines.append(" *   Appendix A - QPACK static table (99 entries).")
    lines.append(" *   Appendix B - Huffman code (257 symbols, EOS included).")
    lines.append(" *")
    lines.append(" * The canonical copies used for generation live in the vendored")
    lines.append(" * quiche checkout (BSD-2-Clause, Cloudflare):")
    lines.append(" *   quiche/src/h3/qpack/static_table.rs  (STATIC_DECODE_TABLE)")
    lines.append(" *   quiche/src/h3/qpack/huffman/table.rs  (ENCODE_TABLE)")
    lines.append(" */")
    lines.append("")
    lines.append("#ifndef NIMRTC_WEB_TRANSPORT_QPACK_TABLES_HPP")
    lines.append("#define NIMRTC_WEB_TRANSPORT_QPACK_TABLES_HPP")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace nimrtc::web_transport::detail::qpack_tables {")
    lines.append("")
    lines.append("/** @brief One entry of the QPACK static table. */")
    lines.append("struct StaticEntry {")
    lines.append("    const char* name;")
    lines.append("    const char* value;")
    lines.append("};")
    lines.append("")
    lines.append(f"/** @brief RFC 9204 Appendix A static table ({len(static)} entries, 0-based). */")
    lines.append(f"inline constexpr StaticEntry kStaticTable[{len(static)}] = {{")
    for name, value in static:
        lines.append(f'    {{"{cxx_escape(name)}", "{cxx_escape(value)}"}},')
    lines.append("};")
    lines.append("")
    lines.append("/** @brief One symbol of the QPACK Huffman code. */")
    lines.append("struct HuffmanCode {")
    lines.append("    std::uint8_t bits;   // code length in bits")
    lines.append("    std::uint32_t code;  // right-aligned code value")
    lines.append("};")
    lines.append("")
    lines.append("/** @brief RFC 9204 Appendix B Huffman code (symbol, EOS). */")
    lines.append(f"inline constexpr HuffmanCode kHuffmanTable[{len(codes)}] = {{")
    for bits, code in codes:
        lines.append(f"    {{{bits}, 0x{code:08x}u}},")
    lines.append("};")
    lines.append("")
    lines.append(f"/** @brief Longest Huffman code length ({max_len} bits). */")
    lines.append(f"inline constexpr int kHuffmanMaxBits = {max_len};")
    lines.append("")
    lines.append("/** @brief Number of symbols per code length (index = length). */")
    lines.append("inline constexpr std::uint16_t kHuffmanCount[] = {")
    for length in range(0, max_len + 1):
        lines.append(f"    {count[length]},")
    lines.append("};")
    lines.append("")
    lines.append("/** @brief First code value per code length (index = length). */")
    lines.append("inline constexpr std::uint32_t kHuffmanFirstCode[] = {")
    for length in range(0, max_len + 1):
        lines.append(f"    0x{first_code[length]:08x}u,")
    lines.append("};")
    lines.append("")
    lines.append("/**")
    lines.append(" * @brief Offset into kHuffmanSymbols of the first symbol of each")
    lines.append(" *        code length (index = length).")
    lines.append(" */")
    lines.append("inline constexpr std::uint16_t kHuffmanSymbolStart[] = {")
    for length in range(0, max_len + 1):
        lines.append(f"    {symbol_start[length]},")
    lines.append("};")
    lines.append("")
    lines.append("/**")
    lines.append(" * @brief Symbols grouped by code length, ascending within a length.")
    lines.append(" *")
    lines.append(" * The symbol for a code of length L and value c is")
    lines.append(" *")
    lines.append(" *     kHuffmanSymbols[kHuffmanSymbolStart[L] + (c - kHuffmanFirstCode[L])]")
    lines.append(" */")
    lines.append(f"inline constexpr std::uint16_t kHuffmanSymbols[{len(symbols)}] = {{")
    for i in range(0, len(symbols), 16):
        chunk = symbols[i:i + 16]
        lines.append("    " + ", ".join(str(s) for s in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace nimrtc::web_transport::detail::qpack_tables")
    lines.append("")
    lines.append("#endif  // NIMRTC_WEB_TRANSPORT_QPACK_TABLES_HPP")
    lines.append("")

    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {OUT} "
          f"({len(static)} static entries, {len(codes)} huffman codes, "
          f"max {max_len} bits)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

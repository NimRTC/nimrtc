#!/usr/bin/env python3
"""Repair the GBK mojibake in the C++ sources.

A tool opened the UTF-8 sources with a Chinese-codepage decoder (GBK)
and saved them back.  The damage has two shapes.

A three-byte UTF-8 sequence whose first two bytes form a valid GBK
pair became that single GBK character.  The third byte was then paired
with the byte that followed it; that pair was invalid, so the decoder
replaced both bytes with one question mark.

    "—"  (e2 80 94)  ->  U+9225 + "?"
    "→"  (e2 86 92)  ->  U+922B + "?"

The byte that followed the character was a space, and it was consumed
by the invalid pair, so the space has to come back.  The question mark
is the marker of the damage and is removed with it.

    "— the"  ->  U+9225 + "?" + "the"  ->  "— the"

A two-byte UTF-8 sequence that is a valid GBK pair all by itself lost
nothing but the character.

    "§"  (c2 a7)  ->  U+6402
    "×"  (c3 97)  ->  U+8133

Some files were written back in GBK rather than in UTF-8, so there the
damage sits at the byte level: the GBK encoding of the mangled
character followed by the question mark.

    "—"  ->  e2 80 3f  ->  e2 80 94 20

The first shape leaves a question mark behind, so it cannot be
confused with anything else.  The second leaves no trace, and the
character it leaves behind is a Chinese one -- a file that was Chinese
to begin with would look exactly the same.  The second shape is
therefore only repaired in a file that holds no other Chinese.

The rules are written with explicit escapes so that this script cannot
be damaged by the same mistake it repairs.

Usage:
    python tools/fix_gbk_mojibake.py [--check] [paths...]
    python tools/fix_gbk_mojibake.py --self-test
"""
import argparse
import pathlib
import sys

# Damage that left a question mark behind.  The mark is part of the
# damage, so these rules are safe to apply anywhere.
MARKED_FIXES = [
    ("\u9225?", "\u2014 "),   # em dash and the space it ate
    ("\u922b?", "\u2192 "),   # rightwards arrow and the space it ate
]

# Damage that left no trace: the whole UTF-8 sequence fitted into one
# GBK character, so only that character changed.  These rules rewrite a
# Chinese character into a Latin one, which is wrong if the file was
# Chinese to begin with -- so they only apply to a file that holds no
# other Chinese text (see repair_bytes).
SILENT_FIXES = [
    ("\u6402", "\u00a7"),     # section sign
    ("\u8133", "\u00d7"),     # multiplication sign
]

# The Chinese characters the damage itself produces.  Any other
# Chinese in a file means the file was Chinese to begin with.
DAMAGED_CJK = {"\u9225", "\u922b", "\u6402", "\u8133"}

# The range of the CJK unified ideographs, used to tell a file that
# was Chinese to begin with from a file that only carries the damage.
CJK_RANGES = [
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xF900, 0xFAFF),
]


def is_cjk(ch: str) -> bool:
    code = ord(ch)
    return any(low <= code <= high for low, high in CJK_RANGES)


# Damaged bytes -> the bytes they were before the mangling, for the
# files that were written back in GBK.  Neither sequence can occur in
# valid UTF-8, so these rules cannot touch healthy text.
BYTE_FIXES = [
    (b"\xe2\x80\x3f", b"\xe2\x80\x94 "),   # em dash, space eaten
    (b"\xe2\x86\x3f", b"\xe2\x86\x92 "),   # rightwards arrow, space eaten
]

# Directories that are never first-party sources.
SKIP_DIRS = {"third_party", "build", ".git", "node_modules", "__pycache__"}


def repair_bytes(raw: bytes) -> bytes:
    for broken, whole in BYTE_FIXES:
        raw = raw.replace(broken, whole)
    text = raw.decode("utf-8", errors="surrogateescape")
    for broken, whole in MARKED_FIXES:
        text = text.replace(broken, whole)
    if not has_foreign_cjk(text):
        for broken, whole in SILENT_FIXES:
            text = text.replace(broken, whole)
    return text.encode("utf-8", errors="surrogateescape")


def has_foreign_cjk(text: str) -> bool:
    """True if the text holds Chinese that the damage did not produce.

    The silent damage turns one ASCII symbol into one Chinese
    character.  A file that already contained Chinese text would look
    the same, and rewriting every instance of that one character would
    corrupt the text that was always there.  So the silent rules only
    run on a file whose Chinese consists of nothing but the characters
    the damage itself produced.
    """
    for ch in text:
        if is_cjk(ch) and ch not in DAMAGED_CJK:
            return True
    return False


def damaged_files(root: pathlib.Path) -> list:
    """Every first-party file that still carries the damage."""
    found = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if set(path.relative_to(root).parts) & SKIP_DIRS:
            continue
        raw = path.read_bytes()
        if any(broken in raw for broken, _ in BYTE_FIXES):
            found.append(path)
            continue
        text = raw.decode("utf-8", errors="replace")
        if any(broken in text for broken, _ in MARKED_FIXES):
            found.append(path)
            continue
        if has_foreign_cjk(text):
            continue
        if any(broken in text for broken, _ in SILENT_FIXES):
            found.append(path)
    return found


def _self_test() -> int:
    """Check the repair table against the damage it has to undo.

    The fixtures are byte-level, because the damage has to be
    reproduced exactly.  A Windows CP936 decoder that cannot make
    sense of a byte sequence replaces the whole sequence with a single
    question mark, and the question mark swallows the byte that
    followed -- which is why the space after an em dash disappears.
    Python's own GBK codec behaves differently (it emits U+FFFD and
    keeps the space), so the codec cannot be used to regenerate the
    damage here.

    Each fixture is the damaged bytes followed by the bytes they have
    to turn back into.
    """
    failures = []

    fixtures = [
        # An em dash, written back as UTF-8: U+9225 + "?" + the text
        # that followed the swallowed space.
        (b"\xe9\x88\xa5?the seam", b"\xe2\x80\x94 the seam"),
        # A rightwards arrow, same shape.
        (b"\xe9\x88\xab?engine demux",
         b"\xe2\x86\x92 engine demux"),
        # A section sign and a multiplication sign: the whole pair is
        # a valid GBK character, so only the character changed.
        (b"\xe6\x90\x826.4.3", b"\xc2\xa76.4.3"),
        (b"\xe8\x84\xb3100 Hz", b"\xc3\x97100 Hz"),
        # The same damage in a file that was written back in GBK, so
        # the mangled character sits at the byte level.
        (b"\xe2\x80\x3fthe seam", b"\xe2\x80\x94 the seam"),
        (b"\xe2\x86\x3fengine demux", b"\xe2\x86\x92 engine demux"),
    ]

    for damaged, expected in fixtures:
        got = repair_bytes(damaged)
        if got != expected:
            failures.append(
                "%r -> %r, want %r"
                % (damaged, got, expected))

    # A healthy file must come out unchanged.
    healthy = ("// an em dash \u2014 a section \u00a7 and an arrow "
               "\u2192\n").encode("utf-8")
    if repair_bytes(healthy) != healthy:
        failures.append("healthy file was modified")

    # A file that was Chinese to begin with must not be touched: the
    # silent damage is indistinguishable from that Chinese text.
    chinese = ("// \u63e1\u624b = handshake, \u6402 = a hand\n"
               ).encode("utf-8")
    if repair_bytes(chinese) != chinese:
        failures.append("Chinese text was modified")

    # The same damage in a file with no Chinese of its own is still
    # repaired.
    latin = "// x \u6402 6.4.3\n".encode("utf-8")
    if repair_bytes(latin) != "// x \u00a7 6.4.3\n".encode("utf-8"):
        failures.append("silent damage was not repaired")

    # The repair has to be idempotent: running it twice must not
    # change the file a second time.
    for damaged, expected in fixtures:
        once = repair_bytes(damaged)
        if repair_bytes(once) != once:
            failures.append("not idempotent for %r" % damaged)

    if failures:
        print("fix_gbk_mojibake self-test FAILED:")
        for line in failures:
            # The console may be a GBK code page, which cannot encode
            # every character the damage produces.
            print("  - %s" % line.encode("ascii", "backslashreplace").decode())
        return 1
    print("fix_gbk_mojibake self-test passed.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*", default=None,
                    help="files to repair; defaults to the whole tree")
    ap.add_argument("--check", action="store_true",
                    help="report the damage without writing it back")
    ap.add_argument("--self-test", action="store_true",
                    help="verify the repair rules against the damage "
                         "they have to undo")
    args = ap.parse_args()

    if args.self_test:
        return _self_test()

    root = pathlib.Path(__file__).resolve().parent.parent
    paths = ([pathlib.Path(p) for p in args.paths]
             if args.paths else damaged_files(root))
    if not paths:
        print("no damage found")
        return 0

    broken_files = 0
    for path in paths:
        raw = path.read_bytes()
        fixed = repair_bytes(raw)
        if fixed == raw:
            continue
        broken_files += 1
        if args.check:
            print(f"{path}: damaged")
        else:
            path.write_bytes(fixed)
            print(f"{path}: repaired")
    if args.check and broken_files:
        print(f"{broken_files} file(s) still carry the mojibake")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

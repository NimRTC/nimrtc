"""Self-test for scripts/check_unused_const.py.

Builds a throwaway tree per case in a temp directory and asserts what
the gate reports.  Run:

    python scripts/test_check_unused_const.py
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import shutil
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "chk", Path(__file__).with_name("check_unused_const.py")
)
chk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chk)


def run(root: Path) -> list[str]:
    """The gate's report lines, normalized for comparison."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        chk.main(["check_unused_const.py", str(root)])
    return [os.path.normcase(ln) for ln in buf.getvalue().splitlines() if ln]


def norm(path: Path, line: int, name: str) -> str:
    return os.path.normcase("%s:%d: %s" % (path, line, name))


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="unused-const-")).resolve()
    try:
        # 1. A plain unused internal-linkage constant: reported.
        case = tmp / "case1"
        (case).mkdir()
        (case / "a.cpp").write_text(
            "constexpr int kUnused = 1;\nint f() { return 0; }\n", encoding="utf-8"
        )
        assert run(case) == [norm(case / "a.cpp", 1, "kUnused")], run(case)

        # 2. A used constant: not reported.
        case = tmp / "case2"
        case.mkdir()
        (case / "b.cpp").write_text(
            "constexpr int kUsed = 1;\nint g() { return kUsed; }\n", encoding="utf-8"
        )
        assert run(case) == [], run(case)

        # 3. The same name in a sibling file does NOT rescue an unused
        #    constant -- internal linkage means only the own file can
        #    read it.  Both files are reported.
        case = tmp / "case3"
        case.mkdir()
        (case / "a.cpp").write_text(
            "constexpr int kUnused = 1;\nint f() { return 0; }\n", encoding="utf-8"
        )
        (case / "c.cpp").write_text(
            "constexpr int kUnused = 2;\nint h() { return 0; }\n", encoding="utf-8"
        )
        found = run(case)
        assert len(found) == 2, found
        assert any(os.path.normcase("a.cpp") in f for f in found), found
        assert any(os.path.normcase("c.cpp") in f for f in found), found

        # 4. `extern const` in a header gives the definition external
        #    linkage: Clang stays quiet, so it is not a candidate.
        case = tmp / "case4"
        case.mkdir()
        (case / "d.hpp").write_text("extern const int kExternal;\n", encoding="utf-8")
        (case / "d.cpp").write_text(
            "constexpr int kExternal = 3;\nint i() { return 0; }\n", encoding="utf-8"
        )
        found = run(case)
        assert not any("kExternal" in f for f in found), found

        # 5. An `inline` variable has external linkage since C++17.
        case = tmp / "case5"
        case.mkdir()
        (case / "e.cpp").write_text(
            "inline constexpr int kInline = 4;\nint j() { return 0; }\n", encoding="utf-8"
        )
        found = run(case)
        assert not any("kInline" in f for f in found), found

        # 6. `[[maybe_unused]]` silences the diagnostic.
        case = tmp / "case6"
        case.mkdir()
        (case / "f.cpp").write_text(
            "[[maybe_unused]]\nconstexpr int kAnnotated = 5;\nint k() { return 0; }\n",
            encoding="utf-8",
        )
        found = run(case)
        assert not any("kAnnotated" in f for f in found), found

        # 7. A mention in a comment is not a reference.
        case = tmp / "case7"
        case.mkdir()
        (case / "g.cpp").write_text(
            "constexpr int kCommented = 6;\n// kCommented is fine\nint l() { return 0; }\n",
            encoding="utf-8",
        )
        found = run(case)
        assert found == [norm(case / "g.cpp", 1, "kCommented")], found

        # 8. Vendored trees are skipped.
        case = tmp / "case8"
        (case / "third_party" / "x").mkdir(parents=True)
        (case / "third_party" / "x" / "y.cpp").write_text(
            "constexpr int kVendored = 7;\n", encoding="utf-8"
        )
        found = run(case)
        assert not any("kVendored" in f for f in found), found

        # 9. The reported line number survives comment stripping.
        case = tmp / "case9"
        case.mkdir()
        (case / "h.cpp").write_text(
            "/* a block\n   comment\n   spanning lines */\n"
            "constexpr int kLate = 8;\n",
            encoding="utf-8",
        )
        found = run(case)
        assert found == [norm(case / "h.cpp", 4, "kLate")], found

        # 10. An `extern const` declaration *before* the definition in
        #     the same file gives the definition external linkage:
        #     Clang stays quiet, so it is not a candidate.
        case = tmp / "case10"
        case.mkdir()
        (case / "j.cpp").write_text(
            "extern const int kBefore;\nconstexpr int kBefore = 9;\n"
            "int j() { return 0; }\n",
            encoding="utf-8",
        )
        found = run(case)
        assert not any("kBefore" in f for f in found), found

        # 11. An `extern const` declaration *after* the definition does
        #     not reach it: the definition keeps internal linkage, so
        #     Clang diagnoses it and the gate reports it.
        case = tmp / "case11"
        case.mkdir()
        (case / "k.cpp").write_text(
            "constexpr int kAfter = 10;\nextern const int kAfter;\n"
            "int k() { return 0; }\n",
            encoding="utf-8",
        )
        found = run(case)
        assert found == [norm(case / "k.cpp", 1, "kAfter")], found

        # 12. An `extern const` declaration in a *sibling* .cpp does not
        #     reach the definition either: linkage is judged per
        #     translation unit, so the constant stays internal and is
        #     reported.
        case = tmp / "case12"
        case.mkdir()
        (case / "l.cpp").write_text(
            "extern const int kSibling;\nint l() { return 0; }\n", encoding="utf-8"
        )
        (case / "m.cpp").write_text(
            "constexpr int kSibling = 11;\nint m() { return 0; }\n", encoding="utf-8"
        )
        found = run(case)
        assert found == [norm(case / "m.cpp", 1, "kSibling")], found

        # 13. A constant that is only referenced from another constant's
        #     initializer is referenced, not unused -- Clang does not
        #     diagnose it and the gate does not report it.
        case = tmp / "case13"
        case.mkdir()
        (case / "n.cpp").write_text(
            "constexpr int kRate = 48000;\nconstexpr int kFrame = kRate * 20 / 1000;\n"
            "int n() { return kFrame; }\n",
            encoding="utf-8",
        )
        found = run(case)
        assert found == [], found

        print("all checks passed")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())

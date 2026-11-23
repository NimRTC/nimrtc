"""Check that the published `.sha256` sidecars are `sha256sum -c` compatible.

The README tells a reader to verify a bundle with

    sha256sum -c nimrtc-<version>-<platform>.tar.xz.sha256

so the sidecar has to be a *checksum line*: a hex digest, whitespace, and
the name of the file the digest belongs to.  Two shapes that are not that
were published:

  * the Windows sidecar was the bare hex with no file name at all, and
    `sha256sum -c` rejected it with "no properly formatted SHA256
    checksum lines found";
  * the Linux sidecars named `build/nimrtc-...tar.xz`, so `sha256sum -c`
    looked for a file under a `build/` directory that does not exist once
    the bundle has been downloaded next to its sidecar ("No such file or
    directory").

Neither failure is visible in the release log: the digest was always
correct.  This tool is the gate that says so out loud.

The check is deliberately the *format* one, not the digest one: the digest
is verified against the file it sits next to by `sha256sum -c` itself,
which the release job runs.

Usage:

    python tools/check_sha256_sidecars.py dist/            # check a directory
    python tools/check_sha256_sidecars.py --self-test
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

#: A hex digest of 64 characters, as SHA-256 produces.
_DIGEST_RE = re.compile(r"^[0-9a-f]{64}$")

#: The name a checksum line must carry: the file it belongs to, with no
#: directory component.  `sha256sum` prints the path it was given, so
#: "build/foo.tar.xz" has to be rejected -- the reader has "foo.tar.xz".
_FILENAME_RE = re.compile(r"^[A-Za-z0-9._+-]+$")


class SidecarError(Exception):
    """A sidecar is not in the shape `sha256sum -c` accepts."""


def parse_sidecar(text: str) -> tuple[str, str]:
    """Return `(digest, filename)` for a sidecar, or raise `SidecarError`.

    The rules are the ones `sha256sum -c` enforces, in the order it
    reports them:

      * the file must not be empty and must hold exactly one line;
      * the line must have a digest field and a name field;
      * the digest must be 64 lowercase hex characters;
      * the name must not contain a directory component;
      * the file must not use CRLF line endings -- `sha256sum` reads the
        `\r` as part of the file name ("...tar.xz'$'\\r': No such file
        or directory"), so a sidecar written by PowerShell's
        `Set-Content` fails on every Unix reader.  A lone `\r` breaks it
        the same way, so *any* CR is rejected.
    """
    if "\r" in text:
        raise SidecarError(
            "the sidecar contains a carriage return; `sha256sum -c` reads "
            "it as part of the file name"
        )
    lines = [ln for ln in text.split("\n") if ln.strip()]
    if not lines:
        raise SidecarError("the sidecar is empty")
    if len(lines) > 1:
        raise SidecarError(
            "the sidecar has %d lines; `sha256sum -c` reads one checksum "
            "line per file" % len(lines)
        )

    fields = lines[0].split()
    if len(fields) < 2:
        raise SidecarError(
            "the line has no file name; `sha256sum -c` needs a checksum "
            "line (a digest and the name of the file)"
        )
    digest, name = fields[0], fields[-1]
    if not _DIGEST_RE.match(digest):
        raise SidecarError(
            "the digest %r is not 64 lowercase hex characters" % digest
        )
    if "/" in name or "\\" in name:
        raise SidecarError(
            "the file name %r carries a directory component; the sidecar "
            "must name the file the way the reader has it" % name
        )
    if not _FILENAME_RE.match(name):
        raise SidecarError("the file name %r contains unusable characters" % name)
    return digest, name


def check_sidecar(path: pathlib.Path) -> tuple[str, str]:
    """Check one sidecar file; return `(digest, filename)`."""
    return parse_sidecar(path.read_text(encoding="utf-8"))


def check_directory(directory: pathlib.Path) -> list[str]:
    """Check every `.sha256` sidecar in `directory`.

    Return a list of human-readable problems; an empty list means every
    sidecar is `sha256sum -c` compatible.
    """
    problems: list[str] = []
    sidecars = sorted(directory.glob("*.sha256"))
    if not sidecars:
        return ["no `.sha256` sidecar found in %s" % directory]
    for sidecar in sidecars:
        try:
            digest, name = check_sidecar(sidecar)
        except SidecarError as exc:
            problems.append("%s: %s" % (sidecar.name, exc))
            continue
        # The name must match the bundle the sidecar belongs to: a
        # sidecar for `foo.tar.xz` that names `bar.tar.xz` verifies
        # nothing.
        expected = sidecar.name[: -len(".sha256")]
        if name != expected:
            problems.append(
                "%s: names %r but belongs to %r" % (sidecar.name, name, expected)
            )
    return problems


def _self_test() -> int:
    """Pin the rules.  Returns the number of failures."""
    failures = 0

    def ok(label: str, fn, *, should_pass: bool) -> None:
        nonlocal failures
        try:
            fn()
            passed = should_pass
            detail = ""
        except SidecarError as exc:
            passed = not should_pass
            detail = " (%s)" % exc
        if passed:
            print("[OK  ] %s%s" % (label, detail))
        else:
            print("[FAIL] %s%s" % (label, detail))
            failures += 1

    good = "%s  nimrtc-1.3.0-linux-x86_64-gcc.tar.xz\n" % ("a" * 64)
    ok("a well-formed checksum line passes",
       lambda: parse_sidecar(good), should_pass=True)
    ok("a line without a trailing newline passes",
       lambda: parse_sidecar(good.rstrip("\n")), should_pass=True)
    ok("a CRLF line is rejected",
       lambda: parse_sidecar(good.replace("\n", "\r\n")), should_pass=False)
    ok("a lone CR is rejected",
       lambda: parse_sidecar(good.replace("\n", "\r")), should_pass=False)
    ok("a CR before the final LF is rejected",
       lambda: parse_sidecar(good.replace("\n", "\r\n")), should_pass=False)
    ok("an empty sidecar is rejected",
       lambda: parse_sidecar(""), should_pass=False)
    ok("a whitespace-only sidecar is rejected",
       lambda: parse_sidecar("   \n"), should_pass=False)
    ok("a bare digest with no file name is rejected",
       lambda: parse_sidecar("%s\n" % ("a" * 64)), should_pass=False)
    ok("a digest with a directory prefix is rejected",
       lambda: parse_sidecar("%s  build/foo.tar.xz\n" % ("a" * 64)), should_pass=False)
    ok("an uppercase digest is rejected",
       lambda: parse_sidecar("%s  foo.tar.xz\n" % ("A" * 64)), should_pass=False)
    ok("a short digest is rejected",
       lambda: parse_sidecar("%s  foo.tar.xz\n" % ("a" * 32)), should_pass=False)
    ok("a two-line sidecar is rejected",
       lambda: parse_sidecar(good + good), should_pass=False)
    ok("the 'Algorithm' header line is rejected",
       lambda: parse_sidecar("Algorithm Hash Path\n"), should_pass=False)

    # The directory check: a sidecar whose name disagrees with the
    # bundle it sits next to verifies nothing.
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        (d / "nimrtc-1.3.0-linux-x86_64-gcc.tar.xz.sha256").write_text(
            good, encoding="utf-8"
        )
        problems = check_directory(d)
        if problems:
            print("[FAIL] a matching sidecar is accepted (%s)" % problems[0])
            failures += 1
        else:
            print("[OK  ] a matching sidecar is accepted")

        (d / "nimrtc-1.3.0-macos-arm64-clang.tar.xz.sha256").write_text(
            "%s  wrong-name.tar.xz\n" % ("b" * 64), encoding="utf-8"
        )
        problems = check_directory(d)
        if problems and "wrong-name" in problems[0]:
            print("[OK  ] a sidecar naming the wrong file is reported")
        else:
            print("[FAIL] a sidecar naming the wrong file is reported")
            failures += 1

    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        problems = check_directory(d)
        if problems and "no `.sha256` sidecar" in problems[0]:
            print("[OK  ] an empty directory is reported")
        else:
            print("[FAIL] an empty directory is reported")
            failures += 1

    print()
    print("self-test: %d failure(s)" % failures)
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "directory",
        nargs="?",
        type=pathlib.Path,
        help="the directory holding the `.sha256` sidecars",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return 1 if _self_test() else 0
    if args.directory is None:
        parser.error("a directory is required (or --self-test)")

    problems = check_directory(args.directory)
    if problems:
        for problem in problems:
            print("::error::%s" % problem)
        print(
            "::error::the `.sha256` sidecars are not `sha256sum -c` compatible; "
            "the README documents that command, so it must work"
        )
        return 1
    print("[+] every `.sha256` sidecar is `sha256sum -c` compatible")
    return 0


if __name__ == "__main__":
    sys.exit(main())

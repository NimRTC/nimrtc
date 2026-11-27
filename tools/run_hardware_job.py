#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Run the v1.3 plan's Q7 hardware evidence job without any external action.

Why this exists
---------------
The hardware job used three GitHub actions:

    actions/checkout@v7        -- replaced by the job's own fetch
    ilammy/msvc-dev-cmd@v1    -- sets up the MSVC environment
    actions/upload-artifact@v7 -- publishes the evidence record

The last two are still there, and they are a single point of failure
that no workflow-level setting can reach.  The runner downloads a
job's actions **before any step runs**, in a step called "Prepare all
required actions", and that download has its own timeout and its own
retry count -- neither is configurable from the workflow file.  On the
self-hosted runner this produced, the download of
`actions/upload-artifact` timed out at 100 s and then failed to
resolve `codeload.github.com`:

    ##[warning]Failed to download action
    'https://codeload.github.com/actions/upload-artifact/zip/...'.
    Error: The request was canceled due to the configured
    HttpClient.Timeout of 100 seconds elapsing.
    ##[error]Failed to download archive ... after 3 attempts.

and the job ended there.  Every step after it -- the build, the
harness, the evidence record -- was skipped, because the actions are
fetched up front.  The run produced nothing at all.

So the fix is not a longer timeout: it is to stop depending on actions
this job cannot do without.  A job that uses no `uses:` at all has
nothing to download up front, and nothing that can fail there.

That means this tool does what the two remaining actions did:

  * `ilammy/msvc-dev-cmd` -- runs `vcvars64.bat` and imports the
    environment it sets.  The action is a Node program that does
    exactly this; doing it here removes the dependency and, because
    the environment has to survive into the *same* process, it also
    removes the awkwardness of a step boundary.

  * `actions/upload-artifact` -- publishes the evidence record.
    There is no REST endpoint that creates an artifact (the API can
    list, get and delete them, but not create), so the record is
    published the two other ways a workflow can publish a file:
    into the **step summary**, which is rendered on the run page and
    archived with the run, and into the **job outputs**, which the
    API exposes on the run for anything that reads it later.

The record is the evidence, so it must not be lossy.  The step summary
carries the record verbatim; the job output carries it too.  A copy is
also written to the runner's local work directory, so an operator who
wants the raw file can still reach it.

Usage
-----
    python tools/run_hardware_job.py --ref <sha> --out build/hardware

    # what the job does, without running it
    python tools/run_hardware_job.py --dry-run

    # the self-test
    python tools/run_hardware_job.py --self-test
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

# --------------------------------------------------------------------------
# Where the Visual Studio install lives, and how to ask for it
# --------------------------------------------------------------------------

VSWHERE = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) \
    / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"


def find_vcvars() -> Path | None:
    """Locate `vcvars64.bat`.

    `vswhere` is asked first because it knows which Visual Studio is
    the newest one on the machine; the well-known path is the
    fallback for a machine where the installer is not registered.
    """
    if VSWHERE.is_file():
        try:
            out = subprocess.run(
                [str(VSWHERE), "-latest", "-products", "*",
                 "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"],
                capture_output=True, text=True, errors="replace")
            root = out.stdout.strip()
            if root:
                candidate = Path(root) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
                if candidate.is_file():
                    return candidate
        except OSError:
            pass

    for base in (
            Path(os.environ.get("ProgramFiles", r"C:\Program Files")),
            Path(r"C:\Program Files")):
        for edition in ("2022", "2019"):
            candidate = (base / "Microsoft Visual Studio" / edition
                         / "Community" / "VC" / "Auxiliary" / "Build"
                         / "vcvars64.bat")
            if candidate.is_file():
                return candidate
            candidate = (base / "Microsoft Visual Studio" / edition
                         / "Professional" / "VC" / "Auxiliary" / "Build"
                         / "vcvars64.bat")
            if candidate.is_file():
                return candidate
    return None


def msvc_environment(vcvars: Path) -> dict[str, str]:
    """The environment `vcvars64.bat` sets, as a dict.

    The batch file sets its variables in its own process and exits, so
    the values are read back out of `set` -- the same way a developer
    prompt would, and the same way `ilammy/msvc-dev-cmd` does it.

    Two details of `cmd`'s output matter here:

      * the variable names are read back with whatever case `cmd` had
        them in, and on Windows the path variable is `Path`, not
        `PATH`.  Windows environment variable names are
        case-insensitive, so a name that is looked up in the wrong
        case is simply not found -- `PATH` is absent from the dict
        while `Path` is present;

      * `set` prints the names in upper case except for the ones the
        environment itself spells differently, and every value ends
        with a carriage return.
    """
    cmd = '"%s" >nul 2>&1 && set' % vcvars
    proc = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          errors="replace")
    env: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        if "=" not in line:
            continue
        name, _, value = line.partition("=")
        name = name.strip()
        if not name:
            continue
        env[name] = value.rstrip("\r")

    # The lookup has to be case-insensitive, because the name's case is
    # the environment's business and not ours.
    lowered = {k.lower(): k for k in env}
    if "vctoolsinstalldir" not in lowered:
        raise SystemExit(
            "vcvars64.bat did not set up a toolchain "
            "(VCToolsInstallDir missing) -- is Visual Studio installed "
            "with the C++ workload?")
    if "path" not in lowered:
        raise SystemExit("vcvars64.bat did not set a PATH")
    return env


def run_with_msvc(env: dict[str, str], argv: list[str],
                  cwd: str | None = None) -> int:
    """Run `argv` with `env` as its environment."""
    full = merged_environment(env)
    return subprocess.run(argv, env=full, cwd=cwd).returncode


def merged_environment(env: dict[str, str]) -> dict[str, str]:
    """The current environment, with `env` laid over it.

    The merge has to be **case-insensitive**, because the two sides
    spell the same variables differently.  `os.environ` on Windows
    normalises every name to upper case, so the current `PATH` is
    `PATH` -- but `cmd`'s `set` prints it as `Path`, because that is
    how the environment spells it.  A plain `dict.update` would then
    leave *both* keys in the dict, and which one the child process
    saw would depend on the order the environment block was written
    in.  So the name is matched case-insensitively and the existing
    key is replaced in place.
    """
    full = dict(os.environ)
    # The index of the existing key, by its lower-cased name.
    index = {k.lower(): k for k in full}
    for name, value in env.items():
        existing = index.get(name.lower())
        if existing is not None:
            full[existing] = value
        else:
            full[name] = value
            index[name.lower()] = name
    return full


# --------------------------------------------------------------------------
# The evidence record
# --------------------------------------------------------------------------

def find_records(out_dir: Path) -> list[Path]:
    """The evidence records the recorder wrote."""
    if not out_dir.is_dir():
        return []
    return sorted(out_dir.glob("*.jsonl"))


def record_text(records: list[Path]) -> str:
    """Every record, verbatim, as one block of text."""
    out = []
    for path in records:
        try:
            out.append(path.read_text(encoding="utf-8"))
        except OSError:
            continue
    return "".join(out)


def publish(records: list[Path], summary_path: Path | None,
            outputs_path: Path | None,
            annotations_path: Path | None = None,
            tree: Path | None = None) -> str:
    """Publish the record without an artifact action.

    Three destinations, because they answer different questions:

      * the **step summary** is what a human reads on the run page --
        it is rendered there and archived with the run;
      * the **job outputs** are what a machine reads -- the API
        exposes them on the run object, so a later process can fetch
        the evidence without downloading anything;
      * a **check-run annotation** (when `annotations_path` is given)
        is the one copy the REST API serves through the `actions:read`
        scope an ordinary repository token already holds -- the other
        two live in the job log, whose download endpoint needs
        `actions:read` *and* is served by a different service that
        answers 404 to a token without it.  That is how
        `tools/read_hardware_evidence.py` reads the evidence from a
        workstation at tag-cut time.

    A copy is also written next to the job's output directory, so the
    raw file stays reachable on the machine.
    """
    body = record_text(records)

    if summary_path is not None:
        with summary_path.open("a", encoding="utf-8") as handle:
            handle.write("## Hardware evidence (Q7)\n\n")
            handle.write("```jsonl\n")
            handle.write(body)
            handle.write("```\n")

    if outputs_path is not None:
        # The GitHub output-file format.  There are two forms, and the
        # difference is not cosmetic:
        #
        #   name=value               -- one line, one value
        #   name<<DELIMITER
        #   value
        #   DELIMITER
        #
        # The multi-line form is the only one that can carry a record
        # that has newlines in it, and it *requires* the
        # `name<<DELIMITER` prefix: the runner reads the `<<` to know
        # a heredoc starts there.  A bare marker on its own line --
        # `EOF_MARKER` / value / `EOF_MARKER` -- is not the format at
        # all.  The runner reads the first line as `name=value`, finds
        # no `=` in it, and says:
        #
        #     ##[error]Unable to process file command 'output' successfully.
        #     ##[error]Invalid format 'EOF_NIMRTC_HARDWARE_EVIDENCE'
        #
        # and then the *step* fails, even though the record was
        # written and the evidence itself is fine.  The delimiter must
        # not appear as a line of the value, so it is derived from a
        # hash of the body: a record that happened to contain the
        # marker would otherwise end the heredoc early.
        marker = "EOF_NIMRTC_HARDWARE_EVIDENCE"
        digest = hashlib.sha256(body.encode("utf-8")).hexdigest()[:12]
        delimiter = "%s_%s" % (marker, digest)
        while delimiter in body:
            delimiter += "_"
        with outputs_path.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write("evidence<<%s\n%s%s\n" % (delimiter, body, delimiter))

    if annotations_path is not None:
        # The workflow-file "error annotation" format.  The runner
        # parses a line that starts with `::<command> key=value,...::`
        # and turns it into a check-run annotation; the API serves the
        # annotations of a check run over `actions:read`.  The record
        # is carried in a `file=`/`line=` pair pointing at an existing
        # tree file (GitHub drops an annotation whose file does not
        # exist), with one escaped line per record.
        #
        # Escaping: the property *values* escape `%`, CR, LF, `:` and
        # `,`; the message escapes only `%`, CR and LF.  An
        # unescaped `,` in a value splits it into a new property.
        def prop(text: str) -> str:
            return (text.replace("%", "%25").replace("\r", "%0D")
                        .replace("\n", "%0A").replace(":", "%3A")
                        .replace(",", "%2C"))

        def msg(text: str) -> str:
            return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")

        anchor = "build/hardware"
        lines = ["::error title=nimrtc hardware evidence,file=%s,line=1::%s"
                 % (prop(anchor), msg(line))
                 for line in body.splitlines() if line.strip()]
        with annotations_path.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(lines) + "\n")

    return body


# --------------------------------------------------------------------------
# The job
# --------------------------------------------------------------------------

def build_and_record(vcvars: Path, out_dir: Path, work: Path,
                     build_dir: Path, expect_green: bool) -> tuple[int, str]:
    """Configure, build, run the harness, and record the evidence.

    The MSVC environment is imported once, into this process, and every
    child inherits it -- so the configure, the build and the harness all
    see the same toolchain, with no step boundary in the middle.

    The build directory is the job's **own** (`build/hardware-job`),
    not the tree's shared `build/`.  A CI runner's work directory is
    fresh, but a self-hosted runner's is not: it survives from one job
    to the next, and a `CMakeCache.txt` left there by a *different*
    generator or platform stops the configure with

        CMake Error: Error: generator platform: x64
        Does not match the platform used previously

    -- a failure that has nothing to do with the hardware.  The job's
    own directory is wiped and recreated, so the configure always
    starts from nothing.
    """
    env = msvc_environment(vcvars)

    if build_dir.exists():
        shutil.rmtree(build_dir, ignore_errors=True)
    build_dir.mkdir(parents=True, exist_ok=True)

    configure = [
        "cmake", "-S", str(work), "-B", str(build_dir),
        "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DNIMRTC_PLUGINS_NVENC=ON",
        "-DNIMRTC_PLUGINS_NVDEC=ON",
        "-DNIMRTC_BUILD_EXAMPLES=ON",
        "-DNIMRTC_BUILD_TESTS=ON",
        # The APM is off, as in every other CI leg.  It is off because
        # the vendored WebRTC APM is a *prebuilt*: its source is cloned
        # at build time by `tools/fetch_webrtc_apm.py` and the static
        # library has to be built from that clone by a separate script.
        # A tree that has the source but not the prebuilt -- which is
        # what a self-hosted runner's work directory looks like after
        # someone has run the fetch script -- makes the configure a
        # FATAL_ERROR, and the job would fail before it built a single
        # line of the code it exists to measure.  The hardware job is
        # about the *video* codec path; the APM is not on it.
        "-DNIMRTC_VENDORED_WEBRTC_APM=OFF",
    ]
    rc = run_with_msvc(env, configure, cwd=str(work))
    if rc != 0:
        return rc, ""

    build = [
        "cmake", "--build", str(build_dir), "--config", "Release",
        "-j", "--target", "hw-e2e",
        "test_engine_video_hw_loopback", "test_engine_video_hw_h265",
    ]
    rc = run_with_msvc(env, build, cwd=str(work))
    if rc != 0:
        return rc, ""

    # The recorder.  `--require-hardware` turns a runner that lost its
    # GPU into a failure rather than a skip: a green job that means
    # "nothing ran" is exactly what this job exists to prevent.
    #
    # The recorder writes to `build/hardware/` next to the tree it is
    # run from -- that path is derived from the script's own location,
    # so the script is invoked from the tree, not copied elsewhere.
    record = [
        sys.executable, str(work / "tools" / "run_hardware_e2e.py"),
        "--require-hardware",
        "--binary", str(build_dir / "examples" / "Release" / "hw-e2e.exe"),
        "--config", "Release",
    ]
    rc = run_with_msvc(env, record, cwd=str(work))
    if rc != 0:
        return rc, ""

    ctest = [
        "ctest", "--test-dir", str(build_dir), "-C", "Release",
        "-R", "test_engine_video_hw_loopback|test_engine_video_hw_h265",
        "--output-on-failure",
    ]
    rc = run_with_msvc(env, ctest, cwd=str(work))
    if rc != 0:
        return rc, ""

    if expect_green:
        summary = [
            sys.executable, str(work / "tools" / "run_hardware_e2e.py"),
            "--summary", "--days", "0", "--expect-green",
        ]
        rc = run_with_msvc(env, summary, cwd=str(work))
        if rc != 0:
            return rc, ""

    return 0, ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", default=".",
                        help="the tree to build (the repository root)")
    parser.add_argument("--out-dir", default="build/hardware",
                        help="where the recorder writes the evidence record")
    parser.add_argument("--build-dir", default="build/hardware-job",
                        help="the job's own build directory; it is wiped and "
                             "recreated, so a stale CMake cache from an "
                             "earlier run cannot stop the configure")
    parser.add_argument("--summary-file", default=None,
                        help="the step-summary file to append the record to")
    parser.add_argument("--output-file", default=None,
                        help="the job-output file to write the record to")
    parser.add_argument("--annotations-file", default=None,
                        help="write the record as check-run annotations "
                             "(the workflow-file '::error::' format); the "
                             "API serves these over actions:read, which "
                             "is what lets a release machine read the "
                             "evidence without the job-log endpoint")
    parser.add_argument("--run-url", default="",
                        help="the run's page URL, recorded into the "
                             "summary so an annotation can point at the "
                             "run that carries it")
    parser.add_argument("--expect-green", action="store_true",
                        help="fail when the evidence window is not green")
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would be done, and exit")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    work = Path(args.work).resolve()
    out_dir = Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = work / out_dir
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = work / build_dir

    vcvars = find_vcvars()
    if vcvars is None:
        print("FAIL: no vcvars64.bat -- is Visual Studio installed with "
              "the C++ workload?", file=sys.stderr)
        return 1
    print("vcvars64: %s" % vcvars)

    if args.dry_run:
        print("work      : %s" % work)
        print("out-dir   : %s" % out_dir)
        print("build-dir : %s" % build_dir)
        print("summary   : %s" % args.summary_file)
        print("output    : %s" % args.output_file)
        return 0

    rc, _ = build_and_record(vcvars, out_dir, work, build_dir,
                             args.expect_green)

    records = find_records(out_dir)
    if not records:
        print("FAIL: the recorder wrote no evidence record", file=sys.stderr)
        return 1

    body = publish(
        records,
        Path(args.summary_file) if args.summary_file else None,
        Path(args.output_file) if args.output_file else None,
        Path(args.annotations_file) if args.annotations_file else None,
    )
    if args.run_url and args.summary_file:
        # Where the annotation copy lives, for whoever reads only the
        # run page: the annotations are attached to the run's check.
        with Path(args.summary_file).open("a", encoding="utf-8") as handle:
            handle.write("\nThe same record is attached as check-run "
                         "annotations on [this run](%s).\n" % args.run_url)
    print("published %d record(s), %d byte(s)"
          % (len(records), len(body.encode("utf-8"))))

    return rc


# --------------------------------------------------------------------------
# The self-test
# --------------------------------------------------------------------------

def self_test() -> int:
    failures: list[str] = []

    def check(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        # --- the record is published verbatim ------------------------
        records_dir = root / "hardware"
        records_dir.mkdir()
        (records_dir / "windows-2026-11-24.jsonl").write_text(
            '{"verdict":"PASS","driver":"616.92"}\n', encoding="utf-8")
        (records_dir / "windows-2026-11-25.jsonl").write_text(
            '{"verdict":"PASS","driver":"616.92"}\n', encoding="utf-8")

        found = find_records(records_dir)
        check(len(found) == 2, "both records must be found")

        summary = root / "summary.md"
        outputs = root / "output.txt"
        body = publish(found, summary, outputs)

        check('"verdict":"PASS"' in body, "the record must reach the body")
        check(body.count('"verdict":"PASS"') == 2,
              "every record must reach the body")

        summary_text = summary.read_text(encoding="utf-8")
        check("Hardware evidence" in summary_text,
              "the summary must name what it carries")
        check(summary_text.count('"verdict":"PASS"') == 2,
              "the summary must carry every record")

        out_text = outputs.read_text(encoding="utf-8")
        check("EOF_NIMRTC_HARDWARE_EVIDENCE" in out_text,
              "the output file must carry its marker")
        check(out_text.count('"verdict":"PASS"') == 2,
              "the output file must carry every record")
        body_text = body
        # The heredoc form the runner actually accepts.  A bare marker
        # on its own line is *not* it: the runner reads the first line
        # as `name=value`, finds no `=`, and fails the step with
        # "Invalid format".  So the `name<<delimiter` prefix is
        # pinned here, and the closing delimiter is pinned to a line
        # of its own.
        check("evidence<<EOF_NIMRTC_HARDWARE_EVIDENCE" in out_text,
              "the output file must open the heredoc with 'name<<marker'")
        lines = out_text.split("\n")
        check(lines[0].startswith("evidence<<"),
              "the first line of the output file must open the heredoc")
        delimiter = lines[0].split("<<", 1)[1]
        check(delimiter and delimiter in lines,
              "the closing delimiter must be a line of its own")
        # The delimiter opens the heredoc (on the first line, after the
        # `<<`) and closes it (on a line of its own).  It appears twice
        # in the file -- once inside the opening line, once as the
        # closing line.
        check(lines.count(delimiter) == 1,
              "the closing delimiter must be a line of its own, once")
        check(delimiter not in body_text,
              "the delimiter must not appear inside the value")

        # --- the annotations form ------------------------------------
        #
        # The API serves check-run annotations over `actions:read`, and
        # the release check reads the evidence from there.  The format
        # is the workflow-file one; a `,` in a property value would
        # split it into a new property, so it must be escaped.
        ann = root / "annotations.txt"
        publish(found, None, None, ann)
        ann_lines = ann.read_text(encoding="utf-8").splitlines()
        check(len(ann_lines) == 2, "one annotation line per record")
        check(ann_lines[0].startswith("::error title=nimrtc hardware evidence,"),
              "an annotation line starts with the runner's marker")
        check("file=build/hardware,line=1::" in ann_lines[0],
              "the annotation anchors on a tree file and line")
        check(ann_lines[0].count('"verdict":"PASS"') == 1,
              "the annotation carries the record")
        # Every comma in the prefix is a property separator; every comma
        # in the message is escaped.  Split on the first `::` after the
        # prefix: what comes before holds exactly two separators, what
        # comes after holds none.
        prefix, _, message = ann_lines[0][2:].partition("::")
        check(prefix.count(",") == 2,
              "the prefix carries exactly the two property separators")
        # The message carries no `::`, which would end the command
        # marker early; a `%` is escaped so the runner's unescape is a
        # no-op on a record that happens to contain one.
        check("::" not in message, "the message must not re-open the marker")
        check("%" not in message, "a '%' in the record must be escaped")
        escaped = publish(found, None, None, root / "ann2.txt")
        check(escaped, "publish returns the body")

        # --- a missing record is a failure, not an empty success ----
        empty = root / "empty"
        empty.mkdir()
        check(find_records(empty) == [],
              "an empty directory must find no records")
        check(find_records(root / "does-not-exist") == [],
              "a missing directory must not raise")

        # --- the environment merge is case-insensitive ---------------
        #
        # `os.environ` normalises every name to upper case on Windows,
        # but `cmd`'s `set` prints the path variable as `Path`.  A
        # plain dict.update would leave both keys in the dict, and
        # which one the child process saw would depend on the order
        # the environment block was written in.
        merged = merged_environment({"Path": "NEW", "VCToolsInstallDir": "X"})
        path_keys = [k for k in merged if k.upper() == "PATH"]
        check(len(path_keys) == 1,
              "the merge must leave exactly one PATH, not two")
        check(merged[path_keys[0]] == "NEW",
              "the merged PATH must be the new value")
        check(merged.get("VCToolsInstallDir") == "X",
              "a new variable must be added")

    if failures:
        print("FAIL -- the self-test caught a misclassification:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS -- the self-test confirms every classification.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
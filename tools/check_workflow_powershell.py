#!/usr/bin/env python3
"""Parse the PowerShell steps of the workflows, before they run.

Why this exists
---------------
A `run:` block with `shell: pwsh` (or `shell: powershell`) is PowerShell,
and PowerShell has a syntax error that no other check in this repository
can see.  The YAML parses.  The script is valid text.  But PowerShell
reads `$var:` as a *drive reference* -- the `:` is a drive qualifier, not
punctuation -- so

    Write-Host "mirror $m: not a clone"

is a **parser error**, not a string with a colon in it.  The step then
dies before it does anything, and the job's log says

    InvalidVariableReferenceWithDrive

without naming the mistake.

That happened on 2026-11-24, in the hardware job's own mirror-fetching
step, on the self-hosted Windows runner.  The step that was written to
make the job *survive* a broken network link was the step that failed:
the log line naming the mirror was the bug.

The guard takes every `run:` block of every workflow, resolves the shell
each one runs under (the step's own `shell:`, else the job's
`defaults.run.shell`, else GitHub's default for the runner's OS), and
parses the PowerShell ones with the PowerShell parser itself.  The
`${{ ... }}` expressions are masked first: GitHub substitutes them
before the shell sees the script, and `${{` reads as a hashtable literal
to PowerShell, so the block as written is not the block that runs.

The check needs the PowerShell parser, so it only runs where
PowerShell is: on Windows, and on the Linux and macOS runners it reports
that it was skipped rather than passing silently.

    python tools/check_workflow_powershell.py
    python tools/check_workflow_powershell.py --self-test
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = REPO_ROOT / ".github" / "workflows"

# `${{ ... }}` -- non-greedy, across lines.  GitHub substitutes these
# before the shell runs, so the block as written is not the block that
# runs.
EXPRESSION = re.compile(r"\$\{\{.*?\}\}", re.DOTALL)

# The shells this guard knows how to parse.
POWERSHELL_SHELLS = ("powershell", "pwsh")

# GitHub's default shell per runner OS: bash everywhere except Windows,
# where it is PowerShell.
WINDOWS_RUNNER = re.compile(r"windows|self-hosted|win", re.IGNORECASE)

try:
    import yaml
except ImportError:  # pragma: no cover - the dependency is optional
    yaml = None


def _powershell_parser() -> str | None:
    """The path of a PowerShell that has the language parser, or None."""
    for exe in ("pwsh", "powershell"):
        path = shutil.which(exe)
        if path:
            return path
    return None


def _is_windows_runner(runs_on) -> bool:
    """Whether a job's `runs-on` names a Windows runner.

    A list of labels counts as Windows when any of them does: the
    self-hosted hardware runner is `[self-hosted, windows, gpu-nvenc]`.
    """
    return any(WINDOWS_RUNNER.search(label) for label in _labels(runs_on))


def _scalar(node):
    """The value of a scalar node, or None.

    A plain Python value is passed through: the self-test builds job
    mappings by hand, and a node and a plain value are both "a scalar"
    for this guard's purposes.
    """
    if node is None:
        return None
    if isinstance(node, (str, int, float, bool)):
        return node
    return getattr(node, "value", None)


def _labels(node) -> list[str]:
    """A `runs-on` value's labels, as a list of strings.

    `runs-on` is either a single label or a list of them, and the
    self-hosted hardware runner is a list:
    `[self-hosted, windows, gpu-nvenc]`.
    """
    if node is None:
        return []
    if isinstance(node, str):
        return [node]
    if isinstance(node, (list, tuple)):
        return [str(x) for x in node]
    if isinstance(node, dict):
        return []
    value = _scalar(node)
    if value is None:
        return []
    return [str(value)]


def _mapping(node) -> dict:
    """A mapping node's keys and values.

    A plain dict is passed through: the self-test builds job mappings
    by hand, and a node and a dict are both "a mapping" for this
    guard's purposes.
    """
    if isinstance(node, dict):
        return node
    out: dict = {}
    if node is None or not hasattr(node, "value"):
        return out
    for key_node, value_node in node.value:
        key = _scalar(key_node)
        if key is not None:
            out[str(key)] = value_node
    return out


def _steps_of(job_node) -> list[tuple[str, str, str | None, int]]:
    """The (name, source, shell, line) quadruples of a job's steps.

    The shell is the step's own `shell:`, or None when the step does not
    name one -- in which case the job's `defaults.run.shell` applies, or
    GitHub's default for the runner's OS.  The line is where the
    `run:` key sits, so a failure names a place in the file.
    """
    out: list[tuple[str, str, str | None, int]] = []
    steps_node = _mapping(job_node).get("steps")
    if steps_node is None:
        return out
    for step_node in (getattr(steps_node, "value", None) or []):
        step = _mapping(step_node)
        run_node = step.get("run")
        if run_node is None:
            continue
        run = _scalar(run_node)
        if not isinstance(run, str):
            continue
        shell = _scalar(step.get("shell"))
        if shell is not None and not isinstance(shell, str):
            shell = None
        name = _scalar(step.get("name")) or "(unnamed)"
        line = getattr(run_node, "start_mark", None)
        out.append((str(name), run, shell,
                    (line.line + 1) if line is not None else 0))
    return out


def _job_shell(job: dict, runs_on) -> str:
    """The shell a job's steps run under when the step names none."""
    defaults_node = job.get("defaults")
    if defaults_node is not None:
        run_defaults = _mapping(_mapping(defaults_node).get("run"))
        shell = _scalar(run_defaults.get("shell"))
        if isinstance(shell, str):
            return shell
    # GitHub's default: bash, except on Windows, where it is PowerShell.
    if _is_windows_runner(runs_on):
        return "pwsh"
    return "bash"

def collect(directory: Path) -> list[tuple[str, str, int]]:
    """The (label, script, line) triples of every PowerShell step.

    The label names the workflow, the job and the step, so a failure
    says which one it is.
    """
    if yaml is None:
        raise SystemExit("PyYAML is required by this check")

    out: list[tuple[str, str, int]] = []
    paths = sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml"))
    for path in paths:
        text = path.read_text(encoding="utf-8")
        try:
            root = yaml.compose(text)
        except yaml.YAMLError as exc:
            out.append((f"{path.name}: the workflow does not parse as YAML",
                        str(exc), 0))
            continue
        jobs = _mapping(_mapping(root).get("jobs"))
        for job_name, job_node in jobs.items():
            job = _mapping(job_node)
            runs_on = job.get("runs-on")
            default_shell = _job_shell(job, runs_on)
            for name, body, shell, line in _steps_of(job_node):
                effective = shell or default_shell
                if effective not in POWERSHELL_SHELLS:
                    continue
                masked = EXPRESSION.sub("GITHUB_EXPRESSION", body)
                out.append((f"{path.name}: job '{job_name}', step '{name}'",
                            masked, line))
    return out


def parse_all(exe: str, scripts: list[tuple[str, str, int]]) -> list[str]:
    """The parser's complaints about every script, in one invocation.

    One PowerShell process for the whole tree: the parser is fast, but
    starting a process per step costs more than the parse.
    """
    problems: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        names: list[str] = []
        for n, (label, body, _line) in enumerate(scripts):
            path = tmpdir / ("step%03d.ps1" % n)
            path.write_text(body, encoding="utf-8")
            names.append(str(path).replace("\\", "\\\\").replace("'", "''"))
        # Each script is parsed in turn; the results come back as
        # "<index>|<message>" lines so the two sides can be matched up.
        # The parser's messages follow the thread's UI culture, and this
        # machine's is Chinese -- a CI log that says what is wrong in a
        # language the reader may not have is a log that has to be
        # translated by hand, so the culture is pinned to English.
        script = (
            "[System.Threading.Thread]::CurrentThread."
            "CurrentUICulture = 'en-US'\n"
            "$files = @(%s)\n"
            "foreach ($f in $files) {\n"
            "  $e = $null; $t = $null\n"
            "  [System.Management.Automation.Language.Parser]::"
            "ParseFile($f, [ref]$t, [ref]$e) | Out-Null\n"
            "  if ($e.Count) {\n"
            "    foreach ($err in $e) {\n"
            "      Write-Output ('{0}|{1}|{2}' -f "
            "[array]::IndexOf($files, $f), $err.Extent.StartLineNumber, "
            "$err.Message)\n"
            "    }\n"
            "  }\n"
            "}\n" % ", ".join("'%s'" % n for n in names))
        proc = subprocess.run([exe, "-NoProfile", "-Command", script],
                              capture_output=True, timeout=600)
        out = proc.stdout.decode("utf-8", "replace")
        for line in out.split("\n"):
            line = line.strip()
            if not line or "|" not in line:
                continue
            index, lineno, message = line.split("|", 2)
            try:
                label, body, base = scripts[int(index)]
            except (ValueError, IndexError):
                continue
            problems.append(f"{label}: line {base + int(lineno)}: {message}")
    return problems


def self_test() -> int:
    failures: list[str] = []

    def expect(cond: bool, what: str) -> None:
        print(("PASS  " if cond else "FAIL  ") + what)
        if not cond:
            failures.append(what)

    if yaml is None:
        print("SKIP  PyYAML is not installed -- the self-test needs it")
        return 0

    exe = _powershell_parser()
    if exe is None:
        print("SKIP  no PowerShell on this machine -- the self-test "
              "needs the parser")
        return 0

    # 1. The exact line that broke the hardware job.
    bad = 'Write-Host "mirror $m: not a clone"'
    expect(bool(parse_all(exe, [("t", bad, 0)])),
           "the line that broke the hardware job is rejected")

    # 2. The form it was replaced with parses.
    good = 'Write-Host ("mirror {0}: not a clone" -f $m)'
    expect(not parse_all(exe, [("t", good, 0)]),
           "the format-string form is accepted")

    # 3. A `${{ }}` expression, masked, parses -- and the unmasked form
    #    does not, which is why the masking is there.
    expect(bool(parse_all(exe, [("t", '$a = "${{ github.sha }}"', 0)])),
           "an unmasked `${{ }}` expression does not parse (why it is masked)")
    expect(not parse_all(exe, [("t", EXPRESSION.sub("GITHUB_EXPRESSION",
                                                     '$a = "${{ github.sha }}"'),
                                0)]),
           "the masked expression parses")

    # 4. An ordinary script parses.
    expect(not parse_all(exe, [("t", "Write-Host 'ok'", 0)]),
           "an ordinary script parses")

    # 5. The runner OS decides the default shell: a Linux job's steps
    #    are bash and are not parsed as PowerShell.
    expect(_job_shell({}, "ubuntu-22.04") == "bash",
           "a Linux job defaults to bash")
    expect(_job_shell({}, "windows-2022") == "pwsh",
           "a Windows job defaults to PowerShell")
    expect(_job_shell({}, ["self-hosted", "windows", "gpu-nvenc"]) == "pwsh",
           "the self-hosted Windows runner defaults to PowerShell")
    expect(_job_shell({"defaults": {"run": {"shell": "powershell"}}},
                       "ubuntu-22.04") == "powershell",
           "a job's defaults.run.shell wins")

    # 6. Every workflow in the tree passes.
    problems = parse_all(exe, collect(WORKFLOWS))
    for p in problems:
        print("        " + p)
    expect(not problems, "every workflow in .github/workflows passes")

    if failures:
        print(f"\n{len(failures)} self-test failure(s).")
        return 1
    print("\nPASS -- the self-test confirms every classification.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--dir", default=str(WORKFLOWS),
                        help="directory of workflows (default: .github/workflows)")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    directory = Path(args.dir)
    if not directory.is_dir():
        print(f"error: {directory} is not a directory", file=sys.stderr)
        return 2

    if yaml is None:
        # PyYAML is not part of the standard library.  The CI workflow
        # installs it explicitly and runs this check as its own step,
        # where a missing PyYAML IS a failure: there, the interpreter is
        # supposed to have it.
        print("SKIP -- PyYAML is not installed; the PowerShell guard "
              "cannot run.")
        return 0

    exe = _powershell_parser()
    if exe is None:
        print("SKIP -- no PowerShell on this machine, so the PowerShell "
              "steps cannot be parsed here.  The check runs on the "
              "Windows runner.")
        return 0

    scripts = collect(directory)
    problems = parse_all(exe, scripts)
    for p in problems:
        print("FAIL  " + p)
    if problems:
        return 1
    print(f"PASS -- every PowerShell step in {len(scripts)} step(s) parses")
    return 0


if __name__ == "__main__":
    sys.exit(main())

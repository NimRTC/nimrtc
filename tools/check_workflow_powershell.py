#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Check that every PowerShell block in the workflows *parses*.

Why this exists
---------------
A workflow's `run:` block is not checked by anything.  GitHub writes it
to a `.ps1` file and runs it; if the file does not parse, the step
dies with a `ParserError` -- and the error says nothing about what the
step was trying to do:

    At ...ps1:33 char:24
    +     Write-Host "mirror $m: not a clone"
    +                        ~~~
    Variable reference is not valid. ':' was not followed by a valid
    variable name character. Consider using ${} to delimit the name.

That is a *parse* error, so it kills the whole script before a single
command runs -- and the workflow has no way to say so in a pull
request.  The hardware job sat on exactly that failure: `$m:` reads
as a drive reference to PowerShell, because the letter after the
colon is a drive name.

So the guard is a parse, done locally, on every pull request.  A
script that PowerShell cannot parse is a step that will never run,
and the cost of finding that out is one command instead of a
twenty-minute self-hosted run.

What it checks
--------------
Every `run: |` block in every workflow, when the block's shell is
PowerShell, is written to a temporary file and handed to the
PowerShell parser.  The GitHub expressions (`${{ ... }}`) are replaced
with a placeholder first, because they are not PowerShell -- the
workflow substitutes them before the file is written.

The parser is PowerShell's own, so this check cannot disagree with
the runner about what is valid.

Usage
-----
    python tools/check_workflow_powershell.py

    # a self-test that proves the checker can tell a good block from
    # a bad one
    python tools/check_workflow_powershell.py --self-test
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WORKFLOW_DIR = REPO_ROOT / ".github" / "workflows"

# A `run:` block, with its body.  The body is the lines that are
# indented *more* than the `run:` key itself.  The body is matched
# line by line rather than with one regex, because a block's body
# ends at the first line that is *not* more indented -- and that
# line can be the next step's `run:` key.
RUN_KEY = re.compile(r"^([ \t]*)run:[ \t]*\|[ \t]*$", re.MULTILINE)

# A GitHub expression.  It is substituted before the script reaches
# the shell, so it is not PowerShell and must not be parsed as such.
EXPRESSION = re.compile(r"\$\{\{.*?\}\}")

# The shells whose blocks this checker parses.  A `bash` block is not
# PowerShell, and PowerShell's parser would reject perfectly good
# bash.
POWERSHELL_SHELLS = ("powershell", "pwsh")


def extract_run_blocks(text: str) -> list[tuple[str, str]]:
    """Every `run:` block in a workflow, as (shell, body).

    The shell is read from the step's own `shell:` key when there is
    one.  A step without a `shell:` runs in `bash` on a Linux runner
    and in `pwsh` on a Windows one; the default is `bash`, and a `run:`
    block under a Windows `defaults:` is rare enough that the
    conservative answer is the right one.
    """
    blocks: list[tuple[str, str]] = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = RUN_KEY.match(line)
        if not match:
            continue
        key_indent = len(match.group(1))

        # The body is every following line that is indented *more*
        # than the `run:` key.  The first line that is not is the end
        # of the block -- it is the next step's key.
        body_lines: list[str] = []
        for body_line in lines[index + 1:]:
            if not body_line.strip():
                body_lines.append("")
                continue
            indent = len(body_line) - len(body_line.lstrip())
            if indent <= key_indent:
                break
            body_lines.append(body_line[key_indent + 2:])
        body = "\n".join(body_lines).rstrip() + "\n"

        # The `shell:` key sits on the same step, above or below the
        # `run:` key.
        shell = _shell_for(lines, index, key_indent)
        blocks.append((shell, body))
    return blocks


def _shell_for(lines: list[str], run_index: int, key_indent: int) -> str:
    """The `shell:` of the step that owns the `run:` at `run_index`."""
    shell = ""
    # Look up first -- a `shell:` key usually sits above the `run:`.
    for line in lines[:run_index][::-1]:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if indent < key_indent:
            break
        if indent > key_indent:
            continue
        if stripped.startswith("shell:"):
            shell = stripped.split(":", 1)[1].strip().strip("'\"")
            break
        if stripped.startswith("- ") and not stripped.startswith("- name:"):
            # The start of another step.
            break
    if shell:
        return shell

    # Then down -- `shell:` may follow the `run:` key.
    for line in lines[run_index + 1:]:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if indent < key_indent:
            break
        if indent > key_indent:
            continue
        if stripped.startswith("shell:"):
            return stripped.split(":", 1)[1].strip().strip("'\"")
        if stripped.startswith("- ") and not stripped.startswith("- name:"):
            break
    return shell or "bash"


def parse_with_powershell(body: str) -> list[str]:
    """The parse errors PowerShell reports for `body`, if any."""
    script = EXPRESSION.sub("PLACEHOLDER", body)
    handle, path = tempfile.mkstemp(suffix=".ps1")
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\r\n") as f:
            f.write(script)
        # The parser is asked directly, so the answer is the parser's
        # and not a runner's.
        command = (
            "$errors = $null; "
            "[System.Management.Automation.Language.Parser]::ParseFile("
            "%r, [ref]$null, [ref]$errors) | Out-Null; "
            "if ($errors) { $errors | ForEach-Object { $_.Message } }"
            % path
        )
        proc = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True, text=True, errors="replace")
        output = (proc.stdout or "") + (proc.stderr or "")
        messages = []
        for line in output.splitlines():
            line = line.strip()
            if not line:
                continue
            if line.startswith("+") or line.startswith("At "):
                # The context lines PowerShell prints around an error.
                continue
            if line.startswith("CategoryInfo") or line.startswith("FullyQualifiedErrorId"):
                continue
            messages.append(line)
        return messages
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def powershell_available() -> bool:
    """Is there a PowerShell to ask?"""
    try:
        subprocess.run(["powershell", "-NoProfile", "-Command", "$null"],
                       capture_output=True, timeout=60)
        return True
    except (OSError, subprocess.SubprocessError):
        return False


def check() -> int:
    """Parse every PowerShell block in every workflow."""
    if not powershell_available():
        print("check_workflow_powershell: no PowerShell on this machine; "
              "the check is skipped")
        return 0

    workflows = sorted(WORKFLOW_DIR.glob("*.yml"))
    if not workflows:
        print("FAIL: no workflow found in %s" % WORKFLOW_DIR)
        return 1

    failures: list[str] = []
    checked = 0
    for path in workflows:
        text = path.read_text(encoding="utf-8")
        for index, (shell, body) in enumerate(extract_run_blocks(text)):
            if shell not in POWERSHELL_SHELLS:
                continue
            checked += 1
            messages = parse_with_powershell(body)
            if messages:
                failures.append("%s (run block %d, shell=%s):\n  %s"
                                % (path.name, index + 1, shell,
                                   "\n  ".join(messages)))

    if failures:
        print("FAIL -- %d PowerShell block(s) do not parse:" % len(failures))
        for failure in failures:
            print("  - %s" % failure)
        return 1
    print("PASS -- %d PowerShell block(s) in %d workflow(s) parse."
          % (checked, len(workflows)))
    return 0


# --------------------------------------------------------------------------
# The self-test
# --------------------------------------------------------------------------

def self_test() -> int:
    """Prove the checker tells a good block from a bad one."""
    failures: list[str] = []

    def check_that(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    # A block that parses.
    good = (
        "$ref = \"abc\"\n"
        "$url = \"https://github.com/x/y.git\"\n"
        "foreach ($m in @(\"a\", \"b\")) {\n"
        "    Write-Host \"mirror ${m}: not a clone\"\n"
        "}\n"
    )
    check_that(not parse_with_powershell(good),
               "a block with ${m} must parse")

    # The bug that killed the hardware job: `$m:` is a drive
    # reference to PowerShell.
    bad = (
        "$m = \"D:\\MyOpen\\NimRTC\"\n"
        "Write-Host \"mirror $m: not a clone\"\n"
    )
    check_that(parse_with_powershell(bad),
               "a block with `$m:` must not parse")

    # The GitHub expressions are not PowerShell and must not be
    # parsed as such.
    with_expr = (
        "$ref = \"${{ inputs.ref || github.sha }}\"\n"
        "Write-Host $ref\n"
    )
    check_that(not parse_with_powershell(with_expr),
               "a block with a GitHub expression must parse")

    # The extractor has to find the blocks and read the shell.
    sample = (
        "jobs:\n"
        "  a:\n"
        "    steps:\n"
        "      - name: one\n"
        "        shell: powershell\n"
        "        run: |\n"
        "          Write-Host \"hi\"\n"
        "          Write-Host \"there\"\n"
        "      - name: two\n"
        "        shell: bash\n"
        "        run: |\n"
        "          echo hi\n"
    )
    blocks = extract_run_blocks(sample)
    check_that(len(blocks) == 2,
               "both run blocks must be found (found %d)" % len(blocks))
    if len(blocks) == 2:
        check_that(blocks[0][0] == "powershell",
                   "the first block's shell must be powershell")
        check_that("Write-Host \"there\"" in blocks[0][1],
                   "the first block's body must be complete")
        check_that(blocks[1][0] == "bash",
                   "the second block's shell must be bash")

    if failures:
        print("FAIL -- the self-test caught a misclassification:")
        for failure in failures:
            print("  - %s" % failure)
        return 1
    print("PASS -- the self-test confirms every classification.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return check()


if __name__ == "__main__":
    sys.exit(main())

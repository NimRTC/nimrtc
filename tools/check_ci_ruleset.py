#!/usr/bin/env python3
"""check_ci_ruleset.py -- keep the merge gate and the CI matrix in step.

The repository's merge gate is a GitHub Ruleset (`main`), and it
demands a list of required status checks.  That list and the set of
checks the CI workflows actually produce are two things that drift
apart, and the drift is invisible until it blocks a merge.

Two ways to drift, both observed in this repository:

  1. **A required check that no workflow produces.**  The gate then
     blocks every merge, forever, with nothing to fix from the PR
     page.  The `windows / fast` check is exactly this trap: it comes
     from `ci-fast.yml`, which deliberately does *not* run on `main`
     (`branches-ignore: [main, 'release/**']`), so requiring it would
     deadlock the repository.

  2. **A workflow that produces a check the gate does not require.**
     This is the quieter failure: the check runs, it goes green, it
     costs eleven minutes of runner time, and nobody looks at it.  A
     sanitizer leg that is not required catches nothing.

This script reads the workflows and the ruleset, and asserts that the
two sets are *equal*.  It needs no build and no network, so it runs on
every push.

What counts as "produced on main"
----------------------------------

A check only counts when the workflow that produces it runs on both

  * a push to `main`, and
  * a pull request whose base is `main`.

The second half is the one that is easy to miss.  `pull_request:
branches: [main]` filters on the PR's *base* branch, so a workflow
that omits it never runs for a PR into `main` -- no matter what it
does on push.

A workflow whose push trigger carries a `paths:` filter is excluded:
it only runs when particular files change, so its check does not
report on every commit and cannot be required.

Usage:
    python tools/check_ci_ruleset.py [--repo-root ROOT] [--verbose]
    python tools/check_ci_ruleset.py --self-test

Exit code 0 when the gate and the matrix agree, 1 otherwise.

`--self-test` builds a synthetic workflow tree and asserts that the
checker still distinguishes each shape, so the guard cannot silently
degrade into a no-op.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None

# The ruleset that guards `main`, and the file it is mirrored in.
DEFAULT_RULESET = ".github/rulesets/protect-main.json"

# The branch the gate guards.  A pull request into this branch is what
# the gate evaluates.
GUARDED_BRANCH = "main"

EXPR_RE = re.compile(r"\$\{\{\s*matrix\.([A-Za-z_][A-Za-z0-9_]*)\s*\}\}")


# ---------------------------------------------------------------------------
# The ruleset file
# ---------------------------------------------------------------------------

def load_ruleset(path: Path) -> dict:
    """Read the ruleset mirror and return its required check contexts."""
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    for rule in data.get("rules", []):
        if rule.get("type") == "required_status_checks":
            return {
                "contexts": [c["context"] for c in
                             rule["parameters"]["required_status_checks"]],
                "strict": rule["parameters"].get(
                    "strict_required_status_checks_policy", False),
                "refs": data.get("conditions", {}).get(
                    "ref_name", {}).get("include", []),
                "rules": [r.get("type") for r in data.get("rules", [])],
            }
    raise SystemExit("%s has no required_status_checks rule" % path)


# ---------------------------------------------------------------------------
# The workflows
# ---------------------------------------------------------------------------

def _branch_list(node) -> list:
    """The branch list a trigger node names, whatever shape it has."""
    if node is None:
        return []
    if isinstance(node, str):
        return [node]
    if isinstance(node, list):
        return [str(x) for x in node]
    if isinstance(node, dict):
        return [str(x) for x in node]
    return []


def workflow_push_branches(wf: dict) -> tuple:
    """(branches, has_paths_filter) for the `push` trigger.

    `branches-ignore` inverts the filter, so it is reported separately
    by the caller as "runs everywhere except these".
    """
    on = wf.get("on", wf.get(True, {}))
    push = (on or {}).get("push") or {}
    branches = _branch_list(push.get("branches"))
    has_paths = "paths" in push or "paths-ignore" in push
    return branches, has_paths


def workflow_push_ignore(wf: dict) -> list:
    on = wf.get("on", wf.get(True, {}))
    push = (on or {}).get("push") or {}
    return _branch_list(push.get("branches-ignore"))


def workflow_pr_branches(wf: dict) -> list:
    on = wf.get("on", wf.get(True, {}))
    pull_request = (on or {}).get("pull_request") or {}
    return _branch_list(pull_request.get("branches"))


def expand_matrix(name: str, job: dict) -> list:
    """The check names a job produces.

    A matrix job produces one check per matrix combination, with the
    matrix expression substituted -- `linux-sanitizers / ${{
    matrix.preset }}` becomes `linux-sanitizers / asan-ubsan` and
    `linux-sanitizers / tsan.clang`.  A job without a matrix produces
    exactly its own name.
    """
    if "${{" not in name:
        return [name]
    matrix = ((job.get("strategy") or {}).get("matrix")) or {}
    include = matrix.get("include") or []
    if not include:
        # A matrix without `include` (a plain list of values) cannot be
        # expanded without knowing the axis names; fall back to the
        # unresolved name so the mismatch is reported rather than
        # silently accepted.
        return [name]
    out = []
    for entry in include:
        if not isinstance(entry, dict):
            continue
        rendered = name
        for key, value in entry.items():
            rendered = rendered.replace("${{ matrix.%s }}" % key, str(value))
            rendered = rendered.replace("${{matrix.%s}}" % key, str(value))
        if EXPR_RE.search(rendered):
            # An axis the `include` entry does not carry: still
            # unresolved, so the name cannot be matched.
            return [name]
        out.append(rendered)
    return out


def collect_checks(wf_dir: Path) -> dict:
    """Every check the workflows produce, and where each one runs.

    Returns {check_name: {"workflows": [...], "on_main_push": bool,
    "on_pr_to_main": bool, "path_filtered": bool}}.
    """
    info = {}
    for path in sorted(wf_dir.glob("*.y*ml")):
        if yaml is None:
            raise SystemExit("PyYAML is required by this check")
        with path.open(encoding="utf-8") as handle:
            wf = yaml.safe_load(handle)
        if not isinstance(wf, dict):
            continue
        jobs = wf.get("jobs") or {}
        if not isinstance(jobs, dict):
            continue

        push_branches, path_filtered = workflow_push_branches(wf)
        push_ignore = workflow_push_ignore(wf)
        pr_branches = workflow_pr_branches(wf)

        # Does this workflow run on a push to the guarded branch?
        if push_branches:
            on_main_push = GUARDED_BRANCH in push_branches
        elif push_ignore:
            on_main_push = GUARDED_BRANCH not in push_ignore
        else:
            # No branch filter at all: the workflow runs on every push.
            on_main_push = True

        # Does it run for a pull request into the guarded branch?
        on_pr_to_main = GUARDED_BRANCH in pr_branches

        for job in jobs.values():
            if not isinstance(job, dict):
                continue
            raw_name = job.get("name")
            if not raw_name:
                # An unnamed job reports under the job's key.
                continue
            for check in expand_matrix(str(raw_name), job):
                entry = info.setdefault(check, {
                    "workflows": [], "on_main_push": False,
                    "on_pr_to_main": False, "path_filtered": False,
                    "unresolved": "${{" in check,
                })
                entry["workflows"].append(path.name)
                entry["on_main_push"] |= on_main_push
                entry["on_pr_to_main"] |= on_pr_to_main
                entry["path_filtered"] |= path_filtered
    return info


def guaranteed_checks(info: dict) -> set:
    """The checks that are produced on every push to main and every PR
    into main -- the ones the gate can require."""
    return {name for name, e in info.items()
            if e["on_main_push"] and e["on_pr_to_main"]
            and not e["path_filtered"] and not e["unresolved"]}


# ---------------------------------------------------------------------------
# The check
# ---------------------------------------------------------------------------

def run_check(repo_root: Path, ruleset_path: Path, verbose: bool) -> int:
    ruleset = load_ruleset(ruleset_path)
    required = set(ruleset["contexts"])
    info = collect_checks(repo_root / ".github" / "workflows")
    guaranteed = guaranteed_checks(info)

    problems = []

    # 1. The gate must not require a check that nothing produces.
    for ctx in sorted(required - guaranteed):
        e = info.get(ctx)
        if e is None:
            problems.append(
                "required check %r is produced by no workflow at all" % ctx)
        else:
            why = []
            if not e["on_main_push"]:
                why.append("does not run on a push to %s" % GUARDED_BRANCH)
            if not e["on_pr_to_main"]:
                why.append("does not run for a PR into %s" % GUARDED_BRANCH)
            if e["path_filtered"]:
                why.append("is path-filtered, so it does not report on "
                           "every commit")
            if e["unresolved"]:
                why.append("has an unresolved matrix expression")
            problems.append("required check %r %s" % (ctx, "; ".join(why)))

    # 2. The gate must not ignore a check that the matrix produces.
    for ctx in sorted(guaranteed - required):
        e = info[ctx]
        problems.append(
            "check %r (%s) is produced on %s but is not required by the gate"
            % (ctx, ", ".join(sorted(set(e["workflows"]))), GUARDED_BRANCH))

    # 3. The ruleset must actually guard the branch the gate claims.
    refs = ruleset["refs"]
    expected_ref = "refs/heads/" + GUARDED_BRANCH
    if expected_ref not in refs:
        problems.append("the ruleset does not cover %s (it covers %s)"
                        % (expected_ref, ", ".join(refs) or "nothing"))

    # 4. The gate must be strict, or a PR can merge on the checks of a
    #    commit that is no longer an ancestor of main.
    if not ruleset["strict"]:
        problems.append("the ruleset is not strict: a PR can merge on the "
                        "checks of a commit that is no longer an ancestor "
                        "of %s" % GUARDED_BRANCH)

    if verbose:
        print("workflows:")
        for name, e in sorted(info.items()):
            print("  %-36s push=%-5s pr=%-5s paths=%-5s  %s"
                  % (name, e["on_main_push"], e["on_pr_to_main"],
                     e["path_filtered"], ",".join(sorted(set(e["workflows"])))))
        print("required by the gate: %d" % len(required))
        print("guaranteed by the matrix: %d" % len(guaranteed))

    if problems:
        print("FAIL -- the merge gate and the CI matrix disagree:")
        for p in problems:
            print("  -", p)
        return 1

    print("PASS -- the gate requires exactly the %d checks the CI matrix "
          "produces on %s." % (len(required), GUARDED_BRANCH))
    return 0


# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------

SELF_TEST_WORKFLOWS = {
    "ci.yml": """
name: ci
on:
  push:
    branches: [main]
  pull_request:
    branches: [main]
jobs:
  a:
    name: alpha / one
    steps: []
  b:
    name: beta / ${{ matrix.preset }}
    strategy:
      matrix:
        include:
          - preset: p1
          - preset: p2
    steps: []
""",
    "ci-fast.yml": """
name: ci-fast
on:
  push:
    branches-ignore:
      - main
  pull_request:
    branches: [main]
jobs:
  a:
    name: fast / one
    steps: []
""",
    "pathful.yml": """
name: pathful
on:
  push:
    branches: [main]
    paths:
      - 'docs/**'
  pull_request:
    branches: [main]
jobs:
  a:
    name: pathful / one
    steps: []
""",
    "otherbase.yml": """
name: otherbase
on:
  push:
    branches: [main]
  pull_request:
    branches: [release/**]
jobs:
  a:
    name: otherbase / one
    steps: []
""",
}


def self_test(tmp_root: Path) -> int:
    wf_dir = tmp_root / ".github" / "workflows"
    wf_dir.mkdir(parents=True, exist_ok=True)
    for name, body in SELF_TEST_WORKFLOWS.items():
        (wf_dir / name).write_text(body, encoding="utf-8")

    info = collect_checks(wf_dir)
    guaranteed = guaranteed_checks(info)

    failures = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    # A plain job on a workflow that runs on main and on PRs into main.
    expect("alpha / one" in guaranteed,
           "a plain job on a main+PR workflow must be guaranteed")

    # A matrix job expands into one check per combination.
    expect("beta / p1" in guaranteed and "beta / p2" in guaranteed,
           "a matrix job must expand into one check per combination")
    expect("beta / ${{ matrix.preset }}" not in guaranteed,
           "the unresolved matrix name must not count as a check")

    # `branches-ignore: [main]` means the workflow does NOT run on main.
    expect("fast / one" not in guaranteed,
           "a workflow that skips main must not be guaranteed on main")

    # A path-filtered workflow does not report on every commit.
    expect("pathful / one" not in guaranteed,
           "a path-filtered workflow must not be guaranteed")

    # A PR trigger that omits `main` means no PR into main gets the job.
    expect("otherbase / one" not in guaranteed,
           "a workflow whose PR trigger omits main must not be guaranteed")

    if failures:
        print("FAIL -- the self-test caught a misclassification:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS -- the self-test confirms every classification.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=None,
                        help="the repository root (default: inferred)")
    parser.add_argument("--ruleset", default=None,
                        help="the ruleset mirror (default: <root>/%s)"
                             % DEFAULT_RULESET)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            return self_test(Path(tmp))

    root = Path(args.repo_root) if args.repo_root else Path(__file__).resolve().parent.parent
    ruleset_path = Path(args.ruleset) if args.ruleset else root / DEFAULT_RULESET
    if not ruleset_path.exists():
        print("FAIL -- %s does not exist" % ruleset_path)
        return 1
    return run_check(root, ruleset_path, args.verbose)


if __name__ == "__main__":
    sys.exit(main())

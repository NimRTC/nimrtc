#!/usr/bin/env python3
"""apply_ci_ruleset.py -- apply the ruleset mirror to the GitHub repository.

The merge gate lives in two places: the GitHub Ruleset in the
repository settings, and `.github/rulesets/protect-main.json` in the
tree.  The mirror is the reviewable one -- a change to the gate is a
pull request -- and this script is what puts it into effect.

The script is deliberately two-phase:

  * `--check` (the default) compares the mirror with what the
    repository actually has, and reports the difference.  It needs
    read access only.
  * `--apply` writes the mirror to the repository.  It needs write
    access and it says so before it does anything.

The comparison ignores the fields GitHub fills in by itself (the
ruleset id, the timestamps, the `_links`), and compares the rest
exactly, so a difference means a real difference.

Usage:
    python tools/apply_ci_ruleset.py --check
    python tools/apply_ci_ruleset.py --apply

Exit code 0 when the repository matches the mirror, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = "NimRTC/nimrtc"
API = "repos/" + REPO
DEFAULT_MIRROR = ".github/rulesets/protect-main.json"

# The fields GitHub manages itself.  They are not part of the intent,
# so a difference in them is not a difference in the gate.
IGNORED = {"id", "node_id", "created_at", "updated_at", "_links",
           "source", "source_type", "created_by",
           "current_user_can_manage", "ruleset_source",
           "ruleset_source_type"}


def gh(args, payload=None):
    cmd = ["gh", "api"] + args
    if payload is not None:
        cmd += ["--input", "-"]
    r = subprocess.run(cmd, input=payload, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return r.stdout, r.stderr, r.returncode


def strip(node):
    """Drop the fields GitHub manages, recursively."""
    if isinstance(node, dict):
        return {k: strip(v) for k, v in node.items()
                if k not in IGNORED and not k.startswith("_")}
    if isinstance(node, list):
        return [strip(v) for v in node]
    return node


def load_mirror(path: Path) -> dict:
    """Read the mirror and drop its own `_comment` documentation keys."""
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)

    def clean(node):
        if isinstance(node, dict):
            return {k: clean(v) for k, v in node.items()
                    if k != "_comment"}
        if isinstance(node, list):
            return [clean(v) for v in node]
        return node

    return clean(data)


def find_ruleset(rulesets: list, name: str):
    for rs in rulesets:
        if rs.get("name") == name:
            return rs
    return None


def find_by_id(rulesets: list, wanted_id):
    for rs in rulesets:
        if str(rs.get("id")) == str(wanted_id):
            return rs
    return None


FIELDS = ("name", "target", "enforcement", "bypass_actors",
          "conditions", "rules")


def read_ruleset(ruleset_id) -> dict:
    out, err, rc = gh([API + "/rulesets/" + str(ruleset_id)])
    if rc != 0:
        raise SystemExit("ERROR reading the ruleset: " + err.strip()[:400])
    data = json.loads(out)
    return strip({k: data.get(k) for k in FIELDS})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=None)
    parser.add_argument("--mirror", default=None,
                        help="the ruleset mirror (default: <root>/%s)"
                             % DEFAULT_MIRROR)
    parser.add_argument("--check", action="store_true",
                        help="compare only (the default)")
    parser.add_argument("--apply", action="store_true",
                        help="write the mirror to the repository")
    args = parser.parse_args()

    root = (Path(args.repo_root) if args.repo_root
            else Path(__file__).resolve().parent.parent)
    mirror_path = Path(args.mirror) if args.mirror else root / DEFAULT_MIRROR
    if not mirror_path.exists():
        print("FAIL -- %s does not exist" % mirror_path)
        return 1
    mirror = load_mirror(mirror_path)

    out, err, rc = gh([API + "/rulesets"])
    if rc != 0:
        print("ERROR reading the rulesets:", err.strip()[:400])
        print("A GitHub token with read access is needed: `gh auth login`.")
        return 1
    rulesets = json.loads(out)

    # The ruleset is found by its id when the file names one, else by
    # its name.  The id is stable across renames; the name is not.
    wanted_id = mirror.get("id")
    rs = (find_by_id(rulesets, wanted_id) if wanted_id else None) \
        or find_ruleset(rulesets, mirror["name"])
    if rs is None:
        print("ERROR -- no ruleset named %r in %s" % (mirror["name"], REPO))
        print("Available:")
        for candidate in rulesets:
            print("   id=%s name=%r enforcement=%s"
                  % (candidate.get("id"), candidate.get("name"),
                     candidate.get("enforcement")))
        return 1

    print("ruleset: id=%s name=%r enforcement=%s"
          % (rs.get("id"), rs.get("name"), rs.get("enforcement")))
    print("mirror:  %s" % mirror_path)

    if args.apply:
        payload = json.dumps({
            "name": mirror["name"],
            "target": mirror.get("target", "branch"),
            "enforcement": mirror.get("enforcement", "active"),
            "bypass_actors": mirror.get("bypass_actors", []),
            "conditions": mirror["conditions"],
            "rules": mirror["rules"],
        })
        out, err, rc = gh(["--method", "PUT",
                            API + "/rulesets/" + str(rs["id"])],
                           payload=payload)
        if rc != 0:
            print("ERROR applying the ruleset:", err.strip()[:600])
            return 1
        print("OK -- the ruleset was updated.")

    # Re-read and compare, so the answer is about what the repository
    # actually holds rather than about what was sent.
    live = read_ruleset(rs["id"])

    if live == mirror:
        print("PASS -- the repository matches the mirror.")
        return 0

    print("FAIL -- the repository differs from the mirror:")
    print("--- repository ---")
    print(json.dumps(live, indent=2, sort_keys=True))
    print("--- mirror ---")
    print(json.dumps(mirror, indent=2, sort_keys=True))
    if not args.apply:
        print("Run with --apply to make the repository match.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

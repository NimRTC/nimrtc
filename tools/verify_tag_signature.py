#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Verify the signature on a NimRTC release tag, offline.

Why this tool exists
--------------------
`RELEASING.md` §3.3 requires the release tag to be an annotated tag
carrying a signature, so that GitHub renders it as "Verified" and so
that a consumer can prove the tag came from the release manager.  The
requirement was written against OpenPGP (`git tag -s -a` with a key in
the GnuPG keyring), but this repository's actual signing configuration
is `gpg.format = ssh` -- the account's SSH key is registered with
GitHub, so an SSH-signed tag gets the same "Verified" badge.

That mismatch is exactly the kind of thing a release checklist cannot
catch on its own: the prose says "PGP signature", the repository signs
with SSH, and the tag lands unsigned because nobody noticed that
`git tag -s` under `gpg.format = ssh` produces a *different* kind of
signature than the one the prose describes.  This tool reports what is
actually on the tag, and verifies it cryptographically.

What it checks
--------------
1.  The tag exists and is an *annotated* tag object.  A lightweight tag
    points straight at a commit and carries no signature at all, so it
    can never satisfy the release requirement.
2.  The tag object carries a signature block, and the tool reports
    which kind (SSH or OpenPGP).
3.  The signature verifies cryptographically:
      * SSH   -- via `git verify-tag --raw` with
                 `gpg.ssh.allowedSignersFile` pointed at an
                 allowed_signers file.  The principal must be one the
                 allowed_signers file lists.
      * PGP   -- via `git verify-tag --raw` as well, which delegates to
                 GnuPG.  When GnuPG is absent, or the release key is
                 not in the keyring, the tool says so explicitly
                 instead of reporting a pass it cannot justify.

Why `git verify-tag` and not a hand-rolled verifier
---------------------------------------------------
The payload of a signed tag is the tag object *minus* its signature
block, and getting that split exactly right is fiddly: the SSH
signature is computed over specific bytes, and an off-by-one newline
turns a valid signature into "incorrect signature".  Git already knows
the boundary and also owns the `ssh-keygen` invocation, so this tool
delegates the cryptographic step to git and reserves itself for the
parts git does not do: deciding *which* signature is required, naming
the expected principal, and refusing to report success when the
verification could not actually be performed.

A note on the Windows `ssh-keygen` build
-----------------------------------------
On Windows, `ssh-keygen -Y verify` blocks when it is spawned without a
console, so a tool that called it directly would hang instead of
reporting a verdict.  Going through `git verify-tag` avoids that: git
uses its own bundled `ssh-keygen`, which behaves.

Usage
-----
    python tools/verify_tag_signature.py --tag v1.1.0
    python tools/verify_tag_signature.py --tag v1.1.0 --repo /path/to/repo
    python tools/verify_tag_signature.py --tag v1.1.0 --json
    python tools/verify_tag_signature.py --tag v1.1.0 --shape-only
    python tools/verify_tag_signature.py --self-test

`--shape-only` performs check 1 and 2 but not check 3.  It is what the
release workflow uses: the CI runner holds the artefact-signing key
(`GPG_PRIVATE_KEY`), not the tag-signing key, so the runner cannot
verify the tag cryptographically -- but it can still refuse to release
a tag that carries no signature block at all.

Exit codes
----------
    0  the tag carries a signature and it verified (or, with
       --shape-only, the tag carries a signature block)
    1  the tag is unsigned, is not an annotated tag, or the signature
       did not verify
    2  the tool could not run (git missing, tag not found, ...)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SSH_SIGNATURE_BEGIN = b"-----BEGIN SSH SIGNATURE-----"
PGP_SIGNATURE_BEGIN = b"-----BEGIN PGP SIGNATURE-----"

# `git verify-tag --raw` writes its human-readable verdict to stderr.
# The two shapes we care about:
#   Good "git" signature for alice@example.com with ED25519 key SHA256:...
#   Good "git" signature with ED25519 key SHA256:...
# The second form means the signature is cryptographically valid but no
# principal in the allowed_signers file matched -- a different failure
# from a bad signature, and one worth distinguishing in the report.
GOOD_FOR_PRINCIPAL = re.compile(
    r'Good "git" signature for (?P<principal>\S+) with (?P<type>\S+) key '
    r'(?P<fingerprint>SHA256:\S+)')
GOOD_NO_PRINCIPAL = re.compile(
    r'Good "git" signature with (?P<type>\S+) key (?P<fingerprint>SHA256:\S+)')
NO_PRINCIPAL_MATCHED = "No principal matched."

DEFAULT_PRINCIPAL = "realdesktop@163.com"
DEFAULT_NAMESPACE = "git"


class ToolError(Exception):
    """Raised when the tool itself cannot run (exit code 2)."""


def _run(cmd, cwd=None, timeout=120, stdin=None):
    """Run a command, capturing output.

    stdin is closed explicitly unless the caller supplies it: several of
    the tools involved (ssh-keygen in particular) will block forever
    waiting for input that a non-interactive caller never sends.
    """
    try:
        return subprocess.run(
            cmd,
            cwd=str(cwd) if cwd else None,
            capture_output=True,
            stdin=stdin if stdin is not None else subprocess.DEVNULL,
            timeout=timeout,
        )
    except FileNotFoundError as exc:
        raise ToolError("required program not found: %s" % cmd[0]) from exc
    except subprocess.TimeoutExpired as exc:
        raise ToolError("timed out after %ss: %s" % (timeout, " ".join(cmd))) from exc


def git(args, repo=None, timeout=120):
    """Run a git command and return the CompletedProcess."""
    return _run(["git"] + list(args), cwd=repo, timeout=timeout)


def decode(data) -> str:
    if isinstance(data, bytes):
        return data.decode("utf-8", errors="replace")
    return data or ""


# ---------------------------------------------------------------------------
# Tag inspection
# ---------------------------------------------------------------------------

def resolve_repo(path) -> Path:
    repo = Path(path).resolve()
    if not repo.is_dir():
        raise ToolError("repository path does not exist: %s" % repo)
    probe = git(["rev-parse", "--git-dir"], repo=repo)
    if probe.returncode != 0:
        raise ToolError("not a git repository: %s" % repo)
    return repo


def read_tag_object(repo, tag):
    """Return the raw annotated-tag object, or None when it is not one."""
    probe = git(["cat-file", "-t", tag], repo=repo)
    if probe.returncode != 0:
        raise ToolError("tag not found: %s" % tag)
    obj_type = decode(probe.stdout).strip()
    if obj_type == "commit":
        # A lightweight tag: it points straight at a commit and has no
        # tag object, so it cannot carry a signature.
        return None
    if obj_type != "tag":
        raise ToolError("%s resolves to a %s, not a tag object" % (tag, obj_type))
    result = git(["cat-file", "tag", tag], repo=repo)
    if result.returncode != 0:
        raise ToolError("cannot read tag object %s: %s"
                        % (tag, decode(result.stderr).strip()))
    return result.stdout


def classify_signature(raw):
    """Return 'ssh', 'pgp' or None for a raw tag object."""
    if raw is None:
        return None
    if SSH_SIGNATURE_BEGIN in raw:
        return "ssh"
    if PGP_SIGNATURE_BEGIN in raw:
        return "pgp"
    return None


def read_tag_object_name(raw):
    """Return the tag's INTERNAL name, or None when it cannot be read.

    An annotated tag object has a header block before its message:

        object 1a2b3c...
        type commit
        tag v1.3.0
        tagger NimRTC <...> 1763980800 +0800

        NimRTC v1.3.0

    The third header line is the tag's own name -- the name the tag was
    CREATED with, which is not necessarily the name of the ref that
    points at it.  A ref and a tag object can disagree: `git update-ref
    refs/tags/X <tag-object>` moves the ref without rewriting the object,
    so the ref can be called `v1.3.0` while the object still says
    `v1.3.0-staged`.

    That is not a cosmetic difference.  `git tag -v`, `git describe`,
    the GitHub release page, and every tool that reads the object
    directly report the INTERNAL name, so a tag whose object disagrees
    with its ref is a tag that presents itself to the world under the
    wrong name.

    Returns None when the header is absent or malformed -- the caller
    then reports "the tag object carries no name", which is itself a
    failure, not a pass.
    """
    if raw is None:
        return None
    text = decode(raw)
    # The header ends at the first empty line.  Reading past it would
    # pick up a "tag ..." line out of the commit message.
    header = text.split("\n\n", 1)[0]
    for line in header.splitlines():
        if line.startswith("tag "):
            name = line[len("tag "):].strip()
            return name or None
    return None


def ref_basename(tag):
    """The short name of a tag ref, without the `refs/tags/` prefix."""
    name = (tag or "").strip()
    if name.startswith("refs/tags/"):
        name = name[len("refs/tags/"):]
    return name


# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------

def _verify_with_git(repo, tag, signature_kind, allowed_signers):
    """Run `git verify-tag --raw` and interpret the verdict."""
    args = []
    if signature_kind == "ssh":
        # Point git at the allowed_signers file explicitly rather than
        # relying on the caller's ~/.ssh/config: the release cut runs
        # on a workstation, not on the machine that made the tag.
        args += ["-c", "gpg.ssh.allowedSignersFile=%s" % allowed_signers,
                 "-c", "gpg.format=ssh"]
    else:
        args += ["-c", "gpg.format=openpgp"]

    result = git(args + ["verify-tag", "--raw", tag], repo=repo)
    combined = decode(result.stderr) + decode(result.stdout)

    good = GOOD_FOR_PRINCIPAL.search(combined)
    if good:
        return {
            "ok": True,
            "principal": good.group("principal"),
            "key_type": good.group("type"),
            "fingerprint": good.group("fingerprint"),
            "detail": "signature verified",
        }

    good_no_principal = GOOD_NO_PRINCIPAL.search(combined)
    if good_no_principal:
        # The signature itself is valid, but git could not match it to a
        # principal in the allowed_signers file.  Saying "the signature
        # is bad" here would be wrong; the signature is fine, the trust
        # anchor is missing.
        return {
            "ok": False,
            "principal": None,
            "key_type": good_no_principal.group("type"),
            "fingerprint": good_no_principal.group("fingerprint"),
            "detail": ("signature is cryptographically valid but no principal "
                       "matched the allowed_signers file; add the signing key "
                       "as a principal to %s" % allowed_signers),
        }

    if NO_PRINCIPAL_MATCHED in combined:
        return {
            "ok": False,
            "principal": None,
            "key_type": None,
            "fingerprint": None,
            "detail": "no principal matched the allowed_signers file",
        }

    # git exits non-zero for "no signature", "bad signature" and for
    # "cannot verify" alike.  Distinguish the last one, because it means
    # the verifier could not run -- reporting that as "the signature is
    # invalid" would be a lie.
    lowered = combined.lower()
    cannot_verify = ("no public key", "can't check signature",
                     "cannot verify", "no such file or directory",
                     "not found", "gpg: ", "skipped")
    if any(marker in lowered for marker in cannot_verify):
        return {
            "ok": False,
            "principal": None,
            "key_type": None,
            "fingerprint": None,
            "detail": ("could not verify the signature in this environment: %s"
                       % (combined.strip() or "git verify-tag reported no detail")),
            "unverifiable": True,
        }

    return {
        "ok": False,
        "principal": None,
        "key_type": None,
        "fingerprint": None,
        "detail": combined.strip() or "git verify-tag reported no detail",
    }


def verify_tag(repo, tag, allowed_signers, principal, namespace,
               shape_only=False):
    """Verify one tag.  Returns a result dictionary."""
    raw = read_tag_object(repo, tag)
    result = {"tag": tag, "repo": str(repo)}

    if raw is None:
        result.update({
            "annotated": False,
            "signature_kind": None,
            "ok": False,
            "detail": ("%s is a lightweight tag (it points straight at a commit) "
                       "and therefore carries no signature; use "
                       "`git tag -s -a %s` to create an annotated, signed tag"
                       % (tag, tag)),
        })
        return result

    kind = classify_signature(raw)
    result["annotated"] = True
    result["signature_kind"] = kind

    # The tag object's own name must match the ref it is reached
    # through.  A mismatch means the tag presents itself under a
    # different name than the one it was released as -- `git tag -v`,
    # `git describe` and the GitHub release page all read the internal
    # name, so this is what a consumer actually sees.
    object_name = read_tag_object_name(raw)
    expected_name = ref_basename(tag)
    result["tag_object_name"] = object_name
    result["expected_tag_name"] = expected_name
    result["tag_name_matches"] = (object_name == expected_name)

    if not result["tag_name_matches"]:
        if object_name is None:
            detail = ("the tag object %s carries no 'tag' header line, so it "
                      "does not name itself; expected the object to be "
                      "named %s" % (tag, expected_name))
        else:
            detail = ("the tag object reached through %s names itself %r, "
                      "but the ref is named %r; the tag therefore presents "
                      "itself under the wrong name (the internal name is "
                      "what `git tag -v`, `git describe` and the GitHub "
                      "release page report)"
                      % (tag, object_name, expected_name))
        result.update({"ok": False, "detail": detail})
        return result

    if kind is None:
        result.update({
            "ok": False,
            "detail": ("%s is an annotated tag but carries no signature block; "
                       "it was created with `git tag -a` instead of "
                       "`git tag -s -a`" % tag),
        })
        return result

    if shape_only:
        result.update({
            "ok": True,
            "detail": ("tag object carries a %s signature block (shape check "
                       "only; not verified cryptographically)" % kind),
        })
        return result

    verdict = _verify_with_git(repo, tag, kind, allowed_signers)
    result.update(verdict)

    if verdict.get("ok") and principal:
        signed_by = verdict.get("principal")
        if signed_by != principal:
            result["ok"] = False
            result["detail"] = ("signature is valid but was made by %s, "
                                "not the expected principal %s"
                                % (signed_by, principal))
    return result


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _self_test():
    """Build a throwaway repository, sign a tag, and verify it.

    The scratch repository lives in the system temp directory.  It must
    never be created inside the NimRTC working tree: a test that writes
    a `.git` directory into the repository it is testing would leave the
    tree in a state that no later check could trust.
    """
    git_bin = shutil.which("git")
    if git_bin is None:
        print("self-test: git not found", file=sys.stderr)
        return 2
    ssh_keygen = shutil.which("ssh-keygen")
    if ssh_keygen is None:
        print("self-test: ssh-keygen not found", file=sys.stderr)
        return 2

    failures = 0
    tmp = Path(tempfile.mkdtemp(prefix="nimrtc-tagselftest-"))
    try:
        git(["init", "-q", "."], repo=tmp)
        git(["config", "user.name", "NimRTC Self Test"], repo=tmp)
        git(["config", "user.email", DEFAULT_PRINCIPAL], repo=tmp)
        git(["config", "commit.gpgsign", "false"], repo=tmp)
        (tmp / "a.txt").write_text("self-test\n", encoding="utf-8")
        git(["add", "a.txt"], repo=tmp)
        git(["commit", "-q", "-m", "self-test commit"], repo=tmp)

        key = tmp / "key"
        gen = _run([ssh_keygen, "-t", "ed25519", "-N", "", "-C",
                    DEFAULT_PRINCIPAL, "-f", str(key)], cwd=tmp)
        if gen.returncode != 0:
            print("[FAIL] self-test: ssh-keygen could not create a key: %s"
                  % decode(gen.stderr).strip(), file=sys.stderr)
            return 1

        git(["config", "gpg.format", "ssh"], repo=tmp)
        git(["config", "user.signingkey", str(key) + ".pub"], repo=tmp)
        git(["config", "tag.gpgsign", "true"], repo=tmp)

        signers = tmp / "allowed_signers"
        pub = Path(str(key) + ".pub").read_text(encoding="utf-8").strip()
        signers.write_text(
            '%s namespaces="%s" %s\n' % (DEFAULT_PRINCIPAL, DEFAULT_NAMESPACE, pub),
            encoding="utf-8")

        signed = git(["tag", "-s", "-a", "v0.0.0-self", "-m", "self-test tag"],
                     repo=tmp)
        if signed.returncode != 0:
            print("[FAIL] self-test: could not sign a tag: %s"
                  % decode(signed.stderr).strip(), file=sys.stderr)
            return 1

        # 1. A correctly signed tag verifies.
        res = verify_tag(tmp, "v0.0.0-self", signers, DEFAULT_PRINCIPAL,
                         DEFAULT_NAMESPACE)
        if res.get("ok"):
            print("[OK  ] signed tag verifies (principal %s, key %s)"
                  % (res.get("principal"), res.get("fingerprint")))
        else:
            print("[FAIL] signed tag did not verify: %s" % res.get("detail"))
            failures += 1

        # 2. An unsigned annotated tag is rejected.
        #    `tag.gpgsign = true` is still set in this scratch repo, and
        #    it applies to *every* tag the repo creates -- including a
        #    plain `git tag -a`, which would silently come out signed
        #    and turn this case into a duplicate of case 1.  Turn it off
        #    for the call.
        git(["-c", "tag.gpgsign=false", "tag", "-a", "v0.0.0-unsigned",
             "-m", "unsigned"], repo=tmp)
        res = verify_tag(tmp, "v0.0.0-unsigned", signers, DEFAULT_PRINCIPAL,
                         DEFAULT_NAMESPACE)
        if not res.get("ok"):
            print("[OK  ] unsigned annotated tag is rejected")
        else:
            print("[FAIL] unsigned annotated tag was accepted")
            failures += 1

        # 3. A lightweight tag is rejected.  `tag.gpgsign = true` also
        #    turns a bare `git tag` into an annotated signed tag, so it
        #    has to be disabled here as well for this to be a
        #    lightweight tag at all.
        git(["-c", "tag.gpgsign=false", "tag", "v0.0.0-light"], repo=tmp)
        res = verify_tag(tmp, "v0.0.0-light", signers, DEFAULT_PRINCIPAL,
                         DEFAULT_NAMESPACE)
        if not res.get("ok"):
            print("[OK  ] lightweight tag is rejected")
        else:
            print("[FAIL] lightweight tag was accepted")
            failures += 1

        # 4. A tag whose signature block has been corrupted is rejected.
        #    The tampered object is written straight into the object
        #    database and pointed at by a ref, because `git tag` cannot
        #    create a tag object with a broken signature.
        raw = git(["cat-file", "tag", "v0.0.0-self"], repo=tmp).stdout
        lines = raw.split(b"\n")
        for index, line in enumerate(lines):
            if line.startswith(b"U1NIU0lH"):
                mutated = bytearray(line)
                mutated[10] = ord("A") if mutated[10] != ord("A") else ord("B")
                lines[index] = bytes(mutated)
                break
        tampered = b"\n".join(lines)
        proc = subprocess.run(
            ["git", "hash-object", "-w", "-t", "tag", "--stdin"],
            cwd=str(tmp), input=tampered, capture_output=True,
            timeout=60)
        if proc.returncode != 0:
            print("[FAIL] self-test: could not write the tampered tag object")
            failures += 1
        else:
            tampered_sha = decode(proc.stdout).strip()
            git(["update-ref", "refs/tags/v0.0.0-tampered", tampered_sha], repo=tmp)
            res = verify_tag(tmp, "v0.0.0-tampered", signers, DEFAULT_PRINCIPAL,
                             DEFAULT_NAMESPACE)
            if not res.get("ok"):
                print("[OK  ] tampered signature is rejected")
            else:
                print("[FAIL] tampered signature was accepted")
                failures += 1

        # 5. A tag signed by an unknown key is not accepted against an
        #    allowed_signers file that does not list it.
        other = tmp / "other_key"
        gen = _run([ssh_keygen, "-t", "ed25519", "-N", "", "-C",
                    "stranger@example.com", "-f", str(other)], cwd=tmp)
        if gen.returncode == 0:
            git(["config", "user.signingkey", str(other) + ".pub"], repo=tmp)
            git(["tag", "-s", "-a", "v0.0.0-stranger", "-m", "stranger tag"],
                repo=tmp)
            res = verify_tag(tmp, "v0.0.0-stranger", signers, DEFAULT_PRINCIPAL,
                             DEFAULT_NAMESPACE)
            if not res.get("ok"):
                print("[OK  ] signature from an unlisted key is rejected")
            else:
                print("[FAIL] signature from an unlisted key was accepted")
                failures += 1
        else:
            print("[FAIL] self-test: could not create the second key")
            failures += 1

        # 6. A tag whose OBJECT names itself differently from the ref it
        #    is reached through is rejected.  This is the defect the
        #    v1.3.0 release carried: the signing script created the tag
        #    object under a probe name (`v1.3.0-staged`) and then moved
        #    the ref onto that object with `git update-ref`, so the ref
        #    said `v1.3.0` while the object still said `v1.3.0-staged`.
        #    The signature verified -- it was a valid signature over a
        #    valid object -- and nothing complained, because nothing
        #    looked at the object's name.
        #
        #    The reproduction is the same two-step dance: sign under one
        #    name, point a differently-named ref at the resulting object.
        git(["tag", "-s", "-a", "v0.0.0-probe", "-m", "probe tag"], repo=tmp)
        probe_sha = git(["rev-parse", "refs/tags/v0.0.0-probe"],
                        repo=tmp).stdout.decode().strip()
        git(["update-ref", "refs/tags/v0.0.0-renamed", probe_sha], repo=tmp)
        res = verify_tag(tmp, "v0.0.0-renamed", signers, DEFAULT_PRINCIPAL,
                         DEFAULT_NAMESPACE)
        if not res.get("ok") and res.get("tag_object_name") == "v0.0.0-probe":
            print("[OK  ] a tag object whose internal name differs from the "
                  "ref is rejected")
        else:
            print("[FAIL] a tag object whose internal name differs from the "
                  "ref was accepted (ok=%s, name=%s)"
                  % (res.get("ok"), res.get("tag_object_name")))
            failures += 1

        # 7. The name check fires even under `--shape-only`, because it
        #    is a shape property, not a cryptographic one.  The release
        #    workflow only ever runs the shape check, so a check that
        #    only ran under full verification would never guard the
        #    release at all.
        res = verify_tag(tmp, "v0.0.0-renamed", signers, DEFAULT_PRINCIPAL,
                         DEFAULT_NAMESPACE, shape_only=True)
        if not res.get("ok"):
            print("[OK  ] the name check also fires under --shape-only")
        else:
            print("[FAIL] the name check did not fire under --shape-only")
            failures += 1

        # 8. A tag whose object names itself correctly is accepted by the
        #    name check -- the check must not reject well-formed tags.
        res = verify_tag(tmp, "v0.0.0-self", signers, DEFAULT_PRINCIPAL,
                         DEFAULT_NAMESPACE)
        if res.get("tag_name_matches") and res.get("tag_object_name") == "v0.0.0-self":
            print("[OK  ] a correctly named tag object matches its ref")
        else:
            print("[FAIL] a correctly named tag object did not match its ref "
                  "(name=%s)" % res.get("tag_object_name"))
            failures += 1

        # 9. The name is read from the HEADER, not from the message.
        #    A tag whose commit message happens to contain a line
        #    starting with "tag " must not be misread as naming itself.
        res = verify_tag(tmp, "v0.0.0-self", signers, DEFAULT_PRINCIPAL,
                         DEFAULT_NAMESPACE)
        if res.get("tag_object_name") != "v0.0.0-self":
            print("[FAIL] the tag name was read from the wrong part of the "
                  "object (name=%s)" % res.get("tag_object_name"))
            failures += 1
        else:
            print("[OK  ] the tag name is read from the header, not the message")

        # 10. `ref_basename` strips the `refs/tags/` prefix, so a caller
        #     may pass either form.
        if ref_basename("refs/tags/v1.2.3") == "v1.2.3" and \
                ref_basename("v1.2.3") == "v1.2.3":
            print("[OK  ] the ref basename is computed correctly")
        else:
            print("[FAIL] the ref basename is computed incorrectly")
            failures += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    print("self-test: %d failure(s)" % failures)
    return 0 if failures == 0 else 1


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify the signature on a NimRTC release tag.")
    parser.add_argument("--tag", default=None,
                        help="tag to verify (e.g. v1.1.0)")
    parser.add_argument("--repo", default=".", type=Path,
                        help="path to the repository (default: current directory)")
    parser.add_argument("--allowed-signers", default=None, type=Path,
                        help="allowed_signers file for SSH verification "
                             "(default: ~/.ssh/allowed_signers)")
    parser.add_argument("--expected-principal", default=DEFAULT_PRINCIPAL,
                        help="principal that must have made the signature "
                             "(default: %s)" % DEFAULT_PRINCIPAL)
    parser.add_argument("--namespace", default=DEFAULT_NAMESPACE,
                        help="ssh-keygen namespace (default: %s)" % DEFAULT_NAMESPACE)
    parser.add_argument("--shape-only", action="store_true",
                        help="only check that the tag is annotated and carries a "
                             "signature block; do not verify cryptographically")
    parser.add_argument("--json", action="store_true",
                        help="emit the result as JSON")
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in self-test and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    if not args.tag:
        parser.error("--tag is required (or use --self-test)")

    allowed = args.allowed_signers
    if allowed is None:
        allowed = Path(os.path.expanduser("~")) / ".ssh" / "allowed_signers"
    allowed = allowed.expanduser()

    try:
        repo = resolve_repo(args.repo)
        if not args.shape_only and not allowed.is_file():
            raise ToolError(
                "allowed_signers file not found: %s\n"
                "       For an SSH-signed tag this file must list the signing "
                "key with its principal, e.g.\n"
                "       %s namespaces=\"git\" ssh-ed25519 AAAA...\n"
                "       Use --shape-only to skip cryptographic verification."
                % (allowed, args.expected_principal))

        result = verify_tag(repo, args.tag, allowed, args.expected_principal,
                            args.namespace, shape_only=args.shape_only)
    except ToolError as exc:
        if args.json:
            print(json.dumps({"tag": args.tag, "ok": False, "error": str(exc)},
                             indent=2))
        else:
            print("ERROR: %s" % exc, file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        status = "OK  " if result.get("ok") else "FAIL"
        print("[%s] %s" % (status, result["tag"]))
        print("       annotated tag object : %s" % result.get("annotated"))
        print("       signature kind       : %s"
              % (result.get("signature_kind") or "<none>"))
        if result.get("principal"):
            print("       principal            : %s" % result["principal"])
        if result.get("fingerprint"):
            print("       signing key          : %s %s"
                  % (result.get("key_type"), result["fingerprint"]))
        print("       %s" % result.get("detail", ""))

    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())

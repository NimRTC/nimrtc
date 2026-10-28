#!/usr/bin/env python3
"""Minimal GitHub REST API helper.

Why: `gh` on this machine has a stale/invalid token, and the shell's
JSON parser chokes on the non-ASCII text GitHub returns.  This script
resolves the token from the git credential store and prints raw UTF-8
JSON, optionally filtered by a dotted path.

Usage:
    python tools/gh_api.py repos/NimRTC/nimrtc/actions/runs?per_page=15
    python tools/gh_api.py repos/NimRTC/nimrtc/actions/runs/123/jobs jobs
    python tools/gh_api.py -k name,conclusion,status repos/.../jobs
    python tools/gh_api.py --raw repos/.../runs/123/logs
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

CRED_FILE = os.path.join(os.path.expanduser("~"), ".git-credentials")
API = "https://api.github.com"


def resolve_token() -> str:
    """Return a GitHub token from the git credential store, then the env.

    The credential store is preferred: the GH_TOKEN in the environment is
    a fine-grained PAT that the API rejects with 401.
    """
    candidates: list[str] = []
    if os.path.exists(CRED_FILE):
        with open(CRED_FILE, encoding="utf-8") as handle:
            for line in handle:
                if "github.com" not in line:
                    continue
                cred = line.strip().split("://", 1)[-1].split("@github.com", 1)[0]
                if ":" in cred:
                    token = cred.split(":", 1)[1]
                    if token:
                        candidates.append(token)
    for var in ("GH_TOKEN", "GITHUB_TOKEN"):
        value = os.environ.get(var)
        if value and value.startswith(("ghp_", "gho_", "ghu_", "ghs_", "github_pat_")):
            candidates.append(value)

    for token in candidates:
        if _probe(token):
            return token
    raise SystemExit("no working GitHub token found in ~/.git-credentials or env")


def _probe(token: str) -> bool:
    req = urllib.request.Request(
        f"{API}/user",
        headers={"Authorization": f"Bearer {token}", "User-Agent": "nimrtc-gh-api"},
    )
    try:
        with urllib.request.urlopen(req) as resp:
            return resp.status == 200
    except Exception:
        return False


def _redirect_no_auth(newurl: str):
    """Drop the Authorization header when GitHub redirects to blob storage.

    The job-logs endpoint answers with a 302 to a signed Azure blob URL.
    Forwarding `Authorization: Bearer ...` there makes the blob service
    reject the request with "access token was missing or malformed", so
    the header must be stripped on a cross-host redirect.
    """
    class Handler(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            new = super().redirect_request(req, fp, code, msg, headers, newurl)
            if new is not None and urllib.parse.urlparse(newurl).netloc != urllib.parse.urlparse(req.full_url).netloc:
                new.headers = {k: v for k, v in new.headers.items() if k.lower() != "authorization"}
            return new

    return Handler()


def request(path: str, raw: bool = False) -> bytes:
    url = path if path.startswith("http") else f"{API}/{path.lstrip('/')}"
    req = urllib.request.Request(
        url,
        headers={
            "Authorization": f"Bearer {resolve_token()}",
            "Accept": "application/vnd.github+json",
            "User-Agent": "nimrtc-gh-api",
        },
    )
    opener = urllib.request.build_opener(_redirect_no_auth(None))
    try:
        with opener.open(req) as resp:
            return resp.read()
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        raise SystemExit(f"HTTP {exc.code} for {url}\n{body}") from None


def dig(node, path: str):
    for part in path.split("."):
        if isinstance(node, list):
            node = node[int(part)]
        else:
            node = node[part]
    return node


def main() -> int:
    args = sys.argv[1:]
    raw = False
    keys: list[str] = []
    positional: list[str] = []
    while args:
        flag = args.pop(0)
        if flag == "--raw":
            raw = True
        elif flag == "-k":
            keys = [k for k in args.pop(0).split(",") if k]
        else:
            positional.append(flag)

    if not positional:
        raise SystemExit(__doc__)
    payload = request(positional[0], raw)
    if raw:
        sys.stdout.buffer.write(payload)
        return 0

    data = json.loads(payload.decode("utf-8"))
    selector = positional[1] if len(positional) > 1 else None
    if selector:
        data = dig(data, selector)

    if keys and isinstance(data, list):
        out = []
        for item in data:
            out.append({k: item.get(k) for k in keys})
        data = out
    print(json.dumps(data, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())

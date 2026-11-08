#!/usr/bin/env python3
"""Fetch the CI-produced soak evidence records from GitHub Actions artifacts.

Why this exists
---------------
The seven-day release gate (v1.1 plan §2 Track C, item C5) needs one
record per platform per day.  The collector writes those records to
`build/soak/<platform>-<YYYY-MM-DD>.jsonl` and the workflow uploads them
as workflow artifacts.

The records used to accumulate on a per-platform branch
(`soak/evidence-<platform>`), one commit per day, forever.  That turned
a branch into an append-only artifact store: the branches grew without
bound, were never merged into `main`, and every one of them was a
long-lived ref that had to be kept alive.  The evidence does not belong
in the repository's history -- it belongs in the workflow runs, which
already keep it, with an expiry date and a retention policy instead of
a permanent branch.

So the records live in the artifacts and this tool pulls them back down
when the window is being read.  The release machine's own Windows
Scheduled Task still writes its half straight into `build/soak/`; this
tool fetches the CI half.

Usage
-----
    python tools/fetch_soak_evidence.py
    python tools/fetch_soak_evidence.py --since 2026-11-01 --out build/soak
    python tools/fetch_soak_evidence.py --dry-run

The token comes from the same places `tools/gh_api.py` looks: the git
credential store first, then `GH_TOKEN` / `GITHUB_TOKEN`.

The collector drops exactly duplicated records, so running this twice
over the same window is harmless.  Records are APPENDED, never
overwritten: the local Scheduled Task and the CI job both produce a
record for the same platform and day, and `run_soak.py` reads a day as
green only when every required case has a PASS record and no case has a
FAIL.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gh_api import _redirect_no_auth, resolve_token  # noqa: E402

API = "https://api.github.com"
REPO = "NimRTC/nimrtc"
# The workflow is addressed by FILE NAME, not by the numeric id and not
# by the repository path: `actions/workflows/{workflow_id}/runs` accepts
# the file name, and the numeric id would have to be re-derived every
# time the workflow is re-created.
WORKFLOW_ID = "soak.yml"
DEFAULT_OUT = Path("build") / "soak"
DEFAULT_DAYS = 30


class FetchError(Exception):
    """A recoverable problem while talking to GitHub."""


def api_get(path: str, token: str, raw: bool = False):
    url = path if path.startswith("http") else f"{API}/{path.lstrip('/')}"
    req = urllib.request.Request(
        url,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "User-Agent": "nimrtc-soak-evidence",
        },
    )
    opener = urllib.request.build_opener(_redirect_no_auth(None))
    try:
        with opener.open(req) as resp:
            payload = resp.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:400]
        raise FetchError(f"HTTP {exc.code} for {url}\n{detail}") from None
    except urllib.error.URLError as exc:
        raise FetchError(f"cannot reach {url}: {exc.reason}") from None
    if raw:
        return payload
    return json.loads(payload.decode("utf-8"))


def list_runs(token: str, since: str) -> list:
    """Every run of the soak workflow since `since` (YYYY-MM-DD)."""
    runs = []
    page = 1
    while True:
        query = (
            f"repos/{REPO}/actions/workflows/{WORKFLOW_ID}/runs"
            f"?per_page=100&page={page}&created=%3E%3D{since}"
        )
        data = api_get(query, token)
        batch = data.get("workflow_runs", [])
        runs.extend(batch)
        if len(batch) < 100:
            break
        page += 1
        if page > 20:  # 2000 runs is far beyond any plausible window
            break
    return runs


def list_artifacts(token: str, run_id: int) -> list:
    data = api_get(f"repos/{REPO}/actions/runs/{run_id}/artifacts?per_page=100", token)
    return data.get("artifacts", [])


def download_artifact(token: str, artifact: dict) -> bytes:
    return api_get(artifact["archive_download_url"], token, raw=True)


def extract_records(payload: bytes):
    """Return the (file name, record lines) pairs carried by one archive.

    The archive holds `build/soak/<platform>-<day>.jsonl`.  The FILE NAME
    is authoritative: it carries the day the collector measured, which
    is not necessarily the day the run happened (a run that starts at
    23:00 UTC writes the record for that same day, but a manually
    dispatched run can land after midnight).
    """
    found = []
    try:
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            for info in archive.infolist():
                if info.is_dir() or not info.filename.endswith(".jsonl"):
                    continue
                text = archive.read(info).decode("utf-8", "replace")
                lines = [line.strip() for line in text.splitlines() if line.strip()]
                found.append((os.path.basename(info.filename), lines))
    except zipfile.BadZipFile:
        raise FetchError("artifact payload is not a readable zip archive") from None
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--since",
        default=None,
        help="earliest run date to consider (YYYY-MM-DD); defaults to "
        f"{DEFAULT_DAYS} days ago",
    )
    parser.add_argument(
        "--out",
        default=str(DEFAULT_OUT),
        help=f"directory the records are written into (default: {DEFAULT_OUT})",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="list the runs and artifacts that would be fetched, write nothing",
    )
    args = parser.parse_args()

    since = args.since
    if since is None:
        import datetime

        since = (
            datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=DEFAULT_DAYS)
        ).strftime("%Y-%m-%d")

    try:
        token = resolve_token()
    except SystemExit as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    try:
        runs = list_runs(token, since)
    except FetchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if not runs:
        print(f"no soak runs since {since}; the CI half of the window has no evidence")
        return 0

    out_dir = Path(args.out)
    written = 0
    seen_artifacts = 0

    for run in runs:
        run_id = run["id"]
        created = run.get("created_at", "")[:10]
        for artifact in list_artifacts(token, run_id):
            name = artifact.get("name", "")
            if not name.startswith("soak-"):
                continue
            if artifact.get("expired"):
                print(
                    f"warning: artifact {name} has expired; its day cannot be evidenced",
                    file=sys.stderr,
                )
                continue
            seen_artifacts += 1
            if args.dry_run:
                print(f"would fetch {name} (run {run_id}, {created})")
                continue
            try:
                payload = download_artifact(token, artifact)
                entries = extract_records(payload)
            except FetchError as exc:
                print(f"warning: {name}: {exc}", file=sys.stderr)
                continue
            if not entries:
                continue
            for file_name, lines in entries:
                if not lines:
                    continue
                # The archive holds `build/soak/<platform>-<day>.jsonl`
                # and the collector writes exactly that name, so the
                # base name is the target file name.
                target = out_dir / file_name
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("a", encoding="utf-8") as handle:
                    for line in lines:
                        handle.write(line + "\n")
                written += len(lines)

    if args.dry_run:
        print(f"{seen_artifacts} artifact(s) would be fetched from {len(runs)} run(s)")
        return 0

    print(
        f"fetched {written} record(s) from {seen_artifacts} artifact(s) "
        f"across {len(runs)} run(s) into {out_dir}"
    )
    if written == 0:
        print(
            "no records were fetched; the CI half of the window contributes nothing "
            "until the collector's first run lands on a run of this workflow"
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FetchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(2)

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
long-lived ref that had to be kept alive.  It was also lossy: the
workflow wrote each day's file with a *copy*, so a second run of the
same day REPLACED the first run's records.  A failing run that a later
retry "fixed" was erased from the branch -- and the collector's whole
point is that such a failure is never hidden behind a later pass.  The
artifacts keep every run separately, so nothing is lost there.

The records therefore live in the artifacts, and this tool pulls them
back down when the window is being read.  The release machine's own
Windows Scheduled Task still writes its half straight into
`build/soak/`; this tool fetches the CI half.

The fetcher is idempotent: the same record fetched twice is written
once.  The collector also drops exactly duplicated records, but a file
that grows every time the fetcher runs is not a record anyone can
read, and the tool runs every night over the same window.

Usage
-----
    python tools/fetch_soak_evidence.py
    python tools/fetch_soak_evidence.py --days 21
    python tools/fetch_soak_evidence.py --platform linux-x86_64
    python tools/fetch_soak_evidence.py --since 2026-11-01 --out build/soak
    python tools/fetch_soak_evidence.py --dry-run
    python tools/fetch_soak_evidence.py --self-test

The token comes from the same places `tools/gh_api.py` looks: the git
credential store first, then `GH_TOKEN` / `GITHUB_TOKEN`.

Records are APPENDED, never overwritten: the Windows Scheduled Task and
the CI job both produce a record for the same platform and day, and
`run_soak.py` reads a day as green only when every required case has a
PASS record and no case has a FAIL.
"""

from __future__ import annotations

import argparse
import datetime
import io
import json
import os
import sys
import tempfile
import time
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
# The artifact of one run of one platform is named
# `soak-<platform>-<run id>`.  The platform may itself contain a dash
# (`linux-x86_64`), so the run id is split off from the right.
ARTIFACT_PREFIX = "soak-"


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
    """Download one artifact archive, retrying a dropped connection.

    The download is a redirect to a blob store, and that store resets
    the connection often enough to matter over a window of dozens of
    artifacts.  A reset is a transport hiccup, not a missing
    artifact, so it is worth a second try; an HTTP error is not.
    """
    last = None
    for attempt in range(3):
        try:
            return api_get(artifact["archive_download_url"], token, raw=True)
        except FetchError as exc:
            last = exc
            if "cannot reach" not in str(exc):
                raise
            time.sleep(2 * (attempt + 1))
    raise last


def artifact_platform(name: str) -> str | None:
    """The platform an artifact belongs to, or None if it is not ours.

    The name is `soak-<platform>-<run id>`; the run id is numeric and the
    platform may contain dashes, so the split is taken from the right.
    """
    if not name.startswith(ARTIFACT_PREFIX):
        return None
    rest = name[len(ARTIFACT_PREFIX):]
    platform, _, run_id = rest.rpartition("-")
    if not platform or not run_id.isdigit():
        return None
    return platform


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


def merge_records(path: Path, lines) -> int:
    """Append the record lines that the file does not already carry.

    The fetcher is run over the same window every night, so the same
    record comes back again and again.  Appending blindly would grow the
    file without bound; the collector drops exactly duplicated records
    when it reads them, but a file that grows every night is not a
    record anyone can read.  The write is therefore idempotent: a line
    that is already in the file is not written a second time, and the
    file ends up byte-identical however often the fetcher runs.

    Returns the number of records that were actually added.
    """
    existing = set()
    if path.exists():
        with path.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line:
                    existing.add(line)
    path.parent.mkdir(parents=True, exist_ok=True)
    added = 0
    with path.open("a", encoding="utf-8") as handle:
        for line in lines:
            line = line.strip()
            if not line or line in existing:
                continue
            handle.write(line + "\n")
            existing.add(line)
            added += 1
    return added


def _self_test() -> int:
    """Pin the parts of the fetcher that do not need the network."""
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    # The platform is split off the artifact name from the right, because
    # the platform itself may contain a dash.
    check(artifact_platform("soak-linux-x86_64-42471972402") == "linux-x86_64",
          "the platform is read off the artifact name")
    check(artifact_platform("soak-windows-42471972402") == "windows",
          "a single-word platform is read off the artifact name")
    check(artifact_platform("bench-linux-x86_64-42471972402") is None,
          "an artifact of another workflow is not ours")
    check(artifact_platform("soak-linux-x86_64") is None,
          "a name without a run id is not ours")

    with tempfile.TemporaryDirectory() as tmp:
        target = Path(tmp) / "linux-x86_64-2026-11-08.jsonl"
        first = merge_records(target, ['{"case": "d"}', '{"case": "e"}'])
        check(first == 2, "both records are written the first time")
        again = merge_records(target, ['{"case": "d"}', '{"case": "e"}'])
        check(again == 0, "the same records are not written twice")
        check(target.read_text(encoding="utf-8").count("\n") == 2,
              "the file holds one copy of each record")
        fresh = merge_records(target, ['{"case": "d"}', '{"case": "f"}'])
        check(fresh == 1, "a record that is not there yet is written")
        check(target.read_text(encoding="utf-8").count("\n") == 3,
              "the file grows by exactly the new record")
        merged = merge_records(target, ['{"case": "d"}', '{"case": "f"}'])
        check(merged == 0, "the file is stable once every record is there")

    print("self-test: %d failure(s)" % len(failures))
    for message in failures:
        print("  FAIL %s" % message)
    return 0 if not failures else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--since",
        default=None,
        help="earliest run date to consider (YYYY-MM-DD); defaults to "
        "--days days ago",
    )
    parser.add_argument(
        "--days",
        type=int,
        default=DEFAULT_DAYS,
        help="how many days back to look when --since is not given "
        f"(default: {DEFAULT_DAYS})",
    )
    parser.add_argument(
        "--platform",
        default=None,
        help="only fetch the artifacts of this platform (comma separated "
        "for several); default: every platform",
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
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the built-in self-test and exit",
    )
    args = parser.parse_args()

    if args.self_test:
        return _self_test()

    since = args.since
    if since is None:
        since = (
            datetime.datetime.now(datetime.timezone.utc)
            - datetime.timedelta(days=max(0, args.days))
        ).strftime("%Y-%m-%d")

    wanted = None
    if args.platform:
        wanted = {item.strip() for item in args.platform.split(",") if item.strip()}

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
    failed = []

    for run in runs:
        run_id = run["id"]
        created = run.get("created_at", "")[:10]
        for artifact in list_artifacts(token, run_id):
            name = artifact.get("name", "")
            if not name.startswith(ARTIFACT_PREFIX):
                continue
            platform = artifact_platform(name)
            if wanted is not None and platform not in wanted:
                continue
            if artifact.get("expired"):
                print(
                    f"warning: artifact {name} has expired; its day cannot be evidenced",
                    file=sys.stderr,
                )
                failed.append(name)
                continue
            seen_artifacts += 1
            if args.dry_run:
                print(f"would fetch {name} (run {run_id}, {created})")
                continue
            try:
                payload = download_artifact(token, artifact)
                entries = extract_records(payload)
            except FetchError as exc:
                print(f"error: {name}: {exc}", file=sys.stderr)
                failed.append(name)
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
                written += merge_records(target, lines)

    if args.dry_run:
        print(f"{seen_artifacts} artifact(s) would be fetched from {len(runs)} run(s)")
        return 0

    print(
        f"fetched {written} record(s) from {seen_artifacts} artifact(s) "
        f"across {len(runs)} run(s) into {out_dir}"
    )
    if failed:
        # A window that is read from an incomplete set of artifacts is
        # not the window.  The failure is reported as a failure so the
        # caller cannot mistake a short read for a short window.
        print(
            f"error: {len(failed)} artifact(s) could not be read: "
            f"{', '.join(sorted(failed))}; the window read is INCOMPLETE",
            file=sys.stderr,
        )
        return 2
    if written == 0:
        print(
            "no new records were fetched; the CI half of the window contributes "
            "nothing until the collector's first run lands on a run of this workflow"
        )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FetchError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(2)

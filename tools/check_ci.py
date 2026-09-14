"""Quick CI status check via GitHub API."""
import json
import sys
import urllib.error
import urllib.request

API = "https://api.github.com/repos/NimRTC/nimrtc/actions/runs?per_page=15"
HEADERS = {"User-Agent": "nimrtc-check/1.0", "Accept": "application/vnd.github+json"}


def main():
    req = urllib.request.Request(API, headers=HEADERS)
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = ""
        try:
            body = e.read().decode("utf-8", errors="replace")
        except Exception:
            pass
        print(f"HTTPError {e.code}: {body}")
        return 1
    except Exception as e:
        print(f"ERROR: {e}")
        return 1

    runs = data.get("workflow_runs", [])
    if not runs:
        print("No workflow runs found.")
        return 0

    header = "SHA      BRANCH                          STATUS     CONCLUSION WORKFLOW"
    print(header)
    print("-" * len(header))
    for r in runs:
        sha = r["head_sha"][:7]
        branch = r["head_branch"]
        status = r["status"]
        conclusion = r.get("conclusion") or "pending"
        name = r["name"]
        print(f"{sha:<7} {branch:<30} {status:<10} {conclusion:<10} {name}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

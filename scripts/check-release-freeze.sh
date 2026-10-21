#!/bin/sh
#
# scripts/check-release-freeze.sh
# ===============================
# Pre-tag guardrail for the v1.0.0 "Stable" cut.
#
# `docs/plan/v1.0-plan.md` section 3 (Definition of Done) and
# `docs/adr/ADR-015-api-stability-policy.md` ("Verification") list the
# invariants that must hold at tag cut time. Most are covered by ctest.
# A handful are repo-level facts that no unit test sees and that quietly
# rot between releases:
#
#   sfu         The SFU track (closed as NO on 2026-10-19, v1.0-plan
#               section 7) must not acquire real engine wiring while its
#               Profile JSONs are retained for documentation.
#   api         Every public header carries at least one stability
#               marker. `tests/test_api_classification.cpp` asserts the
#               same property from inside the test binary; this check
#               makes it runnable before a build and prints the file
#               list in one place.
#   api-symbols Every *symbol* (not just every file) at namespace scope
#               carries a marker. `scripts/api_freeze_scan.js` is the
#               same scanner CI runs on all four platforms; this check
#               invokes it so the tag cut cannot pass with an
#               unclassified symbol that CI would only catch later.
#               Requires node; reported as SKIP when node is absent so
#               the POSIX-only path stays usable.
#   version     NIMRTC_API_VERSION_PRE_RELEASE must be "" for a final
#               tag (ADR-015 Verification table).
#   submodules  Every pin must be fetchable from a fresh clone. The
#               v0.14.0-rc.1 cycle briefly pinned src/third_party/msquic
#               at a local-only commit (a88383e) and had to be reverted
#               (ef6ff11); a pin that exists on one machine only breaks
#               every other clone.
#   root        No scratch scripts, captured logs, TLS keylogs or CTest
#               output at the project root (project cleanliness rule;
#               the pre-commit hook scripts/check-root-clean.sh enforces
#               the same list at commit time).
#
# Written as POSIX sh rather than Python so it runs on every platform
# NimRTC targets (including runners with no Python) and so it shares a
# dialect with the pre-commit hook.
#
# Usage:
#   sh scripts/check-release-freeze.sh                # run all checks
#   sh scripts/check-release-freeze.sh --final-tag    # require empty PRE_RELEASE
#   sh scripts/check-release-freeze.sh --only sfu,api # subset by id
#   sh scripts/check-release-freeze.sh --list         # show check ids
#   sh scripts/check-release-freeze.sh --no-network   # skip submodule query
#
# Exit codes:
#   0  every selected check passed
#   1  at least one check failed
#   2  the script itself could not run (bad args, not a git repo, ...)

set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT" || exit 2

FINAL_TAG=0
ONLY=""
NO_NETWORK=0

while [ $# -gt 0 ]; do
    case "$1" in
        --final-tag)  FINAL_TAG=1 ;;
        --no-network) NO_NETWORK=1 ;;
        --list)
            echo "sfu"
            echo "api"
            echo "api-symbols"
            echo "version"
            echo "submodules"
            echo "root"
            exit 0
            ;;
        --only)
            shift
            [ $# -gt 0 ] || { echo "ERROR: --only needs an argument" >&2; exit 2; }
            ONLY="$1"
            ;;
        --only=*)     ONLY="${1#--only=}" ;;
        -h|--help)
            sed -n '3,45p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            exit 2
            ;;
    esac
    shift
done

ALL_CHECKS="sfu api api-symbols version submodules root"
if [ -n "$ONLY" ]; then
    for c in $(echo "$ONLY" | tr ',' ' '); do
        case " $ALL_CHECKS " in
            *" $c "*) ;;
            *)
                echo "ERROR: unknown check id: $c" >&2
                echo "Known ids: $ALL_CHECKS" >&2
                exit 2
                ;;
        esac
    done
    CHECKS="$ONLY"
else
    CHECKS="$ALL_CHECKS"
fi
# Normalize comma-separated ids into space-separated tokens so `want`
# can do a plain `* "$id" *` substring match (e.g. "--only api,version"
# must trigger both `want api` and `want version`). The summary block at
# the end mirrors this conversion when counting.
CHECKS=$(echo "$CHECKS" | tr ',' ' ')

if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "ERROR: $REPO_ROOT is not a git repository" >&2
    exit 2
fi

echo "release freeze checklist ($REPO_ROOT)"
if [ "$FINAL_TAG" -eq 1 ]; then
    echo "mode: --final-tag (NIMRTC_API_VERSION_PRE_RELEASE must be empty)"
fi
echo

FAILED=""

report() {
    # report <PASS|FAIL> <check-id> <note>...
    status="$1"; cid="$2"; shift 2
    echo "[$status] $cid"
    for note in "$@"; do
        echo "       $note"
    done
    echo
    [ "$status" = "FAIL" ] && FAILED="$FAILED $cid"
    return 0
}

want() {
    case " $CHECKS " in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# sfu — the closed track must stay inert
# ---------------------------------------------------------------------------
if want sfu; then
    notes=""
    failures=""
    # Only the assembly profile header is allowed to name the SFU track.
    # profiles/*.json are documentation and are outside this search.
    hits=$(git grep -lIiE 'sfu[-_]agent|kProfileSfu' \
               -- src/ tests/ examples/ tools/ 2>/dev/null \
           | sed 's|\\|/|g' \
           | grep -v '^src/modules/assembly/include/nimrtc/assembly/profiles\.hpp$' \
           | grep -v '^src/modules/assembly/include/nimrtc/assembly/assembly\.hpp$')
    if [ -n "$hits" ]; then
        failures="the closed SFU track is referenced outside the documentation-only allow-list:
$(echo "$hits" | sed 's/^/  /')"
    else
        notes="no SFU engine wiring; only the assembly profile header names it (expected)"
    fi
    if [ -n "$failures" ]; then
        report FAIL sfu "$failures"
    else
        report PASS sfu "$notes"
    fi
fi

# ---------------------------------------------------------------------------
# api — every public header carries a stability marker
# ---------------------------------------------------------------------------
if want api; then
    count=0
    unmarked=""
    for h in $(find src -name '*.hpp' -type f 2>/dev/null | tr '\\' '/'); do
        case "$h" in
            src/third_party/*) continue ;;
            *third_party/*) continue ;;
        esac
        case "$h" in
            */include/nimrtc/*) ;;
            *) continue ;;
        esac
        count=$((count + 1))
        if ! grep -qE 'NIMRTC_API_(STABLE|UNSTABLE)' "$h"; then
            unmarked="$unmarked $h"
        fi
    done
    if [ "$count" -eq 0 ]; then
        report FAIL api "no public headers found under src/<mod>/include/nimrtc/"
    elif [ -n "$unmarked" ]; then
        notes=$(for h in $unmarked; do echo "$h: no NIMRTC_API_STABLE / NIMRTC_API_UNSTABLE"; done)
        report FAIL api "$count public headers scanned" "$notes"
    else
        report PASS api "$count public headers scanned" \
                        "every public header carries a stability marker"
    fi
fi

# ---------------------------------------------------------------------------
# api-symbols — every namespace-scope symbol carries a marker (per-symbol)
# ---------------------------------------------------------------------------
# This delegates to scripts/api_freeze_scan.js, the exact scanner CI runs
# on windows-2022 / linux-gcc / macos-clang / linux-aarch64. Keeping one
# scanner means the tag cut and the merge gate cannot disagree: a symbol
# the CI accepts is a symbol the release accepts, and vice versa.
if want api-symbols; then
    if ! command -v node >/dev/null 2>&1; then
        report PASS api-symbols "skipped (node not on PATH)" \
                        "the per-symbol scan is the same gate CI runs; \
install node to run it locally"
    elif [ ! -f scripts/api_freeze_scan.js ]; then
        report FAIL api-symbols "scripts/api_freeze_scan.js not found"
    else
        # The scanner writes its machine-readable inventory to build/
        # and returns non-zero when any public symbol is unclassified.
        # Capture stdout so the failure note stays readable in a tag-cut
        # log rather than scrolling past as raw console output.
        mkdir -p build
        scan_out=$(node scripts/api_freeze_scan.js 2>&1)
        scan_rc=$?
        if [ "$scan_rc" -eq 0 ]; then
            report PASS api-symbols "$(echo "$scan_out" | sed -n '2,5p' | tr '\n' ' ')"
        else
            report FAIL api-symbols "unclassified public symbols found" \
                          "$scan_out"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# version — NIMRTC_API_VERSION_PRE_RELEASE tracks the release stage
# ---------------------------------------------------------------------------
if want version; then
    hdr="src/core/include/nimrtc/core/api_version.hpp"
    if [ ! -f "$hdr" ]; then
        report FAIL version "$hdr not found"
    else
        value=$(sed -n 's/^#define[[:space:]]\+NIMRTC_API_VERSION_PRE_RELEASE[[:space:]]\+"\([^"]*\)".*/\1/p' "$hdr" | head -1)
        if [ -z "$value" ] && ! grep -q 'NIMRTC_API_VERSION_PRE_RELEASE' "$hdr"; then
            report FAIL version "NIMRTC_API_VERSION_PRE_RELEASE is not defined in api_version.hpp"
        elif [ "$FINAL_TAG" -eq 1 ] && [ -n "$value" ]; then
            report FAIL version "NIMRTC_API_VERSION_PRE_RELEASE = \"$value\"" \
                              "must be \"\" for a final tag (ADR-015 Verification table)"
        elif [ "$FINAL_TAG" -eq 0 ]; then
            case "$value" in
                ""|alpha*|beta*|rc*)
                    report PASS version "NIMRTC_API_VERSION_PRE_RELEASE = \"$value\"" \
                                      "valid stage label (final-tag emptiness needs --final-tag)"
                    ;;
                *)
                    report FAIL version "NIMRTC_API_VERSION_PRE_RELEASE = \"$value\"" \
                                      'expected "", "alpha.N", "beta.N" or "rc.N"'
                    ;;
            esac
        else
            report PASS version "NIMRTC_API_VERSION_PRE_RELEASE = \"$value\"" \
                              "correctly empty for a final tag"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# submodules — every pin must be fetchable from a fresh clone
# ---------------------------------------------------------------------------
if want submodules; then
    status_lines=$(git submodule status 2>/dev/null)
    if [ -z "$status_lines" ]; then
        report FAIL submodules "git submodule status returned nothing (not a git repo, or no submodules)"
    else
        notes=""
        failures=""
        echo "$status_lines" | while IFS= read -r line; do
            [ -z "$line" ] && continue
            # `git submodule status` prints a fixed-width one-character
            # status prefix, which is a SPACE when the checkout matches
            # the recorded pin. So the sha is field 1 and the path field 2
            # of the remainder -- not fields 2 and 3 of the whole line.
            flag=$(printf '%s' "$line" | cut -c1)
            rest=$(printf '%s' "$line" | cut -c2-)
            sha=$(printf '%s' "$rest" | awk '{print $1}')
            path=$(printf '%s' "$rest" | sed 's/^[^ ]* //; s/ (.*//')
            case "$flag" in
                -) printf 'NOTE %s: not initialised (submodule update --init)\n' "$path" ;;
                +) printf 'BAD  %s: working tree differs from the recorded pin (%s)\n' "$path" "$sha" ;;
                *)
                    if [ "$NO_NETWORK" -eq 1 ]; then
                        printf 'NOTE %s: pinned %.12s (reachability not checked: --no-network)\n' "$path" "$sha"
                    elif (cd "$path" 2>/dev/null && git branch -r --contains "$sha" 2>/dev/null) | grep -q .; then
                        via=$(cd "$path" && git branch -r --contains "$sha" 2>/dev/null | head -1 | tr -d ' ')
                        printf 'NOTE %s: pinned %.12s reachable via %s\n' "$path" "$sha" "$via"
                    else
                        printf 'BAD  %s: pinned %.12s is not on any remote branch (unfetchable from a fresh clone)\n' "$path" "$sha"
                    fi
                    ;;
            esac
        done > "$REPO_ROOT/build/.freeze-submodules.tmp" 2>/dev/null \
          || : > "$REPO_ROOT/build/.freeze-submodules.tmp"

        if [ -f "$REPO_ROOT/build/.freeze-submodules.tmp" ]; then
            notes=$(grep '^NOTE' "$REPO_ROOT/build/.freeze-submodules.tmp" | sed 's/^NOTE /submodule /' | tr '\n' '\n')
            failures=$(grep '^BAD' "$REPO_ROOT/build/.freeze-submodules.tmp" | sed 's/^BAD  /submodule /')
            rm -f "$REPO_ROOT/build/.freeze-submodules.tmp"
        fi

        if [ -n "$failures" ]; then
            report FAIL submodules "$failures"
        else
            [ -n "$notes" ] || notes="no submodules reported"
            report PASS submodules "$notes"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# root — no scratch / log / keylog / Testing artifacts at the project root
# ---------------------------------------------------------------------------
if want root; then
    offenders=""
    for entry in "$REPO_ROOT"/* "$REPO_ROOT"/.[!.]*; do
        [ -e "$entry" ] || continue
        name=$(basename "$entry")
        case "$name" in
            sslkeylog*.log|*keylog*.log|Testing|_find_*.ps1|stdout.txt|stderr.txt)
                if [ -d "$entry" ]; then offenders="$offenders $name/"; else offenders="$offenders $name"; fi
                ;;
        esac
    done
    if [ -n "$offenders" ]; then
        notes=$(for o in $offenders; do echo "$o: forbidden at project root"; done)
        report FAIL root "$notes"
    else
        report PASS root "no scratch / log / keylog / Testing artifacts at the root"
    fi
fi

# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------
if [ -n "$FAILED" ]; then
    echo "FAILED:$FAILED"
    exit 1
fi

echo "OK: $(echo "$CHECKS" | tr ',' ' ' | wc -w | tr -d ' ') check(s) passed"
exit 0
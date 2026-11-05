#!/usr/bin/env bash
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
#
# Linux weak-network baseline (v1.3 Q4) via 'tc qdisc netem'.
#
# The Linux counterpart of tools/benchmark_weaknet.py: the same three
# tiers (5% loss / 100 ms jitter / 1 Mbps cap) measured with kernel-side
# impairments instead of the in-process userspace shaper.  The netem
# numbers are what a real socket sees; the in-process shaper exists so
# the same tier list can be measured on Windows and macOS without root.
#
# How the impairments are applied: the whole benchmark runs inside a
# throwaway network namespace whose only interface is 'lo', and the
# netem qdisc is attached to that 'lo'.  Loopback traffic then crosses
# the qdisc, so the kernel really drops, delays and rate-limits the
# packets.  A namespace is used rather than the host's 'lo' because a
# root qdisc there would reshape every loopback packet on the machine
# (including the ones the build system and the test harness use).
#
# Requires root (CAP_NET_ADMIN) and iproute2.  Run:
#     sudo tools/benchmark_weaknet_netem.sh --count 200 --size 1200
#
# Output lands in build/e2e/bench_weaknet_netem.json so the project root
# stays clean.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COUNT=200
SIZE=1200
OUT="$ROOT/build/e2e/bench_weaknet_netem.json"
NS="nimrtcbench"

usage() {
  cat <<'EOF'
usage: tools/benchmark_weaknet_netem.sh [--count N] [--size BYTES] [--out PATH]

Runs the three v1.3 Q4 weak-network tiers with 'tc qdisc netem'.
Requires root (CAP_NET_ADMIN) and iproute2.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --count) COUNT="$2"; shift 2 ;;
    --size) SIZE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "error: this script needs root (CAP_NET_ADMIN) to attach a netem qdisc" >&2
  exit 3
fi

for tool in tc ip python3; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "error: '$tool' not found on PATH" >&2
    exit 3
  }
done

cleanup() {
  ip netns del "$NS" 2>/dev/null || true
}
trap cleanup EXIT

cleanup
ip netns add "$NS"
ip netns exec "$NS" ip link set lo up

mkdir -p "$ROOT/build/e2e"

# The tiers, in the same order and with the same parameters as the
# in-process shaper in tools/benchmark_weaknet.py:
#
#   loss-5pct    : 5% uniform drop, 5 ms base delay
#   jitter-100ms : 20 ms base + uniform 0..100 ms jitter
#   cap-1mbps    : 1 Mbps token pace, 5 ms base delay
#
# Two netem specifics are worth spelling out:
#
#   * netem's 'delay TIME JITTER' spreads the delay uniformly *around*
#     TIME, so the jitter tier is expressed as 'delay 70ms 50ms': the
#     spread is 20..120 ms, i.e. the same 20 ms base with 0..100 ms of
#     jitter that the portable shaper applies.
#   * netem's 'rate' limits by queueing, so the sender must not
#     out-run it: the tool paces its sends at exactly the cap rate
#     (the same pacing the portable shaper uses), which keeps the
#     queue empty and makes the reading the configured delay.
#   * the loss is spelled 'loss random PERCENT': the bare 'loss
#     PERCENT' shorthand is rejected by some iproute2 builds, while
#     'loss random' is accepted everywhere.
#
# Each tier needs its own qdisc, so the tool is invoked once per tier
# with --tier, and the per-tier JSON is merged at the end.
run_tier() {
  local tier_id="$1"; shift
  echo "--- tier ${tier_id}: netem $*"
  ip netns exec "$NS" tc qdisc replace dev lo root netem "$@"
  ip netns exec "$NS" python3 "$ROOT/tools/benchmark_weaknet.py" \
    --mode netem --tier "$tier_id" --count "$COUNT" --size "$SIZE" \
    --out "$ROOT/build/e2e/bench_weaknet_netem_${tier_id}.json" --json
}

run_tier "loss-5pct" delay 5ms loss random 5%
run_tier "jitter-100ms" delay 70ms 50ms
run_tier "cap-1mbps" delay 5ms rate 1mbit

python3 - "$ROOT/build/e2e" "$OUT" "$COUNT" "$SIZE" <<'PY'
import json, pathlib, sys

e2e, out, count, size = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
tiers = ("loss-5pct", "jitter-100ms", "cap-1mbps")
results = []
for tier in tiers:
    data = json.loads((e2e / f"bench_weaknet_netem_{tier}.json").read_text())
    results.extend(data["results"])

merged = {
    "mode": "netem",
    "count_per_tier": count,
    "payload_bytes": size,
    "seed": None,
    "results": results,
    "note": ("kernel-side impairments via 'tc qdisc netem' on 'lo' inside a "
             "throwaway network namespace; the tool paces sends and measures "
             "arrival-minus-send; transport-floor baseline, not engine "
             "glass-to-glass"),
}
out.write_text(json.dumps(merged, indent=2), encoding="utf-8")
for r in results:
    print(f"[{r['tier']}] rx={r['received']}/{r['sent']} "
          f"ratio={r['delivery_ratio']:.3f} p50={r['p50_us']:.0f}us "
          f"p99={r['p99_us']:.0f}us")
print(f"wrote {out}")
PY

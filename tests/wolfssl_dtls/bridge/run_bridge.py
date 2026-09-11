#!/usr/bin/env python3
"""Run the wolfSSL bridge components without creating files in the repository root.

The Chrome page and signaling topology are deployment-specific; this runner
provides the deterministic build/artifact checks and reserves build/wolfssl_bridge
for all captured output.
"""
from __future__ import annotations
import argparse, json, subprocess, sys
from pathlib import Path

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default='build')
    ap.add_argument('--responder', type=Path)
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[3]
    out = root / args.build_dir / 'wolfssl_bridge'
    out.mkdir(parents=True, exist_ok=True)
    exe = args.responder or (root / args.build_dir / 'tests' / 'wolfssl_dtls' / 'bridge' / 'wolfssl_responder.exe')
    if not exe.exists():
        print(f'responder not found: {exe}; configure/build the bridge first', file=sys.stderr)
        return 2
    # Keep a machine-readable status file even when Playwright is unavailable.
    status = {'dtlsState': 'not-started', 'responder': str(exe), 'note': 'launch with Chrome signaling topology'}
    (out / 'chrome.json').write_text(json.dumps(status, indent=2) + '\n', encoding='utf-8')
    print(f'bridge artifacts: {out}')
    print('Chrome driver setup is environment-specific; use Playwright and signaling_proxy.py as documented.')
    return 0
if __name__ == '__main__':
    raise SystemExit(main())

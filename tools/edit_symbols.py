#!/usr/bin/env python3
"""
edit_symbols.py — Apply per-symbol stability annotation class-docstring updates.

Run this ONCE after a fresh checkout of the PER-SYMBOL-CLASSIFY branch to
synchronise the class-level docstrings with the file-level "Per-symbol
stability annotations" note added in the v0.14.x sweep.

What it does
------------
For each header it replaces the class-level docstring of the primary public
interface with the version that includes the per-symbol annotation summary.
The file-level header comment (added by the sweep) already carries the same
information; the class-level note makes the summary visible both at file
scope and at the class definition site.

Re-run safety: each replacement is idempotent (old == new after the first run).

Supported files
--------------
  src/plugins/include/nimrtc/plugins/datachannel.hpp   — IDataChannel
  src/plugins/include/nimrtc/plugins/transport.hpp    — ITransport
  src/engine/include/nimrtc/engine/engine.hpp          — NimRTCEngine

Requirements
------------
Python 3.8+.  No external packages.  Must be run from the repo root
(the script uses paths relative to cwd).
"""

import sys
import os

def edit_file(path, old_bytes, new_bytes):
    """
    Replace `old_bytes` with `new_bytes` in `path`, matching CRLF or LF
    transparently.

    Returns True on success, False if `old_bytes` is not found.
    Prints 'OK: <path>' on success, 'SKIP: <path>' if already up to date.
    """
    with open(path, 'rb') as fh:
        content = fh.read()

    if old_bytes not in content:
        # Already applied — idempotent
        print(f'SKIP (already applied): {path}')
        return True

    content = content.replace(old_bytes, new_bytes, 1)
    with open(path, 'wb') as fh:
        fh.write(content)
    print(f'OK:   {path}')
    return True


# ---------------------------------------------------------------------------
# datachannel.hpp — IDataChannel
# ---------------------------------------------------------------------------
_datachannel_path = 'src/plugins/include/nimrtc/plugins/datachannel.hpp'

# Pattern: class docstring WITHOUT per-symbol note → WITH per-symbol note
_datachannel_old = (
    b'/** Bidirectional data-channel (RFC 8831).\r\n'
    b' *\r\n'
    b' * A channel is created by IDataChannelFactory::create(), configured via\r\n'
    b' * open(), used via send(), and torn down via close().\r\n'
    b' *\r\n'
    b' * Thread safety:\r\n'
    b' *   - set_on_message / set_on_state are called from a single thread\r\n'
    b' *     (the engine thread) before open().\r\n'
    b' *   - send() is thread-safe \xe2\x80\x94 implementations must queue outbound data.\r\n'
    b' *   - on_message / on_state callbacks are dispatched from the engine tick.\r\n'
    b' */'
)
_datachannel_new = (
    b'/** Bidirectional data-channel (RFC 8831).\r\n'
    b' *\r\n'
    b' * A channel is created by IDataChannelFactory::create(), configured via\r\n'
    b' * open(), used via send(), and torn down via close().\r\n'
    b' *\r\n'
    b' * Thread safety:\r\n'
    b' *   - set_on_message / set_on_state are called from a single thread\r\n'
    b' *     (the engine thread) before open().\r\n'
    b' *   - send() is thread-safe \xe2\x80\x94 implementations must queue outbound data.\r\n'
    b' *   - on_message / on_state callbacks are dispatched from the engine tick.\r\n'
    b' *\r\n'
    b' * ## Per-symbol stability annotations (v0.14.x PER-SYMBOL-CLASSIFY)\r\n'
    b' *\r\n'
    b' * The file-level header comment carries the full list.  In brief:\r\n'
    b' *   - `name()` / `open()` / `close()` \xe2\x80\x94 STABLE (IPlugin overrides).\r\n'
    b' *   - `open(cfg)` / `send()` / `set_on_message()` / `set_on_state()` \xe2\x80\x94 STABLE.\r\n'
    b' *     The four-member channel contract is the load-bearing data-plane seam;\r\n'
    b' *     both in-tree backends (`SctpDataChannel` id `"sctp"` and\r\n'
    b' *     `WebTransportDataChannel` id `"web_transport"`, v0.14.0-beta.1) consume it.\r\n'
    b' *   - `IDataChannelFactory::id()` / `display_name()` / `create()` \xe2\x80\x94 STABLE.\r\n'
    b' *   - `SimpleDataChannelFactory` \xe2\x80\x94 STABLE (template helper). */'
)


# ---------------------------------------------------------------------------
# transport.hpp — ITransport
# ---------------------------------------------------------------------------
# ITransport does not have a class-level docstring; insert one between the
# "// ITransport" section divider and the class definition.
_transport_path = 'src/plugins/include/nimrtc/plugins/transport.hpp'

_transport_old = (
    b'// -----------------------------------------------------------------------\r\n'
    b'// ITransport\r\n'
    b'// -----------------------------------------------------------------------\r\n'
    b'\r\n'
    b'class NIMRTC_API_STABLE ITransport : public IPlugin {'
)
_transport_new = (
    b'// -----------------------------------------------------------------------\r\n'
    b'// ITransport\r\n'
    b'// -----------------------------------------------------------------------\r\n'
    b'\r\n'
    b'/** Plugin-seam network-transport interface (ADR-001 \xe2\x80\x9cSwapping the\r\n'
    b' * transport\xe2\x80\x9d).  Every in-tree transport backend (ICE, raw UDP,\r\n'
    b' * in-process pipe, WebSocket) implements exactly this interface.\r\n'
    b' *\r\n'
    b' * Thread safety:\r\n'
    b' *   - `set_callbacks()` / `set_on_consent_lost()` are called from a single\r\n'
    b' *     thread (the engine thread) before `open()`.\r\n'
    b' *   - `send()` is thread-safe \xe2\x80\x94 implementations must queue outbound data.\r\n'
    b' *   - `recv()` may be called concurrently from different threads.\r\n'
    b' *     `open()` / `close()` are called from a single thread (the engine thread).\r\n'
    b' *\r\n'
    b' * ## Per-symbol stability annotations (v0.14.x PER-SYMBOL-CLASSIFY)\r\n'
    b' *\r\n'
    b' * The file-level header comment carries the full list.  In brief:\r\n'
    b' *   - `set_callbacks()` / `send()` / `recv()` / `local_addr()` /\r\n'
    b' *     `remote_addr()` \xe2\x80\x94 STABLE.  The five-member contract is the load-bearing\r\n'
    b' *     network seam; every custom transport impl uses it.\r\n'
    b' *   - `set_on_consent_lost()` \xe2\x80\x94 STABLE (RFC 7675 / RFC 8445 \xc2\xa710 hook).\r\n'
    b' *   - `ITransportFactory` / `SimpleTransportFactory` \xe2\x80\x94 STABLE. */\r\n'
    b'\r\n'
    b'class NIMRTC_API_STABLE ITransport : public IPlugin {'
)


# ---------------------------------------------------------------------------
# engine.hpp — NimRTCEngine
# ---------------------------------------------------------------------------
# NimRTCEngine also lacks a class-level docstring; insert one before the
# "// ---------------------------------------------------------------------------" divider that precedes the class definition.
_engine_path = 'src/engine/include/nimrtc/engine/engine.hpp'

_engine_old = (
    b'// -----------------------------------------------------------------------\r\n'
    b'// NimRTCEngine\r\n'
    b'// -----------------------------------------------------------------------\r\n'
    b'\r\n'
    b'class NIMRTC_API_STABLE NimRTCEngine {'
)
_engine_new = (
    b'// -----------------------------------------------------------------------\r\n'
    b'// NimRTCEngine\r\n'
    b'// -----------------------------------------------------------------------\r\n'
    b'\r\n'
    b'/** Main entry point for the NimRTC stack.\r\n'
    b' *\r\n'
    b' * ## Per-symbol stability annotations (v0.14.x PER-SYMBOL-CLASSIFY)\r\n'
    b' *\r\n'
    b' * The file-level header comment carries the full per-method list.\r\n'
    b' * In brief:\r\n'
    b' *   - Lifecycle (`open()` / `pre_open()` / `close()` / `tick()`) \xe2\x80\x94 STABLE.\r\n'
    b' *   - Core payload (`send_audio()` / `create_offer()` /\r\n'
    b' *     `process_remote_sdp()` / `create_data_channel()`) \xe2\x80\x94 STABLE.\r\n'
    b' *   - Video (`start_video()` / `stop_video()` / `send_video()`) \xe2\x80\x94 UNSTABLE.\r\n'
    b' *   - Accessors (`set_on_*()` / ICE diagnostics / plugin handles) \xe2\x80\x94 STABLE.\r\n'
    b' *\r\n'
    b' * The macro expands to nothing in default builds; it becomes load-bearing\r\n'
    b' * when `NIMRTC_ENABLE_API_STABILITY_ANNOTATIONS=1` is set, at which point\r\n'
    b' * `NIMRTC_API_UNSTABLE` emits the cross-compiler `[[deprecated]]` attribute.\r\n'
    b' * Per ADR-015 \xc2\xa7Decision 3: v0.x \xe2\x86\x92 v1.0.0 may mark DEPRECATED\r\n'
    b' * but does not REMOVE any symbol. */\r\n'
    b'\r\n'
    b'class NIMRTC_API_STABLE NimRTCEngine {'
)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    files = [
        (_datachannel_path, _datachannel_old, _datachannel_new),
        (_transport_path,   _transport_old,   _transport_new),
        (_engine_path,     _engine_old,     _engine_new),
    ]

    ok = True
    for path, old, new in files:
        if not os.path.exists(path):
            print(f'WARN: {path} not found — skipping')
            continue
        if not edit_file(path, old, new):
            ok = False

    if not ok:
        sys.exit(1)


if __name__ == '__main__':
    main()

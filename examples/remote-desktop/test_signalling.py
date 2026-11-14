#!/usr/bin/env python3
"""Self-running test for the reference signalling service.

Covers the pure-logic parts:

  - the pairing code lifecycle (issue / validate / expiry / one-shot),
  - the RFC 7635 style TURN credential (issuance, validation, expiry,
    tamper detection),
  - the WebSocket message routing (pairing, room join, candidate
    relay, TURN issuance, ICE restart, reconnect, error paths).

It also runs a live end-to-end check over a real WebSocket connection
to the service, so the hand-rolled RFC 6455 layer is exercised too.

Run it with:

    python examples/remote-desktop/test_signalling.py

The script exits non-zero when a check fails.  No pytest dependency —
the repository's convention for self-running scripts.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wsproto  # noqa: E402  (path shim above)
from signalling_core import (  # noqa: E402
    PairingStore,
    SignallingService,
    TurnCredentialIssuer,
)

_FAILURES = []
_CHECKS = 0


def check(condition: bool, label: str) -> None:
    """Record one check.  A failure does not stop the run — the report
    at the end lists every failure."""
    global _CHECKS
    _CHECKS += 1
    if condition:
        print(f"  PASS  {label}")
    else:
        print(f"  FAIL  {label}")
        _FAILURES.append(label)


def section(title: str) -> None:
    print(f"\n== {title} ==")


# ---------------------------------------------------------------------------
# Pairing codes
# ---------------------------------------------------------------------------


def test_pairing_codes() -> None:
    section("pairing codes")
    store = PairingStore(ttl_s=300.0)
    t0 = 1_000_000.0

    record = store.issue(now=t0)
    check(len(record.code) == 6 and record.code.isdigit(),
          "the pairing code is six decimal digits")
    check(store.validate(record.code, now=t0) is not None,
          "a fresh pairing code validates")
    check(store.validate("999999", now=t0) is None,
          "an unknown pairing code does not validate")

    # Expiry.
    check(store.validate(record.code, now=t0 + 299.0) is not None,
          "the code is still valid one second before expiry")
    check(store.validate(record.code, now=t0 + 300.0) is None,
          "the code is refused at its expiry instant")
    check(store.validate(record.code, now=t0 + 301.0) is None,
          "the code is refused after its expiry instant")

    # The pairing exchange consumes the code.
    record2 = store.issue(now=t0)
    rec, cred = store.pair(record2.code, now=t0)
    check(rec is not None and cred is not None, "pairing returns a record and a credential")
    check(cred is not None and cred.role == "viewer", "the pairing credential is a viewer credential")
    check(store.validate(record2.code, now=t0) is None,
          "a consumed pairing code cannot be used again")
    rec2, cred2 = store.pair(record2.code, now=t0)
    check(rec2 is None and cred2 is None, "replaying a consumed code yields nothing")

    # Registration does not consume the code, but it is one-shot too.
    record3 = store.issue(now=t0)
    rec3, dev_cred = store.register_device(record3.code, now=t0)
    check(rec3 is not None and dev_cred is not None, "the device registers with the code")
    check(dev_cred is not None and dev_cred.role == "device", "the device credential is a device credential")
    rec3b, dev_cred2 = store.register_device(record3.code, now=t0)
    check(rec3b is None and dev_cred2 is None, "a code cannot register two devices")
    check(store.validate(record3.code, now=t0) is not None,
          "registration alone does not consume the code")

    # The device and the viewer share the room.
    rec4, view_cred = store.pair(record3.code, now=t0)
    check(view_cred is not None and dev_cred is not None
          and view_cred.room_id == dev_cred.room_id,
          "the device and the viewer land in the same room")
    check(view_cred is not None and dev_cred is not None
          and view_cred.token != dev_cred.token,
          "the device and the viewer hold different credentials")

    # Expiry sweep.
    store2 = PairingStore(ttl_s=10.0)
    store2.issue(now=t0)
    store2.issue(now=t0)
    removed = store2.expire(now=t0 + 11.0)
    check(removed == 2, "the expiry sweep removes the expired records")


# ---------------------------------------------------------------------------
# TURN credentials
# ---------------------------------------------------------------------------


def test_turn_credentials() -> None:
    section("TURN credentials (RFC 7635 style)")
    issuer = TurnCredentialIssuer(b"shared-secret", ttl_s=600.0,
                                  url="turn:turn.example:3478?transport=udp")
    t0 = 1_000_000.0

    cred = issuer.issue("viewer:peer-1", now=t0)
    check(cred.username.startswith(str(int(t0 + 600.0)) + ":"),
          "the username carries the expiry timestamp")
    check(":viewer:peer-1" in cred.username, "the username carries the user id")
    check(issuer.validate(cred.username, cred.password, now=t0) is True,
          "a freshly issued credential validates")
    check(issuer.validate(cred.username, cred.password, now=t0 + 599.0) is True,
          "the credential is valid one second before expiry")
    check(issuer.validate(cred.username, cred.password, now=t0 + 601.0) is False,
          "the credential is refused after its expiry")

    # Tampering.
    check(issuer.validate(cred.username, "not-the-password", now=t0) is False,
          "a wrong password is refused")
    tampered_user = cred.username.replace("viewer", "device")
    check(issuer.validate(tampered_user, cred.password, now=t0) is False,
          "a tampered username is refused")
    check(issuer.validate("no-colon", cred.password, now=t0) is False,
          "a username without a timestamp is refused")
    check(issuer.validate("not-a-number:x", cred.password, now=t0) is False,
          "a username with a non-numeric timestamp is refused")

    # Two issuers with different secrets do not accept each other's
    # credentials, but two issuers with the same secret do.
    other = TurnCredentialIssuer(b"another-secret", ttl_s=600.0)
    check(other.validate(cred.username, cred.password, now=t0) is False,
          "a different shared secret refuses the credential")
    same = TurnCredentialIssuer(b"shared-secret", ttl_s=600.0)
    check(same.validate(cred.username, cred.password, now=t0) is True,
          "the same shared secret accepts the credential (the TURN server's check)")

    # Determinism: the same username always yields the same password.
    check(issuer.issue("viewer:peer-1", now=t0).password == cred.password,
          "the password is a pure function of (secret, username)")


# ---------------------------------------------------------------------------
# Message routing
# ---------------------------------------------------------------------------


def test_routing() -> None:
    section("message routing")
    service = SignallingService()
    device = "peer-device"
    viewer = "peer-viewer"

    out = service.handle(device, {"type": "pairing_request"})
    check(len(out) == 1 and out[0][0] == device, "the pairing request is answered to the caller")
    check(out[0][1]["type"] == "pairing_code", "the answer is a pairing_code")
    code = out[0][1]["code"]

    out = service.handle(device, {"type": "device_register", "code": code})
    check(out[0][1]["type"] == "device_registered", "the device registers")
    device_cred = out[0][1]["credential"]
    room_id = out[0][1]["room_id"]

    out = service.handle(viewer, {"type": "pair", "code": code})
    check(out[0][1]["type"] == "paired", "the viewer pairs with the code")
    viewer_cred = out[0][1]["credential"]
    check(out[0][1]["room_id"] == room_id, "both peers land in the same room")

    # A wrong credential cannot join.
    out = service.handle("peer-stranger", {"type": "join", "credential": "cred-bogus"})
    check(out[0][1].get("code") == "unauthorized", "an unknown credential cannot join")

    # The device joins first, then the viewer.
    out = service.handle(device, {"type": "join", "credential": device_cred})
    check(out[0][1]["type"] == "joined" and out[0][1]["role"] == "device",
          "the device joins as the device")
    check(out[0][1]["peer_present"] is False, "the device sees no peer yet")

    out = service.handle(viewer, {"type": "join", "credential": viewer_cred})
    check(out[0][1]["type"] == "joined" and out[0][1]["role"] == "viewer",
          "the viewer joins as the viewer")
    check(out[0][1]["peer_present"] is True, "the viewer sees the device")
    check(len(out) == 2 and out[1][0] == device and out[1][1]["type"] == "peer_joined",
          "the device is notified that the viewer joined")

    # A second viewer cannot take the viewer's slot.
    out = service.handle(viewer, {"type": "join", "credential": viewer_cred})
    check(out[0][1]["type"] == "joined", "re-joining with the same credential is idempotent")

    # Candidate relay, in both directions.
    out = service.handle(viewer, {"type": "candidate", "candidate": "a=candidate:1 1 UDP 1 10.0.0.1 5000 typ host"})
    check(len(out) == 1 and out[0][0] == device, "the viewer's candidate is relayed to the device")
    check(out[0][1]["candidate"].startswith("a=candidate:"), "the candidate payload survives the relay")
    check(out[0][1]["from"] == "viewer", "the relay stamps the sender role")

    out = service.handle(device, {"type": "candidate", "candidate": "a=candidate:1 1 UDP 1 10.0.0.2 5001 typ host"})
    check(len(out) == 1 and out[0][0] == viewer, "the device's candidate is relayed to the viewer")

    # Offer / answer relay.
    out = service.handle(viewer, {"type": "offer", "sdp": "v=0\r\n"})
    check(out[0][0] == device and out[0][1]["sdp"] == "v=0\r\n", "the offer is relayed to the device")
    out = service.handle(device, {"type": "answer", "sdp": "v=0\r\nanswer\r\n"})
    check(out[0][0] == viewer, "the answer is relayed to the viewer")

    # ICE restart and reconnect relay.
    out = service.handle(device, {"type": "ice_restart", "sdp": "v=0\r\nnew-ice\r\n"})
    check(out[0][0] == viewer and out[0][1]["type"] == "ice_restart",
          "an ICE restart is relayed to the viewer")
    out = service.handle(viewer, {"type": "reconnect"})
    check(out[0][0] == device and out[0][1]["type"] == "reconnect",
          "a reconnect is relayed to the device")

    # TURN issuance.
    out = service.handle(viewer, {"type": "turn_request"})
    check(out[0][1]["type"] == "turn_credentials", "the TURN request is answered")
    check(":" in out[0][1]["username"], "the TURN username carries a timestamp")
    check(out[0][1]["ttl"] > 0, "the TURN credential carries a TTL")

    # An unknown message type is an error, not a crash.
    out = service.handle(viewer, {"type": "no_such_type"})
    check(out[0][1].get("code") == "unknown_type", "an unknown message type is refused")
    out = service.handle(viewer, {})
    check(out[0][1].get("code") == "bad_request", "a message without a type is refused")

    # A peer that has not joined cannot relay.
    out = service.handle("peer-lonely", {"type": "candidate", "candidate": "x"})
    check(out[0][1].get("code") == "not_joined", "a peer that has not joined cannot relay")

    # Disconnect notifies the other peer.
    out = service.disconnect(viewer)
    check(len(out) == 1 and out[0][0] == device and out[0][1]["type"] == "peer_left",
          "the device is notified when the viewer disconnects")


# ---------------------------------------------------------------------------
# Live WebSocket round trip
# ---------------------------------------------------------------------------


async def _run_live_check() -> None:
    section("live WebSocket round trip")
    from signalling import SignalingServer

    service = SignallingService()
    server = SignalingServer(service)

    async def on_client(reader, writer):
        await server.handle_connection(reader, writer)

    listener = await asyncio.start_server(on_client, "127.0.0.1", 0)
    port = listener.sockets[0].getsockname()[1]

    device = await wsproto.client_connect("127.0.0.1", port, "/")
    viewer = await wsproto.client_connect("127.0.0.1", port, "/")

    async def recv_json(conn, timeout=5.0):
        raw = await asyncio.wait_for(conn.recv_text(), timeout)
        return json.loads(raw)

    # Device: request a pairing code.
    await device.send_text(json.dumps({"type": "pairing_request"}))
    msg = await recv_json(device)
    check(msg["type"] == "pairing_code", "the live service issues a pairing code")
    code = msg["code"]

    # Device: register.
    await device.send_text(json.dumps({"type": "device_register", "code": code}))
    msg = await recv_json(device)
    check(msg["type"] == "device_registered", "the live service registers the device")
    device_cred = msg["credential"]

    # Viewer: pair.
    await viewer.send_text(json.dumps({"type": "pair", "code": code}))
    msg = await recv_json(viewer)
    check(msg["type"] == "paired", "the live service pairs the viewer")
    viewer_cred = msg["credential"]

    # Both join.
    await device.send_text(json.dumps({"type": "join", "credential": device_cred}))
    msg = await recv_json(device)
    check(msg["type"] == "joined", "the live device joins the room")

    await viewer.send_text(json.dumps({"type": "join", "credential": viewer_cred}))
    msg = await recv_json(viewer)
    check(msg["type"] == "joined", "the live viewer joins the room")
    msg = await recv_json(device)
    check(msg["type"] == "peer_joined", "the live device is notified of the viewer")

    # Candidate relay over the wire.
    await viewer.send_text(json.dumps({
        "type": "candidate", "candidate": "a=candidate:1 1 UDP 1 10.0.0.1 5000 typ host",
    }))
    msg = await recv_json(device)
    check(msg["type"] == "candidate" and msg["candidate"].startswith("a=candidate:"),
          "the live service relays a candidate to the device")

    # TURN issuance over the wire.
    await viewer.send_text(json.dumps({"type": "turn_request"}))
    msg = await recv_json(viewer)
    check(msg["type"] == "turn_credentials" and ":" in msg["username"],
          "the live service issues a TURN credential")

    await device.close()
    await viewer.close()
    listener.close()
    await listener.wait_closed()


def main() -> int:
    print("NimRTC remote-desktop reference signalling — self-test")
    test_pairing_codes()
    test_turn_credentials()
    test_routing()
    try:
        asyncio.run(_run_live_check())
    except Exception as exc:  # noqa: BLE001 - the report must survive
        check(False, f"the live WebSocket check raised {exc!r}")

    print(f"\n{_CHECKS - len(_FAILURES)}/{_CHECKS} checks passed")
    if _FAILURES:
        print("FAILED:")
        for failure in _FAILURES:
            print(f"  - {failure}")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

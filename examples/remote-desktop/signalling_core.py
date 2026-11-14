"""Pure signalling logic for the NimRTC remote-desktop reference service.

Everything in this module is synchronous and side-effect free with
respect to the network: it owns the pairing codes, the session
credentials, the TURN credential issuance (RFC 7635 style) and the
room / message routing table.  ``signalling.py`` wraps it in an asyncio
WebSocket server; ``test_signalling.py`` exercises it directly.

The wire protocol implemented here is the one documented in
``docs/guides/remote-desktop.md`` — that guide is the canonical
description, this file is its executable form.

## Identity model

The *credential* is the identity.  A WebSocket address is not: the
same operator may reconnect from a different address (a new DHCP lease,
a VPN that changes the source port, a NAT rebind) and must keep its
session, while a peer that cannot present the right credential must not
be able to join at all.

The pairing flow is:

1. The device asks the service for a **pairing code**
   (``{"type": "pairing_request"}``).  The code is short-lived and is
   shown on the device's screen (or read out of its settings page).
2. The device registers with that code (``{"type": "device_register"}``)
   and receives a **device credential** plus the room id.
3. The operator exchanges the code for a **session credential**
   (``{"type": "pair"}``).  The code is consumed at this point, so it
   cannot be replayed.
4. Both peers join the room with their credential
   (``{"type": "join"}``).  The *viewer* is the offerer; the *device*
   answers.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import logging
import secrets
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

logger = logging.getLogger("signalling.core")

__all__ = [
    "PairingRecord",
    "SessionCredential",
    "TurnCredential",
    "PairingStore",
    "TurnCredentialIssuer",
    "Room",
    "SignallingService",
    "DEFAULT_PAIRING_TTL_S",
    "DEFAULT_SESSION_TTL_S",
    "DEFAULT_TURN_TTL_S",
]

#: How long a pairing code stays valid.  Long enough for a human to
#: type it, short enough that a code left on a screen is worthless.
DEFAULT_PAIRING_TTL_S = 300.0

#: How long a session credential stays valid.
DEFAULT_SESSION_TTL_S = 3600.0

#: How long an issued TURN credential stays valid.  RFC 7635 style:
#: the timestamp inside the username carries the expiry, so the TURN
#: server itself never has to be told about revocation.
DEFAULT_TURN_TTL_S = 3600.0

#: Default TURN relay advertised to clients.
DEFAULT_TURN_URL = "turn:127.0.0.1:3478?transport=udp"


def _now() -> float:
    """Wall-clock seconds.  Overridable in tests via the ``now`` arguments."""
    return time.time()


@dataclass
class PairingRecord:
    """A pending pairing request."""

    code: str
    device_id: str
    expires_at: float
    registered: bool = False
    consumed: bool = False
    device_credential: Optional[str] = None
    room_id: Optional[str] = None


@dataclass
class SessionCredential:
    """A credential that authorises one peer to one room."""

    token: str
    room_id: str
    role: str  # "device" | "viewer"
    device_id: str
    expires_at: float


@dataclass
class TurnCredential:
    """An RFC 7635 style TURN credential."""

    username: str
    password: str
    expires_at: float
    ttl_s: float


class PairingStore:
    """Issues and validates short-lived pairing codes."""

    def __init__(self, ttl_s: float = DEFAULT_PAIRING_TTL_S) -> None:
        self.ttl_s = ttl_s
        self._by_code: Dict[str, PairingRecord] = {}

    # ---- issuance --------------------------------------------------------

    def issue(self, now: Optional[float] = None, device_id: Optional[str] = None) -> PairingRecord:
        """Create a new pairing code.

        The code is six decimal digits — short enough to read off a
        screen and type, long enough that a brute-force attempt needs
        the rate limiting the service layer adds on top.
        """
        now = _now() if now is None else now
        # Retry on the (vanishingly unlikely) collision.
        for _ in range(64):
            code = f"{secrets.randbelow(1_000_000):06d}"
            if code not in self._by_code:
                break
        else:  # pragma: no cover - 64 consecutive collisions
            raise RuntimeError("cannot allocate a unique pairing code")
        record = PairingRecord(
            code=code,
            device_id=device_id or f"dev-{secrets.token_hex(8)}",
            expires_at=now + self.ttl_s,
        )
        self._by_code[code] = record
        return record

    # ---- validation ------------------------------------------------------

    def validate(self, code: str, now: Optional[float] = None) -> Optional[PairingRecord]:
        """Return the record for ``code`` if it is still usable, else None.

        A code is *usable* when it exists, has not expired, and has not
        already been consumed by a pairing exchange.  Registration
        (which proves the device possesses the code) does not consume
        it — only the operator's exchange does.
        """
        now = _now() if now is None else now
        record = self._by_code.get(code)
        if record is None:
            return None
        if record.consumed:
            return None
        if now >= record.expires_at:
            return None
        return record

    def register_device(
        self, code: str, now: Optional[float] = None
    ) -> Tuple[Optional[PairingRecord], Optional[SessionCredential]]:
        """Bind a device to a pairing code.

        Returns ``(record, credential)``; the credential is None when
        the code is unknown, expired, or already registered.
        """
        now = _now() if now is None else now
        record = self.validate(code, now)
        if record is None or record.registered:
            return None, None
        record.registered = True
        room_id = record.room_id or f"room-{secrets.token_hex(8)}"
        record.room_id = room_id
        credential = SessionCredential(
            token=f"cred-{secrets.token_urlsafe(24)}",
            room_id=room_id,
            role="device",
            device_id=record.device_id,
            expires_at=now + DEFAULT_SESSION_TTL_S,
        )
        record.device_credential = credential.token
        return record, credential

    def pair(self, code: str, now: Optional[float] = None) -> Tuple[Optional[PairingRecord], Optional[SessionCredential]]:
        """Exchange a pairing code for a viewer session credential.

        The code is consumed: it cannot be used a second time.
        """
        now = _now() if now is None else now
        record = self.validate(code, now)
        if record is None:
            return None, None
        record.consumed = True
        room_id = record.room_id or f"room-{secrets.token_hex(8)}"
        record.room_id = room_id
        credential = SessionCredential(
            token=f"cred-{secrets.token_urlsafe(24)}",
            room_id=room_id,
            role="viewer",
            device_id=record.device_id,
            expires_at=now + DEFAULT_SESSION_TTL_S,
        )
        return record, credential

    def expire(self, now: Optional[float] = None) -> int:
        """Drop expired records.  Returns the number of records removed."""
        now = _now() if now is None else now
        dead = [c for c, r in self._by_code.items() if now >= r.expires_at]
        for code in dead:
            del self._by_code[code]
        return len(dead)


class TurnCredentialIssuer:
    """Issues and validates RFC 7635 style TURN credentials.

    The *username* is ``<expiry-unix-timestamp>:<user-id>`` and the
    *password* is ``base64(HMAC-SHA1(shared-secret, username))``.  The
    TURN server recomputes the same HMAC over the username it receives
    and compares the result with the password the client presented, so
    no credential state has to be shared with the TURN server beyond
    the shared secret — and the credential expires on its own.
    """

    def __init__(
        self,
        secret: bytes,
        ttl_s: float = DEFAULT_TURN_TTL_S,
        url: str = DEFAULT_TURN_URL,
    ) -> None:
        if not secret:
            raise ValueError("the TURN shared secret must not be empty")
        self._secret = secret
        self.ttl_s = ttl_s
        self.url = url

    def issue(self, user_id: str, now: Optional[float] = None) -> TurnCredential:
        """Issue a credential for ``user_id``."""
        now = _now() if now is None else now
        expiry = int(now + self.ttl_s)
        username = f"{expiry}:{user_id}"
        return TurnCredential(
            username=username,
            password=self._password_for(username),
            expires_at=float(expiry),
            ttl_s=self.ttl_s,
        )

    def _password_for(self, username: str) -> str:
        digest = hmac.new(self._secret, username.encode("utf-8"), hashlib.sha1).digest()
        return base64.b64encode(digest).decode("ascii")

    def validate(
        self, username: str, password: str, now: Optional[float] = None
    ) -> bool:
        """True when ``username`` / ``password`` is a live, matching pair."""
        now = _now() if now is None else now
        expiry, sep, _user_id = username.partition(":")
        if not sep:
            return False
        try:
            expiry_int = int(expiry)
        except ValueError:
            return False
        if now >= expiry_int:
            return False
        expected = self._password_for(username)
        return hmac.compare_digest(expected, password)


@dataclass
class Room:
    """One signalling room: exactly one device and one viewer."""

    room_id: str
    device_id: str = ""
    credentials: Dict[str, SessionCredential] = field(default_factory=dict)
    peers: Dict[str, str] = field(default_factory=dict)  # peer_id → role

    def role_of(self, peer_id: str) -> Optional[str]:
        return self.peers.get(peer_id)

    def peer_of_role(self, role: str) -> Optional[str]:
        for peer_id, peer_role in self.peers.items():
            if peer_role == role:
                return peer_id
        return None


class SignallingService:
    """The message router.

    The router is deliberately transport-free: :meth:`handle` takes a
    peer id and an inbound message and returns the messages that have
    to be delivered, as ``(target_peer_id, message)`` pairs.  The
    asyncio server in ``signalling.py`` only moves bytes; every policy
    decision lives here, which is what makes it testable.
    """

    def __init__(
        self,
        pairing_store: Optional[PairingStore] = None,
        turn_issuer: Optional[TurnCredentialIssuer] = None,
        session_ttl_s: float = DEFAULT_SESSION_TTL_S,
        now_fn=_now,
    ) -> None:
        self.pairing = pairing_store or PairingStore()
        self.turn = turn_issuer or TurnCredentialIssuer(secrets.token_bytes(32))
        self.session_ttl_s = session_ttl_s
        self._now = now_fn
        self._credentials: Dict[str, SessionCredential] = {}
        self._rooms: Dict[str, Room] = {}
        self._peer_credential: Dict[str, str] = {}  # peer_id → credential token
        self._peer_room: Dict[str, str] = {}  # peer_id → room id

    # ---- helpers ---------------------------------------------------------

    def _error(self, code: str, message: str) -> Dict:
        return {"type": "error", "code": code, "message": message}

    def _room(self, room_id: str) -> Room:
        room = self._rooms.get(room_id)
        if room is None:
            room = Room(room_id=room_id)
            self._rooms[room_id] = room
        return room

    def credential_for(self, token: str) -> Optional[SessionCredential]:
        """Return the credential for ``token`` if it exists and is live."""
        cred = self._credentials.get(token)
        if cred is None:
            return None
        if self._now() >= cred.expires_at:
            return None
        return cred

    def room_of_peer(self, peer_id: str) -> Optional[Room]:
        room_id = self._peer_room.get(peer_id)
        return self._rooms.get(room_id) if room_id else None

    # ---- the router ------------------------------------------------------

    def handle(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        """Route one inbound message.

        Returns the list of ``(target_peer_id, outbound_message)`` pairs.
        A message addressed to the peer itself uses the peer id as the
        target, which keeps the transport layer trivial: it looks up
        the target's send queue and writes the message.
        """
        mtype = message.get("type")
        if not isinstance(mtype, str):
            return [(peer_id, self._error("bad_request", "missing 'type' field"))]

        handler = getattr(self, f"_on_{mtype}", None)
        if handler is None:
            return [(peer_id, self._error("unknown_type", f"unknown message type '{mtype}'"))]
        return handler(peer_id, message)

    # ---- pairing / registration -----------------------------------------

    def _on_pairing_request(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        record = self.pairing.issue(now=self._now())
        logger.info(
            "pairing code %s issued for device %s (expires in %.0f s)",
            record.code, record.device_id, self.pairing.ttl_s,
        )
        return [
            (
                peer_id,
                {
                    "type": "pairing_code",
                    "code": record.code,
                    "device_id": record.device_id,
                    "expires_at": record.expires_at,
                },
            )
        ]

    def _on_device_register(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        code = str(message.get("code", ""))
        record, credential = self.pairing.register_device(code, now=self._now())
        if record is None or credential is None:
            return [
                (
                    peer_id,
                    self._error(
                        "pairing_invalid",
                        "the pairing code is unknown, expired, or already registered",
                    ),
                )
            ]
        self._credentials[credential.token] = credential
        room = self._room(credential.room_id)
        room.device_id = record.device_id
        room.credentials[credential.token] = credential
        return [
            (
                peer_id,
                {
                    "type": "device_registered",
                    "device_id": record.device_id,
                    "room_id": credential.room_id,
                    "credential": credential.token,
                    "expires_at": credential.expires_at,
                },
            )
        ]

    def _on_pair(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        code = str(message.get("code", ""))
        record, credential = self.pairing.pair(code, now=self._now())
        if record is None or credential is None:
            return [
                (
                    peer_id,
                    self._error(
                        "pairing_invalid",
                        "the pairing code is unknown, expired, or already used",
                    ),
                )
            ]
        self._credentials[credential.token] = credential
        room = self._room(credential.room_id)
        room.device_id = record.device_id
        room.credentials[credential.token] = credential
        return [
            (
                peer_id,
                {
                    "type": "paired",
                    "credential": credential.token,
                    "room_id": credential.room_id,
                    "device_id": record.device_id,
                    "expires_at": credential.expires_at,
                },
            )
        ]

    # ---- room membership -------------------------------------------------

    def _on_join(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        token = str(message.get("credential", ""))
        credential = self.credential_for(token)
        if credential is None:
            return [
                (
                    peer_id,
                    self._error("unauthorized", "unknown or expired session credential"),
                )
            ]
        room = self._room(credential.room_id)
        # A room holds exactly one device and one viewer.
        existing = room.peer_of_role(credential.role)
        if existing is not None and existing != peer_id:
            return [
                (
                    peer_id,
                    self._error("room_full", f"the room already has a {credential.role}"),
                )
            ]
        room.peers[peer_id] = credential.role
        self._peer_credential[peer_id] = token
        self._peer_room[peer_id] = credential.room_id

        other_role = "viewer" if credential.role == "device" else "device"
        other_peer = room.peer_of_role(other_role)
        out = [
            (
                peer_id,
                {
                    "type": "joined",
                    "role": credential.role,
                    "room_id": credential.room_id,
                    "device_id": credential.device_id,
                    "peer_present": other_peer is not None,
                },
            )
        ]
        if other_peer is not None:
            out.append(
                (
                    other_peer,
                    {"type": "peer_joined", "role": credential.role},
                )
            )
        return out

    def _on_leave(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        return self._leave(peer_id)

    def disconnect(self, peer_id: str) -> List[Tuple[str, Dict]]:
        """Called by the transport when a peer goes away."""
        return self._leave(peer_id)

    def _leave(self, peer_id: str) -> List[Tuple[str, Dict]]:
        room = self.room_of_peer(peer_id)
        if room is None:
            return []
        role = room.role_of(peer_id)
        room.peers.pop(peer_id, None)
        self._peer_room.pop(peer_id, None)
        self._peer_credential.pop(peer_id, None)
        if role is None:
            return []
        other_role = "viewer" if role == "device" else "device"
        other_peer = room.peer_of_role(other_role)
        if other_peer is None:
            return []
        return [(other_peer, {"type": "peer_left", "role": role})]

    # ---- relay -----------------------------------------------------------

    def _relay(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        """Forward a media-plane message to the other peer in the room."""
        room = self.room_of_peer(peer_id)
        if room is None:
            return [(peer_id, self._error("not_joined", "join a room first"))]
        role = room.role_of(peer_id)
        if role is None:
            return [(peer_id, self._error("not_joined", "join a room first"))]
        other_role = "viewer" if role == "device" else "device"
        other_peer = room.peer_of_role(other_role)
        if other_peer is None:
            # The peer is not there yet.  The message is *not* buffered:
            # a stale offer must never be applied after an ICE restart.
            return [(peer_id, self._error("peer_absent", "the other peer has not joined"))]
        out = dict(message)
        out["from"] = role
        return [(other_peer, out)]

    def _on_offer(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        return self._relay(peer_id, message)

    def _on_answer(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        return self._relay(peer_id, message)

    def _on_candidate(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        return self._relay(peer_id, message)

    def _on_ice_restart(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        return self._relay(peer_id, message)

    def _on_reconnect(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        return self._relay(peer_id, message)

    def _on_bye(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        return self._leave(peer_id)

    # ---- TURN ------------------------------------------------------------

    def _on_turn_request(self, peer_id: str, message: Dict) -> List[Tuple[str, Dict]]:
        """Issue a short-lived TURN credential for the calling peer."""
        room = self.room_of_peer(peer_id)
        role = room.role_of(peer_id) if room else "unknown"
        credential = self.turn.issue(f"{role}:{peer_id}", now=self._now())
        return [
            (
                peer_id,
                {
                    "type": "turn_credentials",
                    "username": credential.username,
                    "password": credential.password,
                    "url": self.turn.url,
                    "ttl": credential.ttl_s,
                    "expires_at": credential.expires_at,
                },
            )
        ]

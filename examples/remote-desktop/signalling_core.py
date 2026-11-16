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
    "SessionLimits",
    "SessionPolicy",
    "PolicyViolation",
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

#: How many sessions may be live at once, by default.
DEFAULT_MAX_CONCURRENT_SESSIONS = 64

#: How long a session may sit idle (no relayed media-plane traffic)
#: before it is dropped, by default.  Zero disables the idle timeout.
DEFAULT_IDLE_TIMEOUT_S = 300.0

#: How long a session may live in total, by default.  Zero disables
#: the hard session lifetime.
DEFAULT_SESSION_LIFETIME_S = 7200.0


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
        self, code: str, now: Optional[float] = None, room_id: Optional[str] = None
    ) -> Tuple[Optional[PairingRecord], Optional[SessionCredential]]:
        """Bind a device to a pairing code.

        Returns ``(record, credential)``; the credential is None when
        the code is unknown, expired, or already registered.

        ``room_id`` pins the room the device joins.  A device that asks
        for a *second* code (the first one expired, or a new operator
        wants in) passes the room it already owns, so re-registering
        does not orphan the room and split the pair in two.
        """
        now = _now() if now is None else now
        record = self.validate(code, now)
        if record is None or record.registered:
            return None, None
        record.registered = True
        room_id = room_id or record.room_id or f"room-{secrets.token_hex(8)}"
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
class SessionLimits:
    """The limits a session has to live within.

    Every limit is a plain number, and every one of them is optional:
    a limit of zero (or ``None``) means "no limit of this kind".  That
    makes the *policy* (which limits are actually armed) a separate
    concern from the *numbers* — see :class:`SessionPolicy`.
    """

    #: How many sessions may be live at the same time.  Zero disables
    #: the cap.
    max_concurrent_sessions: int = DEFAULT_MAX_CONCURRENT_SESSIONS

    #: How long a session may live in total, in seconds, counted from
    #: the moment its first peer joined.  Zero disables the hard
    #: lifetime.
    session_lifetime_s: float = DEFAULT_SESSION_LIFETIME_S

    #: How long a session may sit idle — with no relayed media-plane
    #: traffic — before it is dropped.  Zero disables the idle timeout.
    idle_timeout_s: float = DEFAULT_IDLE_TIMEOUT_S

    #: How many rooms may be live at the same time.  Zero disables the
    #: cap.  A room is live while at least one of its peers is joined.
    max_concurrent_rooms: int = 0

    def __post_init__(self) -> None:
        if self.max_concurrent_sessions < 0:
            raise ValueError("max_concurrent_sessions must not be negative")
        if self.max_concurrent_rooms < 0:
            raise ValueError("max_concurrent_rooms must not be negative")
        if self.session_lifetime_s < 0:
            raise ValueError("session_lifetime_s must not be negative")
        if self.idle_timeout_s < 0:
            raise ValueError("idle_timeout_s must not be negative")


class PolicyViolation:
    """The reasons a session can be refused or torn down.

    The values are stable strings: they travel on the wire in the
    ``code`` field of an error message, so a client can branch on them.
    """

    #: Too many sessions are live at once.
    SESSION_LIMIT = "session_limit"

    #: The session has been alive longer than its hard lifetime.
    SESSION_EXPIRED = "session_expired"

    #: The session has been idle for longer than its idle timeout.
    SESSION_IDLE = "session_idle"

    #: The administrative kill switch is engaged.
    KILL_SWITCH = "kill_switch"

    #: Too many rooms are live at once.
    ROOM_LIMIT = "room_limit"


@dataclass
class SessionPolicy:
    """The live-session bookkeeping and the policy decisions on it.

    The policy owns three things:

    1. **The kill switch.**  An administrator can engage it, and every
       subsequent join is refused.  This is the "stop the bleeding"
       lever: it takes effect immediately and without unwinding the
       sessions that are already up.
    2. **The concurrency cap.**  A new session is refused when the
       number of live sessions is already at the cap.
    3. **The timeouts.**  A session dies when it has been alive longer
       than its hard lifetime, or when it has been idle for longer than
       its idle timeout.

    The policy is *transport-free* and *side-effect free* with respect
    to the network: it answers questions ("may this peer join?",
    "is this session still alive?") and the caller acts on the answer.
    """

    limits: SessionLimits = field(default_factory=SessionLimits)

    #: When the kill switch was engaged (None when it is not).
    kill_switch_engaged_at: Optional[float] = None

    #: The reason the kill switch carries, reported to the clients.
    kill_switch_reason: str = "the service is administratively paused"

    # ---- the live sessions ----------------------------------------------

    def _session_table(self) -> Dict[str, Dict]:
        """The live-session table.

        The owner (:class:`SignallingService`) binds this to its own
        table, so the policy and the service can never disagree about
        which sessions are up.
        """
        raise NotImplementedError

    # ---- the kill switch -------------------------------------------------

    @property
    def kill_switch_engaged(self) -> bool:
        """True iff the administrative kill switch is engaged."""
        return self.kill_switch_engaged_at is not None

    def engage_kill_switch(self, now: Optional[float] = None, reason: str = "") -> None:
        """Engage the kill switch.  New joins are refused from now on.

        The sessions that are already up are *not* torn down: the
        switch is a brake on new work, and tearing down live sessions
        is a separate, deliberate act (``kill_all_sessions``).
        """
        now = _now() if now is None else now
        self.kill_switch_engaged_at = now
        if reason:
            self.kill_switch_reason = reason
        logger.warning("the administrative kill switch is engaged: %s", self.kill_switch_reason)

    def release_kill_switch(self) -> None:
        """Release the kill switch.  Joins are accepted again."""
        if self.kill_switch_engaged_at is not None:
            logger.info("the administrative kill switch is released")
        self.kill_switch_engaged_at = None

    # ---- the decisions ---------------------------------------------------

    def check_join(self, now: Optional[float] = None) -> Optional[str]:
        """Can a new session be admitted?

        Returns ``None`` when the join is allowed, else the policy
        violation code that explains the refusal.
        """
        now = _now() if now is None else now
        if self.kill_switch_engaged:
            return PolicyViolation.KILL_SWITCH
        limit = self.limits.max_concurrent_sessions
        if limit > 0 and len(self._live_sessions(now)) >= limit:
            return PolicyViolation.SESSION_LIMIT
        return None

    def check_room_budget(self, now: Optional[float] = None) -> Optional[str]:
        """Can another room be admitted?

        Returns ``None`` when the room is allowed, else the policy
        violation code.
        """
        now = _now() if now is None else now
        limit = self.limits.max_concurrent_rooms
        if limit > 0 and len(self._live_rooms(now)) >= limit:
            return PolicyViolation.ROOM_LIMIT
        return None

    def session_expired(self, session: Dict, now: Optional[float] = None) -> Optional[str]:
        """Has this session outlived its welcome?

        Returns ``None`` when the session is still alive, else the
        policy violation code.
        """
        now = _now() if now is None else now
        lifetime = self.limits.session_lifetime_s
        if lifetime > 0:
            started = session.get("started_at")
            if started is not None and now >= started + lifetime:
                return PolicyViolation.SESSION_EXPIRED
        idle = self.limits.idle_timeout_s
        if idle > 0:
            last = session.get("last_activity")
            if last is not None and now >= last + idle:
                return PolicyViolation.SESSION_IDLE
        return None

    # ---- the live set ----------------------------------------------------

    def _session_table(self) -> Dict[str, Dict]:
        """The live-session table.

        The owner (:class:`SignallingService`) binds this to its own
        table, so the policy and the service can never disagree about
        which sessions are up.
        """
        raise NotImplementedError

    def _live_sessions(self, now: float) -> List[str]:
        """The session ids that are alive at ``now``."""
        alive = []
        for sid, session in self._session_table().items():
            if self.session_expired(session, now) is None:
                alive.append(sid)
        return alive

    def _live_rooms(self, now: float) -> List[str]:
        """The room ids that hold at least one live peer."""
        rooms = set()
        for sid, session in self._session_table().items():
            if self.session_expired(session, now) is not None:
                continue
            room_id = session.get("room_id")
            if room_id:
                rooms.add(room_id)
        return list(rooms)


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
        limits: Optional[SessionLimits] = None,
    ) -> None:
        self.pairing = pairing_store or PairingStore()
        self.turn = turn_issuer or TurnCredentialIssuer(secrets.token_bytes(32))
        self.session_ttl_s = session_ttl_s
        self._now = now_fn
        self._credentials: Dict[str, SessionCredential] = {}
        self._rooms: Dict[str, Room] = {}
        self._peer_credential: Dict[str, str] = {}  # peer_id → credential token
        self._peer_room: Dict[str, str] = {}  # peer_id → room id

        # ---- the session policy -----------------------------------------
        #
        # The policy owns the live-session table, so it is handed a
        # back-reference to the service's table rather than a copy of
        # it: the two must never disagree about which sessions are up.
        self._sessions: Dict[str, Dict] = {}  # session_id → session record
        self._peer_session: Dict[str, str] = {}  # peer_id → session id
        self.policy = SessionPolicy(limits=limits or SessionLimits())
        self.policy._session_table = lambda: self._sessions  # type: ignore[assignment]

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

    # ---- the session policy ----------------------------------------------

    def session_of_peer(self, peer_id: str) -> Optional[Dict]:
        """The session record of the peer, or None when it has none."""
        session_id = self._peer_session.get(peer_id)
        return self._sessions.get(session_id) if session_id else None

    def _session_of_room(self, room_id: str) -> Optional[Dict]:
        """The session of the room, or None when the room has no session.

        A room carries exactly one session: the device and the viewer
        of a room share it, and the concurrency budget counts it once.
        """
        for session in self._sessions.values():
            if session.get("room_id") == room_id:
                return session
        return None

    def live_session_count(self, now: Optional[float] = None) -> int:
        """How many sessions are alive right now."""
        return len(self.policy._live_sessions(self._now() if now is None else now))

    def live_room_count(self, now: Optional[float] = None) -> int:
        """How many rooms hold at least one live peer right now."""
        return len(self.policy._live_rooms(self._now() if now is None else now))

    def engage_kill_switch(self, reason: str = "") -> None:
        """Engage the administrative kill switch.

        New joins are refused from now on.  The sessions that are
        already up keep running — tearing them down is a separate,
        deliberate act (:meth:`kill_all_sessions`).
        """
        self.policy.engage_kill_switch(now=self._now(), reason=reason)

    def release_kill_switch(self) -> None:
        """Release the administrative kill switch."""
        self.policy.release_kill_switch()

    def kill_all_sessions(self, reason: str = "the session was terminated by the operator") -> List[Tuple[str, Dict]]:
        """Tear down every live session.

        Returns the ``(peer_id, message)`` pairs the transport has to
        deliver: each peer in a killed session gets a ``bye``.
        """
        now = self._now()
        out: List[Tuple[str, Dict]] = []
        for session_id in list(self._sessions.keys()):
            out.extend(self._kill_session(session_id, now, reason))
        return out

    def reap_expired_sessions(self, now: Optional[float] = None) -> List[Tuple[str, Dict]]:
        """Drop the sessions that have outlived their welcome.

        A session is expired when it has been alive longer than the
        hard lifetime, or when it has been idle for longer than the
        idle timeout.  Returns the ``bye`` messages for the peers that
        were dropped.
        """
        now = self._now() if now is None else now
        out: List[Tuple[str, Dict]] = []
        for session_id in list(self._sessions.keys()):
            session = self._sessions.get(session_id)
            if session is None:
                continue
            reason = self.policy.session_expired(session, now)
            if reason is None:
                continue
            out.extend(self._kill_session(session_id, now, self._reason_text(reason)))
        return out

    @staticmethod
    def _reason_text(code: str) -> str:
        return {
            PolicyViolation.SESSION_EXPIRED: "the session reached its maximum lifetime",
            PolicyViolation.SESSION_IDLE: "the session was idle for too long",
            PolicyViolation.KILL_SWITCH: "the service is administratively paused",
            PolicyViolation.SESSION_LIMIT: "the service is at its session limit",
            PolicyViolation.ROOM_LIMIT: "the service is at its room limit",
        }.get(code, "the session was terminated")

    def _kill_session(
        self, session_id: str, now: float, reason: str
    ) -> List[Tuple[str, Dict]]:
        """Tear down one session and notify its peers."""
        session = self._sessions.pop(session_id, None)
        if session is None:
            return []
        out: List[Tuple[str, Dict]] = []
        for peer_id in list(session.get("peers", [])):
            self._peer_session.pop(peer_id, None)
            self._leave(peer_id)
            out.append((peer_id, {"type": "bye", "reason": reason}))
        logger.info("session %s torn down: %s", session_id, reason)
        return out

    def _touch(self, peer_id: str, now: Optional[float] = None) -> None:
        """Record activity on the peer's session.

        The idle timeout is measured from the last *media-plane*
        traffic, so the relay — not the join — is what keeps a session
        alive.
        """
        now = self._now() if now is None else now
        session = self.session_of_peer(peer_id)
        if session is not None:
            session["last_activity"] = now

    def _enforce_policy(self, peer_id: str) -> Optional[Dict]:
        """Check the peer against the policy.  Returns an error or None."""
        now = self._now()
        # An expired session is torn down *before* anything else, so the
        # peer gets a clear "your session ended" rather than a
        # confusing downstream failure.
        session = self.session_of_peer(peer_id)
        if session is not None:
            reason = self.policy.session_expired(session, now)
            if reason is not None:
                self._kill_session(session["id"], now, self._reason_text(reason))
                return self._error(reason, self._reason_text(reason))
        return None

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
        # A device that has already registered may ask for a *new* code:
        # the operator may have walked away, the code may have expired,
        # or a second operator may want in.  The device identity is
        # carried over so the room it already owns is not orphaned.
        device_id = None
        session = self.session_of_peer(peer_id)
        if session is not None:
            device_id = session.get("device_id")
        record = self.pairing.issue(now=self._now(), device_id=device_id)
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
        # A device that already owns a room keeps it: re-registering
        # with a fresh code must not strand the room (and the peer
        # that is already in it).
        room_id = None
        session = self.session_of_peer(peer_id)
        if session is not None:
            room_id = session.get("room_id")
        record, credential = self.pairing.register_device(
            code, now=self._now(), room_id=room_id
        )
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

        # ---- the policy ----------------------------------------------------
        #
        # The policy is checked *before* the room is touched, so a
        # refused join leaves no trace: no half-joined peer, no room
        # with a dangling credential.
        now = self._now()

        # A peer that is already in a session is not a *new* session:
        # re-joining with the same credential must not consume a second
        # slot of the concurrency budget.
        already_joined = peer_id in self._peer_session

        if not already_joined:
            refusal = self.policy.check_join(now)
            if refusal is not None:
                logger.info("join refused for %s: %s", peer_id, refusal)
                return [(peer_id, self._error(refusal, self._reason_text(refusal)))]

            # The room budget is checked only when the room is a *new*
            # one — a room that already holds this peer's partner is
            # not a new room.
            if not self._room_has_live_peer(credential.room_id, now):
                refusal = self.policy.check_room_budget(now)
                if refusal is not None:
                    logger.info("join refused for %s: %s", peer_id, refusal)
                    return [(peer_id, self._error(refusal, self._reason_text(refusal)))]

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

        # ---- the session record -------------------------------------------
        #
        # A *session* is a room with at least one peer in it, not a
        # peer: the device and the viewer share one session, and the
        # session is over when the last of them leaves.
        session = self._session_of_room(credential.room_id)
        if session is None:
            session_id = f"sess-{secrets.token_hex(8)}"
            session = {
                "id": session_id,
                "room_id": credential.room_id,
                "device_id": credential.device_id,
                "peers": [],
                "started_at": now,
                "last_activity": now,
            }
            self._sessions[session_id] = session
        session_id = session["id"]
        self._peer_session[peer_id] = session_id
        if peer_id not in session["peers"]:
            session["peers"].append(peer_id)

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
                    "session_id": session_id,
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

    def _room_has_live_peer(self, room_id: str, now: float) -> bool:
        """Does the room already hold a live peer?

        A room that holds a live peer is not a new room, so it does
        not consume a second slot of the room budget.
        """
        for session in self._sessions.values():
            if session.get("room_id") != room_id:
                continue
            if self.policy.session_expired(session, now) is None:
                return True
        return False

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

        # The session record is dropped with its last peer, so the
        # concurrency budget is released as soon as the session ends.
        session_id = self._peer_session.pop(peer_id, None)
        if session_id is not None:
            session = self._sessions.get(session_id)
            if session is not None:
                if peer_id in session["peers"]:
                    session["peers"].remove(peer_id)
                if not session["peers"]:
                    del self._sessions[session_id]

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

        # ---- the policy ----------------------------------------------------
        #
        # A session that has outlived its welcome is torn down before
        # the message is relayed, so the peer gets a clear reason
        # rather than a silent drop.
        refusal = self._enforce_policy(peer_id)
        if refusal is not None:
            return [(peer_id, refusal)]

        other_role = "viewer" if role == "device" else "device"
        other_peer = room.peer_of_role(other_role)
        if other_peer is None:
            # The peer is not there yet.  The message is *not* buffered:
            # a stale offer must never be applied after an ICE restart.
            return [(peer_id, self._error("peer_absent", "the other peer has not joined"))]

        # The media plane is what keeps a session alive: the idle
        # timeout is measured from the last relayed message.
        self._touch(peer_id)

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

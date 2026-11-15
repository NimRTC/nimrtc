#!/usr/bin/env python3
"""Reference signalling service for the NimRTC remote-desktop product.

This is the executable companion of ``docs/guides/remote-desktop.md``.
It is a **reference implementation**, not a production server: it exists
so that a reader can see one complete, working signalling service and
so that the two example clients in ``client.py`` can establish a media
session end to end.

Plan RD-7 keeps the signalling server out of ``src/``: NimRTC ships a
signalling *seam* (``create_offer()`` / ``process_remote_sdp()`` /
``add_turn_server()`` / candidate forwarding), and the room management,
the pairing and the TURN credential issuance are product concerns.

## What it does

  - **Device registration** — a device asks for a short-lived pairing
    code and registers with it.
  - **Pairing** — the operator exchanges the pairing code for a
    session credential.  The *credential* is the identity; the socket
    address is not.
  - **Room join** — both peers present their credential and join the
    room.  The viewer is the offerer, the device answers.
  - **Candidate relay** — SDP, ICE candidates, ICE restarts and
    reconnects are forwarded between the two peers.
  - **TURN credential issuance** — RFC 7635 style
    ``username:timestamp`` + HMAC-SHA1 credentials with a short TTL.

## Dependencies

Python 3 standard library only.  The WebSocket layer is a small
hand-rolled RFC 6455 implementation (``wsproto.py``).

## Usage

    python examples/remote-desktop/signalling.py --host 127.0.0.1 --port 8765

Then, in two more terminals:

    python examples/remote-desktop/client.py --role device --url ws://127.0.0.1:8765
    python examples/remote-desktop/client.py --role viewer --code <the code> \\
        --url ws://127.0.0.1:8765
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import sys
from typing import Dict, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wsproto  # noqa: E402  (path shim above)
from signalling_core import (  # noqa: E402
    DEFAULT_MAX_CONCURRENT_SESSIONS,
    DEFAULT_TURN_TTL_S,
    PairingStore,
    SessionLimits,
    SignallingService,
    TurnCredentialIssuer,
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] signalling: %(message)s",
)
logger = logging.getLogger("signalling")


class SignalingServer:
    """The asyncio WebSocket server around :class:`SignallingService`.

    The server owns one *policy* duty beyond moving bytes: it reaps
    the sessions that have outlived their welcome.  The service can
    decide that a session is expired, but only the server knows when
    to ask — and an expired session nobody reaps is an expired session
    that lives forever.
    """

    def __init__(
        self,
        service: SignallingService,
        reap_interval_s: float = 10.0,
    ) -> None:
        self.service = service
        self._connections: Dict[str, wsproto.WebSocketConnection] = {}
        self._peer_seq = 0
        self._reap_interval_s = reap_interval_s
        self._reaper: Optional[asyncio.Task] = None

    def _next_peer_id(self) -> str:
        self._peer_seq += 1
        return f"peer-{self._peer_seq:04d}"

    # ---- the session reaper ---------------------------------------------

    async def _reap_loop(self) -> None:
        """Drop the sessions that have outlived their welcome.

        The loop is a no-op when neither timeout is armed: the service
        then reports every session as alive, and there is nothing to
        reap.
        """
        while True:
            await asyncio.sleep(self._reap_interval_s)
            try:
                expired = self.service.reap_expired_sessions()
            except Exception as exc:  # noqa: BLE001 - the loop must survive
                logger.warning("the session reaper raised: %r", exc)
                continue
            for target, outbound in expired:
                await self._send(target, outbound)

    def start_reaper(self) -> None:
        """Start the reaper loop."""
        if self._reaper is None:
            self._reaper = asyncio.create_task(self._reap_loop())

    async def stop_reaper(self) -> None:
        """Stop the reaper loop."""
        if self._reaper is not None:
            self._reaper.cancel()
            try:
                await self._reaper
            except asyncio.CancelledError:
                pass
            self._reaper = None

    async def handle_connection(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        """Serve one WebSocket connection for its lifetime."""
        try:
            path = await wsproto.server_handshake(reader, writer)
        except (wsproto.WebSocketError, ConnectionResetError) as exc:
            logger.warning("handshake failed: %s", exc)
            writer.close()
            return

        peer_id = self._next_peer_id()
        conn = wsproto.WebSocketConnection(reader, writer, is_client=False)
        self._connections[peer_id] = conn
        logger.info("peer %s connected (path=%s)", peer_id, path)

        try:
            while True:
                raw = await conn.recv_text()
                try:
                    message = json.loads(raw)
                except json.JSONDecodeError:
                    logger.warning("peer %s sent invalid JSON", peer_id)
                    await self._send(peer_id, {
                        "type": "error",
                        "code": "bad_json",
                        "message": "the payload is not valid JSON",
                    })
                    continue
                if not isinstance(message, dict):
                    await self._send(peer_id, {
                        "type": "error",
                        "code": "bad_request",
                        "message": "the payload must be a JSON object",
                    })
                    continue

                logger.info("peer %s >> %s", peer_id, message.get("type", "?"))
                for target, outbound in self.service.handle(peer_id, message):
                    await self._send(target, outbound)
        except wsproto.ConnectionClosed:
            logger.info("peer %s disconnected", peer_id)
        except (ConnectionResetError, OSError) as exc:
            logger.info("peer %s connection error: %s", peer_id, exc)
        finally:
            self._connections.pop(peer_id, None)
            for target, outbound in self.service.disconnect(peer_id):
                await self._send(target, outbound)
            try:
                writer.close()
            except OSError:
                pass

    async def _send(self, peer_id: str, message: Dict) -> None:
        conn = self._connections.get(peer_id)
        if conn is None:
            return
        try:
            await conn.send_text(json.dumps(message))
        except (wsproto.WebSocketError, OSError) as exc:
            logger.warning("send to peer %s failed: %s", peer_id, exc)


async def main() -> int:
    parser = argparse.ArgumentParser(description="NimRTC remote-desktop reference signalling service")
    parser.add_argument("--host", default="127.0.0.1", help="Bind address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8765, help="Bind port (default: 8765)")
    parser.add_argument(
        "--turn-secret",
        default=os.environ.get("NIMRTC_TURN_SECRET", ""),
        help="Shared secret for the TURN credential HMAC (default: random per run)",
    )
    parser.add_argument(
        "--turn-url",
        default=os.environ.get("NIMRTC_TURN_URL", "turn:127.0.0.1:3478?transport=udp"),
        help="TURN relay URL advertised to the clients",
    )
    parser.add_argument(
        "--turn-ttl",
        type=float,
        default=DEFAULT_TURN_TTL_S,
        help="TURN credential TTL in seconds (default: 3600)",
    )
    parser.add_argument(
        "--max-sessions",
        type=int,
        default=DEFAULT_MAX_CONCURRENT_SESSIONS,
        help="Maximum number of concurrent sessions (0 = no limit)",
    )
    parser.add_argument(
        "--max-rooms",
        type=int,
        default=0,
        help="Maximum number of concurrent rooms (0 = no limit)",
    )
    parser.add_argument(
        "--session-lifetime",
        type=float,
        default=0.0,
        help="Hard session lifetime in seconds (0 = no limit)",
    )
    parser.add_argument(
        "--idle-timeout",
        type=float,
        default=0.0,
        help="Idle disconnect timeout in seconds (0 = no limit)",
    )
    args = parser.parse_args()

    secret = args.turn_secret.encode("utf-8") if args.turn_secret else os.urandom(32)
    service = SignallingService(
        pairing_store=PairingStore(),
        turn_issuer=TurnCredentialIssuer(secret, ttl_s=args.turn_ttl, url=args.turn_url),
        limits=SessionLimits(
            max_concurrent_sessions=args.max_sessions,
            max_concurrent_rooms=args.max_rooms,
            session_lifetime_s=args.session_lifetime,
            idle_timeout_s=args.idle_timeout,
        ),
    )
    server = SignalingServer(service)

    async def on_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        await server.handle_connection(reader, writer)

    listener = await asyncio.start_server(on_client, args.host, args.port)
    logger.info("reference signalling service listening on ws://%s:%d", args.host, args.port)
    logger.info("this is a REFERENCE implementation, not a production server")
    server.start_reaper()
    async with listener:
        await listener.serve_forever()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        logger.info("server stopped")
        sys.exit(0)

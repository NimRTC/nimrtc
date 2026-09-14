#!/usr/bin/env python3
"""
signaling_server.py — Minimal WebSocket signaling server for NimRTC ↔ Chrome interop.

Usage:
    python signaling_server.py [--host 0.0.0.0] [--port 8765]

Each WebSocket client is assigned a room.  The FIRST client to connect
becomes the "offerer"; the SECOND client becomes the "answerer".
Messages from either peer are forwarded verbatim to the other peer in the same room.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import uuid
from typing import Any

try:
    import websockets
except ImportError:
    websockets = None

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("signaling")


class Room:
    """Holds the two peers in a signaling exchange."""

    def __init__(self, room_id: str):
        self.room_id = room_id
        self.peers: dict[str, asyncio.Queue] = {}  # peer_id → send queue
        self.offerer: str | None = None
        self.answerer: str | None = None
        self._lock = asyncio.Lock()
        # Queues for messages addressed to peers that aren't connected yet.
        self.peerless_queues: dict[str, asyncio.Queue] = {}

    async def add_peer(self, peer_id: str, send_queue: asyncio.Queue) -> str:
        """Register a peer and return its role ('offerer' or 'answerer')."""
        async with self._lock:
            if self.offerer is None:
                self.offerer = peer_id
                role = "offerer"
            elif self.answerer is None:
                self.answerer = peer_id
                role = "answerer"
            else:
                raise RuntimeError("Room is full")
            self.peers[peer_id] = send_queue
            logger.info("Room %s: %s joined as %s (%d peers)",
                        self.room_id, peer_id, role, len(self.peers))
            return role

    async def remove_peer(self, peer_id: str) -> None:
        async with self._lock:
            self.peers.pop(peer_id, None)
            if self.offerer == peer_id:
                self.offerer = None
            if self.answerer == peer_id:
                self.answerer = None
            logger.info("Room %s: %s left (%d peers remain)",
                        self.room_id, peer_id, len(self.peers))

    def other_peer(self, peer_id: str) -> str | None:
        """Return the other peer's id, or None if not yet connected."""
        if self.offerer == peer_id:
            return self.answerer
        if self.answerer == peer_id:
            return self.offerer
        return None

    async def broadcast(self, peer_id: str, message: dict) -> None:
        """Forward a message to the other peer in the room.

        If the other peer is not yet connected, queue the message so it can be
        replayed when they join.  This lets the offerer send SDP immediately
        even if the answerer hasn't joined yet.
        """
        other = self.other_peer(peer_id)
        if other and other in self.peers:
            await self.peers[other].put(message)
            logger.debug("Room %s: forwarded %s from %s → %s",
                         self.room_id, message.get("type", "?"),
                         peer_id[:8], other[:8])
        elif other is None and self.offerer is None and self.answerer is None:
            # First peer connected but no second yet; nothing to forward.
            logger.debug("Room %s: %s sent before answerer joined — dropping",
                         self.room_id, peer_id[:8])
        else:
            # Other peer not yet connected — buffer for them.
            if other not in self.peerless_queues:
                self.peerless_queues[other] = asyncio.Queue()
            await self.peerless_queues[other].put(message)
            logger.debug("Room %s: buffered %s from %s for late-joining %s",
                         self.room_id, message.get("type", "?"),
                         peer_id[:8], other[:8])

    async def drain_buffer_for(self, peer_id: str) -> None:
        """Replay queued messages addressed to a newly-joined peer.

        Messages may have been buffered under either:
        - the peer's role name ("offerer" / "answerer"), if the peer hadn't
          been assigned a role when the message arrived, or
        - the peer's actual peer_id (after they joined).
        """
        # Determine this peer's role.
        my_role = "offerer" if self.offerer == peer_id else "answerer"
        sender_role = "answerer" if my_role == "offerer" else "offerer"

        # Drain both the placeholder-keyed queue and the peer_id-keyed queue.
        keys = [peer_id, my_role]
        drained_msgs = []
        for k in keys:
            q = self.peerless_queues.pop(k, None)
            if q is None:
                continue
            while not q.empty():
                try:
                    msg = q.get_nowait()
                except asyncio.QueueEmpty:
                    break
                # Set the "from" field to indicate the sender's role.
                msg_with_from = dict(msg)
                msg_with_from["from"] = sender_role
                drained_msgs.append(msg_with_from)
                logger.debug("Room %s: replayed %s (from %s) to %s",
                             self.room_id, msg.get("type", "?"),
                             sender_role, peer_id[:8])

        # Send drained messages in order to the newly-joined peer.
        for msg in drained_msgs:
            if peer_id in self.peers:
                await self.peers[peer_id].put(msg)


class SignalingServer:
    """Manages rooms and WebSocket connections."""

    def __init__(self):
        self.rooms: dict[str, Room] = {}
        self.connections: dict[str, tuple[asyncio.Queue, Room]] = {}
        self._lock = asyncio.Lock()

    async def get_or_create_room(self, room_id: str) -> Room:
        if room_id not in self.rooms:
            self.rooms[room_id] = Room(room_id)
        return self.rooms[room_id]

    async def register(self, ws: Any,
                       room_id: str) -> tuple[str, Room, str]:
        """
        Register a new WebSocket connection to a room.
        Returns (peer_id, room, role).
        """
        peer_id = str(uuid.uuid4())[:8]
        send_queue: asyncio.Queue[dict] = asyncio.Queue()

        room = await self.get_or_create_room(room_id)
        role = await room.add_peer(peer_id, send_queue)

        async with self._lock:
            self.connections[peer_id] = (send_queue, room)

        logger.info("Peer %s joined room %s as %s", peer_id, room_id, role)
        return peer_id, room, role

    async def unregister(self, peer_id: str) -> None:
        async with self._lock:
            entry = self.connections.pop(peer_id, None)
        if entry:
            send_queue, room = entry
            await room.remove_peer(peer_id)
            # Drain the peer's send queue
            while not send_queue.empty():
                try:
                    send_queue.get_nowait()
                except asyncio.QueueEmpty:
                    break

    async def relay(self, peer_id: str, message: dict) -> None:
        """Relay a message to the other peer (but NOT back to the sender).

        The `from` field is added so recipients know who sent the message.
        If the other peer hasn't joined yet, the message is buffered for them.
        """
        # Look up the room this peer is in (so we can use Room.other_peer).
        async with self._lock:
            entry = self.connections.get(peer_id)
        if not entry:
            logger.warning("relay: peer %s not in connections", peer_id[:8])
            return
        _send_queue, room = entry

        # Add sender identity so the recipient can verify.
        msg_with_from = dict(message)
        msg_with_from["from"] = peer_id

        other = room.other_peer(peer_id)
        if other is None:
            # No other peer in room yet.  Check if this is the very first message
            # ever (room empty) or if sender doesn't have a role (impossible).
            if room.offerer is None and room.answerer is None:
                logger.debug("Room %s: %s sent with no other peer; dropping",
                             room.room_id, peer_id[:8])
                return
            # The other slot will be filled soon — buffer to "the other peer".
            target = room.answerer if peer_id == room.offerer else room.offerer
            if target is None:
                # Edge case: only one peer, the role slot for the OTHER one is None.
                # Buffer to a placeholder key based on the sender's role.
                target = "answerer" if peer_id == room.offerer else "offerer"
            if target not in room.peerless_queues:
                room.peerless_queues[target] = asyncio.Queue()
            await room.peerless_queues[target].put(msg_with_from)
            logger.info("Room %s: buffered %s from %s for late-joining %s",
                       room.room_id, message.get("type", "?"),
                       peer_id[:8], str(target)[:8])
            return

        if other in room.peers:
            await room.peers[other].put(msg_with_from)
            logger.info("Room %s: relayed %s from %s → %s",
                       room.room_id, message.get("type", "?"),
                       peer_id[:8], other[:8])
        else:
            # Other peer not yet connected — buffer for them.
            if other not in room.peerless_queues:
                room.peerless_queues[other] = asyncio.Queue()
            await room.peerless_queues[other].put(msg_with_from)
            logger.debug("Room %s: buffered %s from %s for %s",
                         room.room_id, message.get("type", "?"),
                         peer_id[:8], other[:8])


async def handle_client(ws: Any,
                        server: SignalingServer,
                        room_id: str) -> None:
    """Handle one WebSocket client for its lifetime."""
    peer_id = None
    try:
        # ``websockets`` performs the HTTP upgrade before invoking the
        # handler, so this server receives an already-open WebSocket.
        peer_id, room, role = await server.register(ws, room_id)
        logger.info("Peer %s (%s) connected in room %s", peer_id, role, room_id)
        # Replay any messages that were buffered while this peer was offline.
        await room.drain_buffer_for(peer_id)
        logger.info("Peer %s (%s) connected in room %s", peer_id, role, room_id)

        async def send_task():
            # Pull from peer's send queue and forward over WebSocket.
            while True:
                entry = None
                async with server._lock:
                    entry = server.connections.get(peer_id)
                if entry is None:
                    break
                send_queue, _ = entry
                try:
                    msg = await asyncio.wait_for(send_queue.get(), timeout=5.0)
                except asyncio.TimeoutError:
                    continue
                except asyncio.CancelledError:
                    break
                try:
                    await ws.send(json.dumps(msg))
                except websockets.exceptions.ConnectionClosed:
                    break

        async def recv_task():
            async for raw in ws:
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    logger.warning("Peer %s sent invalid JSON: %r", peer_id, raw)
                    continue

                msg_type = msg.get("type", "?")
                logger.info("Room %s [%s]: %s → relay", room_id, peer_id, msg_type)
                await server.relay(peer_id, msg)

        send_task_handle = asyncio.create_task(send_task())
        recv_task_handle = asyncio.create_task(recv_task())

        try:
            await recv_task_handle
        finally:
            send_task_handle.cancel()
            try:
                await send_task_handle
            except asyncio.CancelledError:
                pass

    except websockets.exceptions.WebSocketException as e:
        logger.warning("WebSocket error for peer %s: %s", peer_id, e)
    finally:
        if peer_id:
            await server.unregister(peer_id)
            logger.info("Peer %s disconnected from room %s", peer_id, room_id)


async def main() -> None:
    if websockets is None:
        raise RuntimeError(
            "websockets package not installed — run: pip install websockets")
    parser = argparse.ArgumentParser(description="NimRTC ↔ Chrome signaling server")
    parser.add_argument("--host", default="0.0.0.0",
                        help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8765,
                        help="WebSocket port (default: 8765)")
    parser.add_argument("--room", default="interop",
                        help="Default room name (default: interop)")
    args = parser.parse_args()

    server = SignalingServer()

    async def handler(ws: Any, *handler_args: Any) -> None:
        # websockets < 15 passes ``path`` as the second positional argument.
        # websockets 13.x (legacy WebSocketServerProtocol) exposes it at
        # ``ws.path``; newer releases would use ``ws.request.path``.
        if handler_args:
            path = handler_args[0]
        else:
            # Legacy path: ws.request does NOT exist on WebSocketServerProtocol
            # in websockets 13.x — use ws.path instead.
            path = getattr(ws, "path", "") or ""
        room_id = str(path).lstrip("/").split("?", 1)[0] or args.room
        logger.info("WS HANDSHAKE: path=%r room_id=%r", path, room_id)
        await handle_client(ws, server, room_id)

    logger.info("Starting signaling server on ws://%s:%d", args.host, args.port)
    async with websockets.serve(handler, args.host, args.port) as srv:
        logger.info("Signaling server ready — ws://%s:%d", args.host, args.port)
        logger.info("Connect Chrome at: ws://%s:%d/%s", args.host, args.port, args.room)
        logger.info("Connect NimRTC demo-p2p at: ws://%s:%d/%s", args.host, args.port, args.room)
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Server stopped")

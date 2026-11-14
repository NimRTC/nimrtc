"""Minimal RFC 6455 WebSocket helpers — standard library only.

The reference signalling service must run on a bare Python 3 install
(no third-party packages), so this module implements just enough of
RFC 6455 to carry JSON text messages:

  - the HTTP/1.1 upgrade handshake (server and client roles),
  - text frames with the mandatory client-side masking,
  - fragmentation reassembly,
  - the control frames a peer actually needs: ping / pong / close.

It is *not* a general-purpose WebSocket library: there is no extension
negotiation, no permessage-deflate, and no binary-frame API.  The
signalling protocol in ``signalling.py`` speaks JSON text frames only.

The engine's own C++ side does not speak WebSocket at all — the
signalling service is a product concern (plan RD-7), and the engine
only needs the SDP / candidate surface described in
``docs/guides/remote-desktop.md``.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import os
import struct
from typing import Optional, Tuple

__all__ = [
    "WebSocketError",
    "ConnectionClosed",
    "WebSocketConnection",
    "server_handshake",
    "client_connect",
]

# The GUID from RFC 6455 §1.3.
_WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#: Maximum accepted frame payload.  The signalling messages are small
#: (an SDP blob is a few kilobytes); anything larger is a bug or an
#: attack, and refusing it keeps the service from buffering megabytes.
_MAX_PAYLOAD = 1 << 20  # 1 MiB


class WebSocketError(Exception):
    """Protocol-level failure (bad handshake, oversized frame, ...)."""


class ConnectionClosed(WebSocketError):
    """The peer closed the connection."""


def _accept_key(client_key: str) -> str:
    """Compute the ``Sec-WebSocket-Accept`` value for a client key."""
    digest = hashlib.sha1((client_key + _WS_GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


async def _read_headers(reader: asyncio.StreamReader) -> Tuple[str, dict]:
    """Read an HTTP request line plus headers.  Returns (request_line, headers)."""
    request_line = await reader.readline()
    if not request_line:
        raise WebSocketError("connection closed during handshake")
    headers = {}
    while True:
        line = await reader.readline()
        if not line or line in (b"\r\n", b"\n"):
            break
        name, _, value = line.decode("latin-1").partition(":")
        headers[name.strip().lower()] = value.strip()
    return request_line.decode("latin-1").strip(), headers


async def server_handshake(
    reader: asyncio.StreamReader, writer: asyncio.StreamWriter
) -> str:
    """Perform the server side of the WebSocket handshake.

    Returns the request path (e.g. ``"/rd/abc123"``) so the caller can
    route on it.
    """
    request_line, headers = await _read_headers(reader)
    parts = request_line.split()
    if len(parts) < 2 or parts[0].upper() != "GET":
        raise WebSocketError("malformed WebSocket request line")
    key = headers.get("sec-websocket-key")
    if not key:
        raise WebSocketError("missing Sec-WebSocket-Key header")
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {_accept_key(key)}\r\n"
        "\r\n"
    )
    writer.write(response.encode("ascii"))
    await writer.drain()
    return parts[1]


async def client_connect(
    host: str, port: int, path: str = "/", timeout: float = 10.0
) -> "WebSocketConnection":
    """Open a client WebSocket connection to ``ws://host:port/path``."""
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(host, port), timeout
    )
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    writer.write(request.encode("ascii"))
    await writer.drain()

    status_line = await reader.readline()
    headers = {}
    while True:
        line = await reader.readline()
        if not line or line in (b"\r\n", b"\n"):
            break
        name, _, value = line.decode("latin-1").partition(":")
        headers[name.strip().lower()] = value.strip()
    if b"101" not in status_line:
        writer.close()
        raise WebSocketError(f"handshake rejected: {status_line!r}")
    if headers.get("sec-websocket-accept") != _accept_key(key):
        writer.close()
        raise WebSocketError("handshake failed: bad Sec-WebSocket-Accept")
    return WebSocketConnection(reader, writer, is_client=True)


class WebSocketConnection:
    """One WebSocket connection.

    Only text frames are exposed.  ``recv_text`` returns the next
    complete text message, transparently reassembling continuation
    frames and answering pings.
    """

    def __init__(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
        is_client: bool,
    ) -> None:
        self._reader = reader
        self._writer = writer
        self._is_client = is_client
        self._send_lock = asyncio.Lock()
        self._closed = False

    # ---- sending ---------------------------------------------------------

    async def _send_frame(self, opcode: int, payload: bytes) -> None:
        if self._closed:
            raise ConnectionClosed("connection already closed")
        header = bytearray()
        header.append(0x80 | opcode)  # FIN + opcode
        length = len(payload)
        mask_bit = 0x80 if self._is_client else 0x00
        if length < 126:
            header.append(mask_bit | length)
        elif length < (1 << 16):
            header.append(mask_bit | 126)
            header.extend(struct.pack(">H", length))
        else:
            header.append(mask_bit | 127)
            header.extend(struct.pack(">Q", length))
        if self._is_client:
            # RFC 6455 §5.3: every client frame MUST be masked.
            mask = os.urandom(4)
            header.extend(mask)
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        async with self._send_lock:
            self._writer.write(bytes(header) + payload)
            await self._writer.drain()

    async def send_text(self, text: str) -> None:
        """Send one text message."""
        await self._send_frame(0x1, text.encode("utf-8"))

    async def ping(self, payload: bytes = b"") -> None:
        """Send a ping control frame."""
        await self._send_frame(0x9, payload)

    async def close(self, code: int = 1000) -> None:
        """Send a close frame and tear the connection down."""
        if self._closed:
            return
        try:
            await self._send_frame(0x8, struct.pack(">H", code))
        except (WebSocketError, OSError):
            pass
        self._closed = True
        try:
            self._writer.close()
        except OSError:
            pass

    # ---- receiving -------------------------------------------------------

    async def _read_frame(self) -> Tuple[bool, int, bytes]:
        header = await self._reader.readexactly(2)
        fin = bool(header[0] & 0x80)
        opcode = header[0] & 0x0F
        masked = bool(header[1] & 0x80)
        length = header[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", await self._reader.readexactly(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", await self._reader.readexactly(8))[0]
        if length > _MAX_PAYLOAD:
            raise WebSocketError(f"frame payload too large: {length}")
        mask = await self._reader.readexactly(4) if masked else b""
        payload = await self._reader.readexactly(length) if length else b""
        if masked and payload:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return fin, opcode, payload

    async def recv_text(self) -> str:
        """Return the next complete text message."""
        chunks = []
        message_opcode: Optional[int] = None
        while True:
            try:
                fin, opcode, payload = await self._read_frame()
            except (asyncio.IncompleteReadError, ConnectionResetError) as exc:
                self._closed = True
                raise ConnectionClosed(str(exc)) from exc

            if opcode == 0x8:  # close
                self._closed = True
                try:
                    self._writer.close()
                except OSError:
                    pass
                raise ConnectionClosed("peer closed the connection")
            if opcode == 0x9:  # ping
                await self._send_frame(0xA, payload)  # pong
                continue
            if opcode == 0xA:  # pong
                continue

            if opcode in (0x1, 0x2):
                message_opcode = opcode
                chunks = [payload]
            elif opcode == 0x0:  # continuation
                if message_opcode is None:
                    raise WebSocketError("continuation frame without a start frame")
                chunks.append(payload)
            else:
                raise WebSocketError(f"unsupported opcode {opcode}")

            if fin:
                data = b"".join(chunks)
                if message_opcode == 0x2:
                    raise WebSocketError("binary frames are not supported")
                return data.decode("utf-8")

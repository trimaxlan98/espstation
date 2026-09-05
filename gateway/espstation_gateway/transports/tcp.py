# TCP transport for WiFi-connected nodes. PROTOCOL.md section 2.2: u16
# big-endian length prefix then the frame body. Unlike serial there is no
# resync problem to solve -- TCP is already an ordered, reliable byte
# stream -- so a malformed length prefix or bad frame is a protocol
# violation worth tearing down the connection over, not console noise.
from __future__ import annotations

import asyncio
import struct
from typing import AsyncIterator

from .base import Transport
from ..protocol.frames import Frame, FrameError, parse

_LEN_STRUCT = struct.Struct(">H")  # big-endian per PROTOCOL.md 2.2


class LengthPrefixDecoder:
    """FrameDecoder for u16-BE length-prefixed framing (TCP and WebSocket
    binary frames both use this per espstation.protocol.yaml `framing:`)."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> list[tuple[str, object]]:
        events: list[tuple[str, object]] = []
        self._buf.extend(chunk)
        while True:
            if len(self._buf) < _LEN_STRUCT.size:
                break
            (length,) = _LEN_STRUCT.unpack_from(self._buf, 0)
            total = _LEN_STRUCT.size + length
            if len(self._buf) < total:
                break
            body = bytes(self._buf[_LEN_STRUCT.size:total])
            del self._buf[:total]
            try:
                events.append(("frame", parse(body)))
            except FrameError:
                events.append(("raw", body))
        return events

    def flush(self) -> list[tuple[str, object]]:
        if not self._buf:
            return []
        tail = bytes(self._buf)
        self._buf.clear()
        return [("raw", tail)]


def encode_length_prefixed(frame_body: bytes) -> bytes:
    if len(frame_body) > 0xFFFF:
        raise ValueError(f"frame body of {len(frame_body)} bytes too large for u16 length prefix")
    return _LEN_STRUCT.pack(len(frame_body)) + frame_body


class TcpTransport(Transport):
    def __init__(self, host: str, port: int, *, connect_timeout: float = 5.0, read_chunk: int = 4096) -> None:
        self.host = host
        self.port = port
        self._connect_timeout = connect_timeout
        self._read_chunk = read_chunk
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None

    async def open(self) -> None:
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), timeout=self._connect_timeout
        )

    async def close(self) -> None:
        writer, self._writer = self._writer, None
        self._reader = None
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass

    async def send(self, frame_bytes: bytes) -> None:
        if self._writer is None:
            raise RuntimeError("TcpTransport is not open")
        self._writer.write(encode_length_prefixed(frame_bytes))
        await self._writer.drain()

    async def receive(self) -> AsyncIterator[bytes]:
        if self._reader is None:
            raise RuntimeError("TcpTransport is not open")
        while True:
            chunk = await self._reader.read(self._read_chunk)
            if not chunk:
                return
            yield chunk

# Transport interface + Link glue.
#
# Design: Transport.send() takes a *raw frame body* (frames.encode() output)
# and is itself responsible for wire-framing it (COBS+delimiter for serial,
# length-prefix for TCP, verbatim for ESP-NOW-like raw transports) -- each
# transport owns the framing rule for its own medium. Inbound is the mirror
# image but asymmetric on purpose: receive() yields raw *wire* bytes exactly
# as they arrived (no per-transport unframing), and a separate FrameDecoder
# turns that byte stream into frames. This asymmetry exists because
# unframing on serial is the hard, stateful part (COBS resync across
# arbitrary chunk boundaries, interleaved boot-ROM garbage -- PROTOCOL.md
# section 2.1) and deserves to live in one well-tested class
# (protocol.frames.StreamingDecoder) that Link wires in, rather than being
# duplicated inside every transport.
from __future__ import annotations

import abc
import asyncio
from dataclasses import dataclass, field
from typing import Any, AsyncIterator, Awaitable, Callable, Protocol

from ..protocol.frames import Frame, FrameError, parse
from ..protocol.ndb import NodeRegistry


class TransportError(RuntimeError):
    pass


class Transport(abc.ABC):
    """Async transport interface. All I/O is async; a transport whose
    underlying library is blocking (pyserial) must push blocking calls into
    an executor itself (see transports/serial_port.py) -- callers never
    block on this interface."""

    @abc.abstractmethod
    async def open(self) -> None:
        ...

    @abc.abstractmethod
    async def close(self) -> None:
        ...

    @abc.abstractmethod
    async def send(self, frame_bytes: bytes) -> None:
        """Send one raw ENLP frame body (already CRC'd, not yet wire-framed
        for this transport's medium)."""
        ...

    @abc.abstractmethod
    def receive(self) -> AsyncIterator[bytes]:
        """Async iterator of raw inbound byte chunks in arrival order.
        Ends when the transport is closed (StopAsyncIteration) or raises if
        the underlying medium fails."""
        ...


class FrameDecoder(Protocol):
    """Turns a raw inbound byte stream into ("frame", Frame) / ("raw",
    bytes) events. protocol.frames.StreamingDecoder (COBS) and
    transports.tcp.LengthPrefixDecoder both satisfy this."""

    def feed(self, chunk: bytes) -> list[tuple[str, object]]: ...

    def flush(self) -> list[tuple[str, object]]: ...


class RawFrameDecoder:
    """FrameDecoder for media where each inbound chunk is already exactly
    one complete frame body with no delimiter and no length prefix -- true
    for ESP-NOW (PROTOCOL.md section 2.3, "the frame body is placed directly
    in the ESP-NOW payload") and for the in-process sim transport that
    stands in for it. There is no partial-chunk buffering to do."""

    def feed(self, chunk: bytes) -> list[tuple[str, object]]:
        if not chunk:
            return []
        try:
            return [("frame", parse(chunk))]
        except FrameError:
            return [("raw", chunk)]

    def flush(self) -> list[tuple[str, object]]:
        return []


@dataclass
class LinkEvent:
    kind: str  # "frame" | "raw" | "error"
    payload: Any


ListenerFn = Callable[[LinkEvent], "Awaitable[None] | None"]


class Link:
    """Pairs one Transport with a FrameDecoder and a NodeRegistry, and pumps
    inbound bytes -> decoded events -> subscribers. One Link per attached
    serial port / TCP node / sim node; this is the unit the REST/WS layer
    and the store subscribe to."""

    def __init__(
        self,
        link_id: str,
        kind: str,
        transport: Transport,
        decoder: FrameDecoder,
        registry: NodeRegistry | None = None,
        *,
        meta: dict[str, Any] | None = None,
    ) -> None:
        self.id = link_id
        self.kind = kind
        self.transport = transport
        self.decoder = decoder
        self.registry = registry or NodeRegistry()
        self.meta = meta or {}
        self.connected = False
        self._task: asyncio.Task | None = None
        self._listeners: list[ListenerFn] = []
        self._seq = 0

    def subscribe(self, callback: ListenerFn) -> None:
        self._listeners.append(callback)

    def unsubscribe(self, callback: ListenerFn) -> None:
        if callback in self._listeners:
            self._listeners.remove(callback)

    def next_seq(self) -> int:
        """Per-sender seq counter for frames the *station* originates on
        this link (CMD, EXP_SET, TELEM_ACK, ...). PROTOCOL.md section 3:
        seq is per-sender and wraps at 65535."""
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFFFF
        return seq

    async def start(self) -> None:
        await self.transport.open()
        self.connected = True
        self._task = asyncio.create_task(self._pump(), name=f"link-pump-{self.id}")

    async def stop(self) -> None:
        self.connected = False
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
            self._task = None
        await self.transport.close()
        for kind, payload in self.decoder.flush():
            await self._emit(LinkEvent(kind, payload))

    async def send_frame(self, frame_bytes: bytes) -> None:
        await self.transport.send(frame_bytes)

    async def _pump(self) -> None:
        try:
            async for chunk in self.transport.receive():
                for kind, payload in self.decoder.feed(chunk):
                    await self._emit(LinkEvent(kind, payload))
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # transport died mid-stream
            self.connected = False
            await self._emit(LinkEvent("error", exc))

    async def _emit(self, event: LinkEvent) -> None:
        for cb in list(self._listeners):
            result = cb(event)
            if asyncio.iscoroutine(result):
                await result

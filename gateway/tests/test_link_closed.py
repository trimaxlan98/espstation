# A transport whose medium ENDS is not a transport that failed.
#
# Regression: Link._pump fell off the end of its `async for` without touching
# `connected`, so a replayed capture that ran out — or any peer that closed
# cleanly — left the app showing a node online that could never produce
# another byte. The counters it had already sent kept being presented as
# current. This is the same class of lie D-22 rejects for channels a node
# cannot sample.
from __future__ import annotations

import asyncio
from typing import AsyncIterator

import pytest

from espstation_gateway.transports.base import Link, LinkEvent, RawFrameDecoder, Transport


class _FiniteTransport(Transport):
    """Yields a fixed number of chunks and then ends, like a finished replay."""

    def __init__(self, chunks: list[bytes]) -> None:
        self._chunks = chunks
        self.closed = False

    async def open(self) -> None: ...

    async def close(self) -> None:
        self.closed = True

    async def send(self, frame_bytes: bytes) -> None: ...

    async def receive(self) -> AsyncIterator[bytes]:
        for c in self._chunks:
            yield c
            await asyncio.sleep(0)


class _BlockingTransport(Transport):
    """Never ends on its own; only stop() takes it down."""

    async def open(self) -> None: ...
    async def close(self) -> None: ...
    async def send(self, frame_bytes: bytes) -> None: ...

    async def receive(self) -> AsyncIterator[bytes]:
        while True:
            await asyncio.sleep(0.01)
            yield b""


async def _events_of(link: Link) -> list[LinkEvent]:
    seen: list[LinkEvent] = []
    link.subscribe(lambda ev: seen.append(ev))
    return seen


@pytest.mark.asyncio
async def test_an_exhausted_medium_marks_the_link_closed():
    link = Link("t-1", "test", _FiniteTransport([b"", b""]), RawFrameDecoder())
    seen = await _events_of(link)
    await link.start()
    assert link.connected is True

    for _ in range(50):          # let the pump drain and finish
        await asyncio.sleep(0.01)
        if not link.connected:
            break

    assert link.connected is False, "an exhausted medium must not stay connected"
    kinds = [e.kind for e in seen]
    assert "closed" in kinds
    assert "error" not in kinds, "running out is an end, not a failure"
    await link.stop()


@pytest.mark.asyncio
async def test_a_deliberate_stop_is_not_reported_as_a_closed_medium():
    """stop() cancels the pump, and CancelledError must not be mistaken for
    the iterator ending: the distinction is what keeps a detach quiet."""
    link = Link("t-2", "test", _BlockingTransport(), RawFrameDecoder())
    seen = await _events_of(link)
    await link.start()
    await asyncio.sleep(0.05)
    await link.stop()

    assert link.connected is False
    assert "closed" not in [e.kind for e in seen]


@pytest.mark.asyncio
async def test_a_transport_that_raises_is_still_an_error():
    class _Broken(Transport):
        async def open(self) -> None: ...
        async def close(self) -> None: ...
        async def send(self, frame_bytes: bytes) -> None: ...

        async def receive(self) -> AsyncIterator[bytes]:
            yield b""
            raise OSError("the port went away")

    link = Link("t-3", "test", _Broken(), RawFrameDecoder())
    seen = await _events_of(link)
    await link.start()
    for _ in range(50):
        await asyncio.sleep(0.01)
        if not link.connected:
            break

    assert link.connected is False
    kinds = [e.kind for e in seen]
    assert "error" in kinds and "closed" not in kinds
    await link.stop()

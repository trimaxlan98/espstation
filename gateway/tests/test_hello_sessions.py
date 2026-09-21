# A NDB too big for one frame is announced as several HELLO chunks. They are one
# announcement: one session, one HELLO_ACK per chunk.
from __future__ import annotations

from espstation_gateway.protocol import frames
from espstation_gateway.protocol import messages as msg
from espstation_gateway.runtime import HELLO_ANNOUNCE_GAP_S, GatewayRuntime
from espstation_gateway.store import Store
from espstation_gateway.transports.sim import SimNode


class FakeLink:
    def __init__(self, link_id: str) -> None:
        self.id = link_id
        self.sent: list[bytes] = []
        self._seq = 0

    def next_seq(self) -> int:
        self._seq += 1
        return self._seq

    async def send_frame(self, frame_bytes: bytes) -> None:
        self.sent.append(frame_bytes)

    def acks(self) -> list[msg.HelloAck]:
        out = []
        for raw in self.sent:
            frame = frames.parse(raw)
            if frame.type == msg.TYPE_HELLO_ACK:
                out.append(msg.HelloAck.from_payload(frame.payload))
        return out


class Clock:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


def _chunks(node_id: int = 1001) -> list[msg.Hello]:
    node = SimNode(node_id, dio=True)
    hellos = [msg.Hello.from_payload(p) for p in node._hello_payloads()]
    assert len(hellos) >= 2
    return hellos


def _runtime(tmp_path) -> tuple[GatewayRuntime, Clock, Store]:
    store = Store(tmp_path / "hello.db")
    runtime = GatewayRuntime(store)
    clock = Clock()
    runtime._now = clock
    return runtime, clock, store


async def test_chunks_of_one_announcement_share_a_session_and_each_gets_an_ack(tmp_path) -> None:
    runtime, clock, store = _runtime(tmp_path)
    link = FakeLink("sim-1")
    chunks = _chunks()
    for hello in chunks:
        await runtime._on_hello(link, hello)
        clock.t += 0.05  # milliseconds apart in real life
    acks = link.acks()
    assert len(acks) == len(chunks)  # the node counts ACKs: one per chunk, never fewer
    assert len({a.session for a in acks}) == 1 and all(a.accepted for a in acks)
    # the merged NDB is complete, in memory and in SQLite
    keys = {c.key for h in chunks for c in h.ndb}
    assert {c.key for c in runtime.registry.get(1001).channels_by_id.values()} == keys
    import json
    assert {c["key"] for c in json.loads(store.get_node(1001)["ndb_json"])} == keys
    store.close()


async def test_a_hello_after_the_window_opens_a_new_session(tmp_path) -> None:
    runtime, clock, store = _runtime(tmp_path)
    link = FakeLink("sim-1")
    first, second = _chunks()[:2]
    await runtime._on_hello(link, first)
    clock.t += HELLO_ANNOUNCE_GAP_S - 0.1  # still inside the window: same announcement
    await runtime._on_hello(link, second)
    clock.t += HELLO_ANNOUNCE_GAP_S  # exactly the gap: the node came back
    await runtime._on_hello(link, first)
    a, b, c = link.acks()
    assert a.session == b.session
    assert c.session != a.session
    store.close()


async def test_the_window_slides_with_each_chunk(tmp_path) -> None:
    # "Less than 5 s between consecutive chunks", not "5 s since the first".
    runtime, clock, store = _runtime(tmp_path)
    link = FakeLink("sim-1")
    hello = _chunks()[0]
    for _ in range(4):
        await runtime._on_hello(link, hello)
        clock.t += HELLO_ANNOUNCE_GAP_S - 1
    assert len({a.session for a in link.acks()}) == 1
    store.close()


async def test_another_link_or_another_node_opens_a_new_session(tmp_path) -> None:
    runtime, clock, store = _runtime(tmp_path)
    first, second = _chunks()[:2]
    link_a, link_b = FakeLink("sim-1"), FakeLink("serial-2")
    await runtime._on_hello(link_a, first)
    await runtime._on_hello(link_b, second)  # same node, other link, immediately after
    assert link_a.acks()[0].session != link_b.acks()[0].session

    await runtime._on_hello(link_b, _chunks(node_id=1002)[0])  # different node: its own session
    other = link_b.acks()[1].session
    assert "1002" in other and other != link_b.acks()[0].session
    store.close()


async def test_sessions_opened_in_the_same_millisecond_still_differ(tmp_path, monkeypatch) -> None:
    runtime, clock, store = _runtime(tmp_path)
    monkeypatch.setattr("espstation_gateway.runtime.time.time", lambda: 1_700_000_000.0)
    link = FakeLink("sim-1")
    hello = _chunks()[0]
    await runtime._on_hello(link, hello)
    clock.t += 100
    await runtime._on_hello(link, hello)
    a, b = link.acks()
    assert a.session != b.session
    store.close()


async def test_every_chunk_records_a_time_sync_anchor(tmp_path) -> None:
    # Documented choice: each chunk is a valid measurement, so none is skipped.
    runtime, clock, store = _runtime(tmp_path)
    calls = []
    real = store.record_time_sync
    store.record_time_sync = lambda *a, **k: (calls.append(a), real(*a, **k))[1]  # type: ignore[method-assign]
    link = FakeLink("sim-1")
    chunks = _chunks()
    for hello in chunks:
        await runtime._on_hello(link, hello)
    assert len(calls) == len(chunks)
    store.close()

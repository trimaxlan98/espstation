# Attaching a recorded bench capture as a node, through the real REST layer.
#
# This is the zero-hardware demo of the Morse practice. It matters that it
# goes through app.py and the runtime rather than calling the decoder
# directly: what is being tested is that the app cannot tell a replay from a
# board, because both use the same adapter, the same codec and the same
# registry.
from __future__ import annotations

import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from espstation_gateway.app import DEFAULT_TOKEN, Settings, create_app

EVIDENCIA = (
    Path(__file__).resolve().parents[2]
    / "bench" / "practicas" / "morse-duplex" / "evidencia"
)
LOG = EVIDENCIA / "tanda_k40_l600_A.log"
AUTH = {"Authorization": f"Bearer {DEFAULT_TOKEN}"}


@pytest.fixture()
def client():
    app = create_app(Settings(token=DEFAULT_TOKEN))
    with TestClient(app) as c:
        yield c


@pytest.mark.skipif(not LOG.is_file(), reason="no bench evidence in the tree")
def test_a_recorded_capture_attaches_as_a_node_with_morse_channels(client):
    r = client.post("/api/links", headers=AUTH, json={
        "kind": "morse-replay", "path": str(LOG), "label": "replay", "speed": 200.0,
    })
    assert r.status_code == 200, r.text
    link = r.json()
    assert link["kind"] == "morse" and link["replay"] is True
    assert link["commands"] is False, "a recording must never look commandable"

    node_id = link["node_id"]
    # The adapter announces on its first bytes, and the replay's first line
    # is due immediately, but "immediately" is still a hop through the event
    # loop -- so poll rather than sleep a guessed amount.
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        nodes = client.get("/api/nodes", headers=AUTH).json()
        if any(n["node_id"] == node_id for n in nodes):
            break
        time.sleep(0.1)
    nodes = client.get("/api/nodes", headers=AUTH).json()
    node = next((n for n in nodes if n["node_id"] == node_id), None)
    assert node is not None, "the replayed board never announced itself"
    assert node["fw"] == "bench-morse-duplex"

    detail = client.get(f"/api/nodes/{node_id}", headers=AUTH).json()
    keys = {ch["key"] for ch in detail["ndb"]}
    assert {"morse.tx", "morse.rx", "morse.pulse_ms", "morse.letters"} <= keys

    r = client.delete(f"/api/links/{link['id']}", headers=AUTH)
    assert r.status_code == 200


def test_a_missing_capture_is_a_clean_error(client, tmp_path):
    r = client.post("/api/links", headers=AUTH, json={
        "kind": "morse-replay", "path": str(tmp_path / "nope.log"),
    })
    assert r.status_code >= 400


def test_path_is_required(client):
    r = client.post("/api/links", headers=AUTH, json={"kind": "morse-replay"})
    assert r.status_code == 400


# -- the poll task ---------------------------------------------------------
class _FakeLink:
    def __init__(self):
        self.id, self.connected = "morse-1", True
        self.meta = {}


class _FakeTransport:
    def __init__(self):
        self.written: list[str] = []

    async def write_text(self, text):
        self.written.append(text)


class _FakeDecoder:
    verbose = None


async def test_morse_poll_primes_verbose_even_when_the_board_answers_late(monkeypatch):
    """Regression: the adapter checked `decoder.verbose == 0` exactly once,
    0.8 s after attach. A board that had not finished answering `r` by then
    left verbose None, the `v` was never sent, and morse.pulse_ms /
    morse.gap_ms stayed empty for the whole session -- the precise failure
    D-22 says `v` exists to prevent."""
    import asyncio as _asyncio

    from espstation_gateway.runtime import GatewayRuntime
    from espstation_gateway.store import Store

    real_sleep = _asyncio.sleep
    monkeypatch.setattr(_asyncio, "sleep", lambda d: real_sleep(0))

    rt = GatewayRuntime(Store(":memory:"))
    link, transport, decoder = _FakeLink(), _FakeTransport(), _FakeDecoder()

    async def answer_late():
        while len(transport.written) < 3:     # the board is slow: two `r` go
            await real_sleep(0)               # out before it says anything
        decoder.verbose = 0

    async def stop_soon():
        while transport.written.count("v" + chr(10)) == 0:
            await real_sleep(0)
        await real_sleep(0)
        link.connected = False

    await _asyncio.wait_for(_asyncio.gather(
        rt._morse_poll(link, transport, decoder, verbose=True, period_s=0.0),
        answer_late(), stop_soon()), timeout=10)

    assert transport.written.count("v" + chr(10)) == 1, "exactly one `v` per attach"
    assert transport.written.count("r" + chr(10)) >= 2
    assert transport.written[0] == "r" + chr(10), "`r` first: never guess, ask"


async def test_two_morse_sources_never_share_a_node_id():
    """The id is only 8 bits of the path, so a board and a capture can collide.
    Two links on one node id merge downstream and their TELEM_ACKs cross."""
    from espstation_gateway.runtime import GatewayRuntime
    from espstation_gateway.store import Store

    rt = GatewayRuntime(Store(":memory:"))
    a = rt._morse_node_id("/tmp/one.log")
    rt.links["morse-1"] = type("L", (), {"meta": {"node_id": a}})()
    b = rt._morse_node_id("/tmp/one.log")      # same source, now taken
    assert a != b and (b >> 8) == 0x4D

# POST /api/sim/fault with kind=wire takes arbitrary JSON: every bad input has
# to come back as a 400 and leave the cable exactly as it was, never a 500 and
# never a half-installed or silently dead wire.
from __future__ import annotations

import httpx
import pytest

from espstation_gateway.app import DEFAULT_TOKEN, Settings, create_app
from espstation_gateway.store import Store

AUTH = {"Authorization": f"Bearer {DEFAULT_TOKEN}", "Content-Type": "application/json"}


def _body(**fields: str) -> bytes:
    """Raw JSON text, because httpx refuses to serialise NaN/inf and a client
    can send them (1e400 parses to inf)."""
    parts = ['"kind":"wire"', '"tx":1001', '"rx":1002']
    parts += [f'"{k}":{v}' for k, v in fields.items()]
    return ("{" + ",".join(parts) + "}").encode()


BAD_BODIES = [
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"loss":null}', id="loss-null"),
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"loss":[1]}', id="loss-list"),
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"loss":{}}', id="loss-dict"),
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"loss":true}', id="loss-bool"),
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"delay_us":"abc"}', id="delay-string"),
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"delay_us":"40"}', id="delay-numeric-string"),
    pytest.param(b'{"kind":"wire","tx":null,"rx":1002,"loss":0.1}', id="tx-null"),
    pytest.param(b'{"kind":"wire","tx":"x","rx":1002,"loss":0.1}', id="tx-string"),
    pytest.param(b'{"kind":"wire","tx":true,"rx":1002,"loss":0.1}', id="tx-bool"),
    pytest.param(b'{"kind":"wire","tx":1001.5,"rx":1002,"loss":0.1}', id="tx-fraction"),
    pytest.param(b'{"kind":"wire","tx":[1001],"rx":1002,"loss":0.1}', id="tx-list"),
    pytest.param(b'{"kind":"wire","rx":1002,"loss":0.1}', id="tx-missing"),
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"voltage":3.3}', id="unknown-field"),
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"loss":2}', id="loss-out-of-range"),
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"delay_us":-1}', id="delay-negative"),
    pytest.param(b'{"kind":"wire","tx":1001,"rx":1002,"loss":1' + b"0" * 400 + b'}', id="loss-huge-int"),
    pytest.param(b'{"kind":"wire","tx":1099,"rx":1002,"loss":0.1}', id="no-such-node"),
    pytest.param(b'{"kind":"wire","tx":1002,"rx":1001,"loss":0.1}', id="no-such-wire"),
]

NON_FINITE = ["Infinity", "-Infinity", "NaN", "1e400", "-1e400"]
NUMERIC_FIELDS = ["delay_us", "jitter_us", "loss", "glitch_rate", "bit_error_rate", "glitch_width_us"]


async def _with_client(tmp_path, fn):
    app = create_app(Settings(sim_dio=False), store=Store(tmp_path / "fault.db"))
    async with app.router.lifespan_context(app), httpx.AsyncClient(
        transport=httpx.ASGITransport(app=app), base_url="http://test"
    ) as client:
        runtime = app.state.runtime
        # Two dio nodes with a single wire 1001 -> 1002.
        a, b = runtime.sim_network.spawn(2, dio=True)
        assert (a.node_id, b.node_id) == (1001, 1002)
        runtime.sim_network.connect_wire(1001, 1002, delay_us=40, loss=0.0)
        return await fn(client, runtime)


def _wire_config(runtime) -> dict:
    (wire,) = runtime.sim_network.topology()["wires"]
    return {k: wire[k] for k in ("delay_us", "loss", "glitch_rate", "bit_error_rate", "jitter_us", "glitch_width_us")}


@pytest.mark.parametrize("body", BAD_BODIES)
async def test_bad_wire_fault_is_a_400_and_changes_nothing(tmp_path, body: bytes) -> None:
    async def scenario(client, runtime):
        before = _wire_config(runtime)
        response = await client.post("/api/sim/fault", content=body, headers=AUTH)
        assert response.status_code == 400, response.text
        assert response.json()["detail"]  # a message, not an empty error
        assert _wire_config(runtime) == before

    await _with_client(tmp_path, scenario)


@pytest.mark.parametrize("value", NON_FINITE)
@pytest.mark.parametrize("field", NUMERIC_FIELDS)
async def test_non_finite_wire_values_are_rejected_and_the_old_config_is_kept(tmp_path, field: str, value: str) -> None:
    async def scenario(client, runtime):
        before = _wire_config(runtime)
        response = await client.post("/api/sim/fault", content=_body(**{field: value}), headers=AUTH)
        assert response.status_code == 400, response.text
        assert "finite" in response.json()["detail"] or "0..1" in response.json()["detail"] or ">= 0" in response.json()["detail"]
        assert _wire_config(runtime) == before

    await _with_client(tmp_path, scenario)


async def test_a_valid_wire_fault_still_works_and_accepts_integers_and_float_ids(tmp_path) -> None:
    async def scenario(client, runtime):
        ok = await client.post("/api/sim/fault", content=_body(loss="0.25", delay_us="80"), headers=AUTH)
        assert ok.status_code == 200
        assert ok.json()["loss"] == 0.25 and ok.json()["delay_us"] == 80.0
        floats = await client.post(
            "/api/sim/fault", content=b'{"kind":"wire","tx":1001.0,"rx":1002.0,"loss":0}', headers=AUTH)
        assert floats.status_code == 200
        assert _wire_config(runtime)["loss"] == 0.0

    await _with_client(tmp_path, scenario)


@pytest.mark.parametrize("body", [
    b'{"kind":"packet_loss","a":1001,"b":1002,"fraction":null}',
    b'{"kind":"stuck_sensor","node_id":1001,"channel":null}',
    b'{"kind":"heap_leak","node_id":[1]}',
    b'{"kind":"brownout","node_id":{}}',
])
async def test_other_fault_kinds_do_not_500_on_junk_either(tmp_path, body: bytes) -> None:
    async def scenario(client, runtime):
        response = await client.post("/api/sim/fault", content=body, headers=AUTH)
        assert response.status_code < 500, response.text

    await _with_client(tmp_path, scenario)

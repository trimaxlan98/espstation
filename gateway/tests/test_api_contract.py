from __future__ import annotations

import asyncio

import httpx
import pytest

from espstation_gateway.app import DEFAULT_TOKEN, Settings, create_app
from espstation_gateway.runtime import GatewayRuntime
from espstation_gateway.store import Store


AUTH = {"Authorization": f"Bearer {DEFAULT_TOKEN}"}


@pytest.mark.asyncio
async def test_cors_allows_local_electron_renderer_preflight(tmp_path) -> None:
    store = Store(tmp_path / "cors.db")
    app = create_app(store=store)
    transport = httpx.ASGITransport(app=app)

    async with app.router.lifespan_context(app), httpx.AsyncClient(
        transport=transport, base_url="http://test"
    ) as client:
        response = await client.options(
            "/api/nodes",
            headers={
                "Origin": "http://localhost:5173",
                "Access-Control-Request-Method": "GET",
                "Access-Control-Request-Headers": "authorization",
            },
        )
        remote_response = await client.options(
            "/api/nodes",
            headers={
                "Origin": "https://example.com",
                "Access-Control-Request-Method": "GET",
                "Access-Control-Request-Headers": "authorization",
            },
        )

    assert response.status_code == 200
    assert response.headers["access-control-allow-origin"] == "http://localhost:5173"
    assert "access-control-allow-origin" not in remote_response.headers


@pytest.mark.asyncio
async def test_sim_node_rest_shapes_match_desktop_contract(tmp_path) -> None:
    store = Store(tmp_path / "contract.db")
    app = create_app(Settings(sim_preload=1), store=store)

    transport = httpx.ASGITransport(app=app)
    async with app.router.lifespan_context(app), httpx.AsyncClient(
        transport=transport, base_url="http://test"
    ) as client:
        nodes = []
        for _ in range(50):
            response = await client.get("/api/nodes", headers=AUTH)
            assert response.status_code == 200
            nodes = response.json()
            if nodes:
                break
            await asyncio.sleep(0.01)

        assert len(nodes) == 1
        summary = nodes[0]
        assert {
            "node_id", "label", "mac", "state", "online", "last_seen",
            "uptime_ms", "heap_free", "rssi", "fw", "target", "link_id",
        } <= summary.keys()
        assert summary["online"] is True

        detail_response = await client.get(f"/api/nodes/{summary['node_id']}", headers=AUTH)
        assert detail_response.status_code == 200
        detail = detail_response.json()
        assert detail["boot"]["count"] == 1
        assert detail["ndb"]
        assert "telemetry" in detail["caps"]

        telemetry = {"channels": {}}
        for _ in range(100):
            telemetry_response = await client.get(
                f"/api/nodes/{summary['node_id']}/telemetry?channels=adc.a0",
                headers=AUTH,
            )
            assert telemetry_response.status_code == 200
            telemetry = telemetry_response.json()
            if telemetry["channels"].get("adc.a0"):
                break
            await asyncio.sleep(0.01)

        assert telemetry["channels"]["adc.a0"]
        assert telemetry["channels"]["adc.a0"][0][0] > 1_000_000_000


@pytest.mark.asyncio
async def test_runtime_stream_envelope_matches_desktop_contract(tmp_path) -> None:
    store = Store(tmp_path / "stream.db")
    runtime = GatewayRuntime(store)
    events: list[dict] = []
    runtime.subscribe(events.append)

    await runtime.start()
    try:
        await runtime.attach_sim(1)
        for _ in range(100):
            if any(event["kind"] == "node" for event in events) and any(
                event["kind"] == "telemetry" for event in events
            ):
                break
            await asyncio.sleep(0.01)

        node_event = next(event for event in events if event["kind"] == "node")
        assert set(node_event) == {"kind", "node_id", "ts", "data"}
        assert node_event["node_id"] == node_event["data"]["node_id"]
        assert "state" in node_event["data"]

        telemetry_event = next(event for event in events if event["kind"] == "telemetry")
        assert set(telemetry_event) == {"kind", "node_id", "ts", "data"}
        assert set(telemetry_event["data"]) == {"channel", "value", "replay"}
        assert telemetry_event["ts"] > 1_000_000_000
    finally:
        await runtime.shutdown()
        store.close()

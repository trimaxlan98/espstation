# FastAPI application: the REST + WS contract the desktop consumes. This is
# a thin adapter -- everything stateful lives in GatewayRuntime (runtime.py)
# so the wiring here can be unit-tested without a real event loop dance
# beyond what httpx.ASGITransport/TestClient already gives us.
#
# Auth: a single bearer token, constant-time compared (hmac.compare_digest)
# per docs/ARCHITECTURE.md's security model. Applied uniformly to every
# route including /api/ping -- a lighter "ping is public" carve-out was
# considered and rejected: one rule ("every request needs the token") is
# easier to get right in the desktop client and easier to audit than two.
# WS auth is `?token=` (browsers can't set a custom header on the WS
# handshake) -- which is exactly why access logging is disabled in
# __main__.py's uvicorn config, so that token never lands in a log line.
from __future__ import annotations

import asyncio
import hmac
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal

from fastapi import Body, Depends, FastAPI, Header, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

from . import __version__
from .protocol import messages as msg
from .runtime import CommandTimeoutError, GatewayRuntime
from .store import DEFAULT_DB_PATH, Store
from .transports.serial_port import InvalidPortPathError, SerialOpenError, SerialPermissionError, list_ports

API_VERSION = "0.1.0"
DEFAULT_TOKEN = "espstation-dev"
DEFAULT_PORT = 8787


@dataclass
class Settings:
    token: str = DEFAULT_TOKEN
    host: str = "127.0.0.1"
    port: int = DEFAULT_PORT
    db_path: Path = DEFAULT_DB_PATH
    sim_preload: int = 0


class LinkCreateBody(BaseModel):
    kind: Literal["serial", "tcp", "sim"]
    path: str | None = None
    baudrate: int = 115200
    baud: int | None = None
    host: str | None = None
    port: int | None = None
    label: str | None = None


class CommandBody(BaseModel):
    op: str
    args: dict[str, Any] = {}


class ExperimentValidateBody(BaseModel):
    node_id: int | None = None
    spec: dict[str, Any]


class SimSpawnBody(BaseModel):
    count: int = 1
    label_prefix: str = "sim"


def create_app(settings: Settings | None = None, *, store: Store | None = None) -> FastAPI:
    settings = settings or Settings()
    app = FastAPI(title="espstation-gateway", version=API_VERSION)
    app.add_middleware(
        CORSMiddleware,
        # Vite uses localhost in development; packaged Electron pages send a
        # null origin. Authorization still requires the gateway bearer token.
        allow_origin_regex=r"^(?:https?://(?:localhost|127\.0\.0\.1)(?::\d+)?|null)$",
        allow_methods=["*"],
        allow_headers=["Authorization", "Content-Type"],
    )
    app.state.settings = settings
    app.state.store = store or Store(settings.db_path)
    app.state.runtime = GatewayRuntime(app.state.store)
    app.state.start_time = time.time()

    def verify_token(authorization: str | None = Header(default=None)) -> None:
        if authorization is None or not authorization.startswith("Bearer "):
            raise HTTPException(status_code=401, detail="missing bearer token")
        token = authorization[len("Bearer "):]
        if not hmac.compare_digest(token, app.state.settings.token):
            raise HTTPException(status_code=401, detail="invalid token")

    auth = Depends(verify_token)

    @app.on_event("startup")
    async def _startup() -> None:
        runtime: GatewayRuntime = app.state.runtime
        await runtime.start()
        if settings.sim_preload > 0:
            await runtime.attach_sim(settings.sim_preload)

    @app.on_event("shutdown")
    async def _shutdown() -> None:
        runtime: GatewayRuntime = app.state.runtime
        await runtime.shutdown()
        app.state.store.close()

    # -- health ---------------------------------------------------------

    @app.get("/api/ping", dependencies=[auth])
    async def ping() -> dict[str, Any]:
        return {
            "name": "espstation-gateway",
            "version": __version__,
            "api": API_VERSION,
            "uptime": time.time() - app.state.start_time,
        }

    # -- ports / links --------------------------------------------------

    @app.get("/api/ports", dependencies=[auth])
    async def ports() -> list[dict[str, Any]]:
        runtime: GatewayRuntime = app.state.runtime
        in_use_paths = {
            l.meta.get("path") for l in runtime.links.values() if l.kind == "serial" and l.connected
        }
        return [
            {"path": p.path, "vid": p.vid, "pid": p.pid, "description": p.description, "in_use": p.path in in_use_paths}
            for p in list_ports()
        ]

    @app.get("/api/links", dependencies=[auth])
    async def get_links() -> list[dict[str, Any]]:
        runtime: GatewayRuntime = app.state.runtime
        return runtime.list_link_summaries()

    @app.post("/api/links", dependencies=[auth])
    async def create_link(body: LinkCreateBody) -> dict[str, Any]:
        runtime: GatewayRuntime = app.state.runtime
        try:
            if body.kind == "serial":
                if not body.path:
                    raise HTTPException(status_code=400, detail="'path' is required for kind=serial")
                link = await runtime.attach_serial(body.path, body.baud or body.baudrate)
            elif body.kind == "tcp":
                if not body.host or not body.port:
                    raise HTTPException(status_code=400, detail="'host' and 'port' are required for kind=tcp")
                link = await runtime.attach_tcp(body.host, body.port)
            else:
                links = await runtime.attach_sim(1, label_prefix=body.label or "sim")
                link = links[0]
        except (InvalidPortPathError, SerialPermissionError, SerialOpenError) as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        return runtime.link_summary(link)

    @app.delete("/api/links/{link_id}", dependencies=[auth])
    async def delete_link(link_id: str) -> dict[str, Any]:
        runtime: GatewayRuntime = app.state.runtime
        try:
            await runtime.detach_link(link_id)
        except KeyError:
            raise HTTPException(status_code=404, detail=f"no such link {link_id!r}")
        return {"ok": True}

    # -- nodes ------------------------------------------------------------

    @app.get("/api/nodes", dependencies=[auth])
    async def get_nodes() -> list[dict[str, Any]]:
        runtime: GatewayRuntime = app.state.runtime
        return runtime.list_node_summaries()

    @app.get("/api/nodes/{node_id}", dependencies=[auth])
    async def get_node(node_id: int) -> dict[str, Any]:
        runtime: GatewayRuntime = app.state.runtime
        detail = runtime.node_detail(node_id)
        if detail is None:
            raise HTTPException(status_code=404, detail=f"no such node {node_id}")
        return detail

    @app.post("/api/nodes/{node_id}/command", dependencies=[auth])
    async def node_command(node_id: int, body: CommandBody) -> dict[str, Any]:
        runtime: GatewayRuntime = app.state.runtime
        try:
            ack = await runtime.send_command(node_id, body.op, body.args)
        except KeyError as exc:
            raise HTTPException(status_code=404, detail=str(exc))
        except CommandTimeoutError as exc:
            raise HTTPException(status_code=504, detail=str(exc))
        return ack.model_dump(by_alias=True)

    @app.get("/api/nodes/{node_id}/telemetry", dependencies=[auth])
    async def node_telemetry(
        node_id: int,
        since: float | None = None,
        until: float | None = None,
        channels: str | None = Query(default=None),
        max_points: int | None = None,
    ) -> dict[str, dict[str, list[list[float]]]]:
        runtime: GatewayRuntime = app.state.runtime
        ndb = runtime.registry.get(node_id)
        channel_keys = channels.split(",") if channels else None
        if channel_keys and ndb is None:
            raise HTTPException(status_code=404, detail=f"no NDB for node {node_id}")
        try:
            channel_ids = [ndb.by_key(key).id for key in channel_keys] if channel_keys and ndb else None
        except KeyError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        rows = runtime.store.query_samples(
            node_id, channel_ids=channel_ids, since=since, until=until, max_points=max_points
        )
        result: dict[str, list[list[float]]] = {}
        for row in rows:
            channel_id = row["channel_id"]
            key = ndb.by_id(channel_id).key if ndb else str(channel_id)
            result.setdefault(key, []).append([row["ts"], row["value"]])
        return {"channels": result}

    @app.post("/api/nodes/{node_id}/experiment", dependencies=[auth])
    async def push_experiment(node_id: int, spec: msg.ExperimentSpec) -> dict[str, Any]:
        runtime: GatewayRuntime = app.state.runtime
        try:
            errors = await runtime.push_experiment(node_id, spec)
        except KeyError as exc:
            raise HTTPException(status_code=404, detail=str(exc))
        if errors:
            raise HTTPException(status_code=422, detail={"errors": errors})
        return {"ok": True, "id": spec.id}

    # -- experiment library -------------------------------------------------

    @app.get("/api/experiments", dependencies=[auth])
    async def list_experiments() -> list[dict[str, Any]]:
        return app.state.store.list_experiments()

    @app.put("/api/experiments/{exp_id}", dependencies=[auth])
    async def put_experiment(exp_id: str, spec: msg.ExperimentSpec) -> dict[str, Any]:
        if spec.id != exp_id:
            raise HTTPException(status_code=400, detail="body 'id' must match the path exp_id")
        app.state.store.put_experiment(exp_id, spec.model_dump(by_alias=True))
        return {"ok": True}

    @app.post("/api/experiments/validate", dependencies=[auth])
    async def validate_experiment(body: ExperimentValidateBody) -> dict[str, Any]:
        runtime: GatewayRuntime = app.state.runtime
        try:
            spec = msg.ExperimentSpec.model_validate(body.spec)
        except Exception as exc:
            return {"valid": False, "errors": [str(exc)]}
        errors: list[str] = []
        if body.node_id is not None:
            ndb = runtime.registry.get(body.node_id)
            if ndb is None:
                errors.append(f"no NDB for node {body.node_id} (no HELLO seen yet)")
            else:
                errors.extend(ndb.validate_spec(spec))
        return {"valid": not errors, "errors": errors}

    # -- runs ---------------------------------------------------------------

    @app.get("/api/runs", dependencies=[auth])
    async def list_runs(node_id: int | None = None) -> list[dict[str, Any]]:
        return app.state.store.list_runs(node_id)

    @app.get("/api/runs/{run_id}", dependencies=[auth])
    async def get_run(run_id: str) -> dict[str, Any]:
        run = app.state.store.get_run(run_id)
        if run is None:
            raise HTTPException(status_code=404, detail=f"no such run {run_id!r}")
        return run

    @app.get("/api/runs/{run_id}/samples", dependencies=[auth])
    async def get_run_samples(run_id: str) -> list[dict[str, Any]]:
        return app.state.store.run_samples(run_id)

    # -- network / sim --------------------------------------------------

    @app.get("/api/network/topology", dependencies=[auth])
    async def network_topology() -> dict[str, Any]:
        # Sim mesh topology is real (loss/latency are simulated but the
        # graph structure and counters are live). Real-hardware NET_REPORT
        # frames are processed and broadcast over /ws/stream as "node"
        # events already; folding them into this same persistent snapshot
        # is not yet wired -- see the report for this known gap.
        runtime: GatewayRuntime = app.state.runtime
        return runtime.sim_network.topology()

    @app.get("/api/sim/scenarios", dependencies=[auth])
    async def sim_scenarios() -> list[dict[str, Any]]:
        return [
            {"id": "single-node", "name": "Single node", "description": "One sim node with the default NDB.", "node_count": 1},
            {"id": "small-mesh", "name": "Small mesh", "description": "3-node ESP-NOW-like mesh with light loss.", "node_count": 3},
            {"id": "large-mesh", "name": "Large mesh", "description": "20-node mesh for demoing the topology graph at scale.", "node_count": 20},
        ]

    @app.post("/api/sim/spawn", dependencies=[auth])
    async def sim_spawn(body: SimSpawnBody) -> list[dict[str, Any]]:
        runtime: GatewayRuntime = app.state.runtime
        links = await runtime.attach_sim(body.count, label_prefix=body.label_prefix)
        return [runtime.link_summary(l) for l in links]

    @app.post("/api/sim/fault", dependencies=[auth])
    async def sim_fault(payload: dict[str, Any] = Body(...)) -> dict[str, Any]:
        runtime: GatewayRuntime = app.state.runtime
        payload = dict(payload)
        kind = payload.pop("kind", None)
        if not kind:
            raise HTTPException(status_code=400, detail="'kind' is required")
        node_id = payload.pop("node_id", None)
        try:
            return runtime.sim_network.apply_fault(kind, node_id=node_id, **payload)
        except (KeyError, ValueError) as exc:
            raise HTTPException(status_code=400, detail=str(exc))

    # -- WebSocket --------------------------------------------------------

    @app.websocket("/ws/stream")
    async def ws_stream(websocket: WebSocket, token: str = Query(default="")) -> None:
        if not hmac.compare_digest(token, app.state.settings.token):
            await websocket.close(code=1008)
            return
        await websocket.accept()
        runtime: GatewayRuntime = app.state.runtime
        queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()

        async def on_event(evt: dict[str, Any]) -> None:
            await queue.put(evt)

        runtime.subscribe(on_event)
        try:
            while True:
                evt = await queue.get()
                await websocket.send_json(evt)
        except WebSocketDisconnect:
            pass
        finally:
            runtime.unsubscribe(on_event)

    return app

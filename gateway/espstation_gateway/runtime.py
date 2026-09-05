# GatewayRuntime: the live, in-memory session state that ties transports,
# the protocol codec, the NDB registry and the SQLite store together. Kept
# separate from app.py so the FastAPI layer stays a thin HTTP/WS adapter
# over something that's independently testable (and independently usable
# from __main__.py without spinning up uvicorn).
from __future__ import annotations

import asyncio
import json
import time
from typing import Any, Awaitable, Callable

from .protocol import messages as msg
from .protocol import spec as protocol_spec
from .protocol.frames import Frame, encode as encode_frame
from .protocol.ndb import NodeRegistry, UnknownChannelError
from .store import Store
from .transports.base import Link, LinkEvent, RawFrameDecoder
from .transports.serial_port import SerialTransport
from .transports.sim.network import SimNetwork
from .transports.sim.node import SimTransport
from .transports.tcp import LengthPrefixDecoder, TcpTransport
from .protocol.frames import StreamingDecoder

Subscriber = Callable[[dict[str, Any]], "Awaitable[None] | None"]


class CommandTimeoutError(RuntimeError):
    pass


class GatewayRuntime:
    def __init__(self, store: Store) -> None:
        self.store = store
        self.registry = NodeRegistry()
        self.links: dict[str, Link] = {}
        self.node_link: dict[int, str] = {}
        self.sim_network = SimNetwork()
        self._hello_by_node: dict[int, msg.Hello] = {}
        self._heartbeat_by_node: dict[int, msg.Heartbeat] = {}

        self._link_seq = 0
        self._cmd_id_seq = 0
        self._pending_cmds: dict[tuple[int, int], asyncio.Future] = {}
        self._active_run: dict[int, str] = {}
        self._time_sync_pending: dict[int, int] = {}
        self._subscribers: list[Subscriber] = []
        self._tasks: list[asyncio.Task] = []

    # -- pub/sub for the WS layer -------------------------------------------

    def subscribe(self, callback: Subscriber) -> None:
        self._subscribers.append(callback)

    def unsubscribe(self, callback: Subscriber) -> None:
        if callback in self._subscribers:
            self._subscribers.remove(callback)

    async def _publish(self, kind: str, data: Any) -> None:
        node_id = data.get("node_id") if isinstance(data, dict) else None
        timestamp = data.get("ts", time.time()) if isinstance(data, dict) else time.time()
        if isinstance(data, dict) and kind != "node":
            data = {key: value for key, value in data.items() if key not in ("node_id", "ts")}
        event = {"kind": kind, "node_id": node_id, "ts": timestamp, "data": data}
        for cb in list(self._subscribers):
            result = cb(event)
            if asyncio.iscoroutine(result):
                await result

    # -- lifecycle ------------------------------------------------------

    async def start(self) -> None:
        self._tasks.append(asyncio.create_task(self._time_sync_loop(), name="time-sync-loop"))

    async def shutdown(self) -> None:
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            try:
                await t
            except (asyncio.CancelledError, Exception):
                pass
        self._tasks = []
        for link in list(self.links.values()):
            await link.stop()
        await self.sim_network.stop()

    # -- link management --------------------------------------------------

    def _new_link_id(self, kind: str) -> str:
        self._link_seq += 1
        return f"{kind}-{self._link_seq}"

    async def _start_link(self, link: Link, meta: dict[str, Any] | None = None) -> Link:
        if meta:
            link.meta.update(meta)
        link.subscribe(self._link_listener(link))
        await link.start()
        self.links[link.id] = link
        self.store.record_link(link.id, link.kind, link.meta)
        await self._publish("link", self.link_summary(link))
        return link

    async def attach_serial(self, path: str, baudrate: int = 115200) -> Link:
        transport = SerialTransport(path, baudrate)
        link = Link(self._new_link_id("serial"), "serial", transport, StreamingDecoder(), self.registry)
        return await self._start_link(link, {"path": path, "baudrate": baudrate})

    async def attach_tcp(self, host: str, port: int) -> Link:
        transport = TcpTransport(host, port)
        link = Link(self._new_link_id("tcp"), "tcp", transport, LengthPrefixDecoder(), self.registry)
        return await self._start_link(link, {"host": host, "port": port})

    async def attach_sim(self, count: int = 1, *, label_prefix: str = "sim") -> list[Link]:
        nodes = self.sim_network.spawn(count, label_prefix=label_prefix)
        links = []
        for node in nodes:
            transport = SimTransport(node)
            link = Link(self._new_link_id("sim"), "sim", transport, RawFrameDecoder(), self.registry)
            await self._start_link(link, {"node_id": node.node_id, "label": node.label})
            links.append(link)
        self.sim_network.ensure_report_loop()
        return links

    async def detach_link(self, link_id: str) -> None:
        link = self.links.pop(link_id, None)
        if link is None:
            raise KeyError(f"no such link {link_id!r}")
        await link.stop()
        self.store.close_link(link_id)
        await self._publish("link", {"id": link_id, "detached": True})

    def link_summary(self, link: Link) -> dict[str, Any]:
        summary = {"id": link.id, "kind": link.kind, "connected": link.connected, **link.meta}
        if "baudrate" in summary:
            summary["baud"] = summary.pop("baudrate")
        return summary

    def list_link_summaries(self) -> list[dict[str, Any]]:
        return [self.link_summary(l) for l in self.links.values()]

    # -- inbound frame dispatch ---------------------------------------------

    def _link_listener(self, link: Link) -> Callable[[LinkEvent], Awaitable[None]]:
        async def _on_event(event: LinkEvent) -> None:
            await self._handle_link_event(link, event)
        return _on_event

    async def _handle_link_event(self, link: Link, event: LinkEvent) -> None:
        if event.kind == "raw":
            payload = event.payload
            text = payload if isinstance(payload, str) else bytes(payload).decode("utf-8", errors="replace")
            await self._publish("raw", {"link_id": link.id, "text": text})
            return
        if event.kind == "error":
            link.connected = False
            await self._publish("link", {"id": link.id, "error": str(event.payload)})
            return
        frame: Frame = event.payload  # event.kind == "frame"
        self.node_link[frame.node] = link.id
        await self._dispatch_frame(link, frame)

    async def _dispatch_frame(self, link: Link, frame: Frame) -> None:
        try:
            decoded = msg.decode_message(frame.type, frame.payload)
        except msg.MessageDecodeError as exc:
            await self._publish("raw", {"link_id": link.id, "text": f"[decode error type={frame.type:#04x} node={frame.node}] {exc}"})
            return

        if frame.type == msg.TYPE_HELLO and isinstance(decoded, msg.Hello):
            await self._on_hello(link, decoded)
        elif frame.type == msg.TYPE_HEARTBEAT and isinstance(decoded, msg.Heartbeat):
            self._heartbeat_by_node[frame.node] = decoded
            self.store.touch_node(frame.node)
            await self._publish("heartbeat", {
                "node_id": frame.node, "uptime_ms": decoded.uptime_ms, "heap_free": decoded.heap_free,
                "heap_min": decoded.heap_min, "state": decoded.state_name(), "rssi": decoded.rssi,
                "flags": sorted(decoded.flag_set()),
            })
            summary = self.node_summary(frame.node)
            if summary is not None:
                await self._publish("node", summary)
        elif frame.type == msg.TYPE_TELEMETRY and isinstance(decoded, msg.Telemetry):
            await self._on_telemetry(frame.node, frame.seq, decoded)
        elif frame.type == msg.TYPE_EVENT and isinstance(decoded, msg.Event):
            ts = self.store.to_epoch(frame.node, decoded.ts_ms)
            self.store.record_event(frame.node, ts, decoded.code, decoded.severity, decoded.data)
            await self._publish("event", {"node_id": frame.node, "ts": ts, "code": decoded.code, "severity": decoded.severity, "data": decoded.data})
        elif frame.type == msg.TYPE_LOG and isinstance(decoded, msg.Log):
            ts = self.store.to_epoch(frame.node, decoded.ts_ms)
            self.store.record_log(frame.node, ts, decoded.level, decoded.tag, decoded.msg)
            await self._publish("log", {"node_id": frame.node, "ts": ts, "level": decoded.level_name(), "tag": decoded.tag, "message": decoded.msg})
        elif frame.type == msg.TYPE_CMD_ACK and isinstance(decoded, msg.CmdAck):
            self._resolve_cmd_ack(frame.node, decoded)
        elif frame.type == msg.TYPE_EXP_STATE and isinstance(decoded, msg.ExpState):
            self._on_exp_state(frame.node, decoded)
            await self._publish("event", {"node_id": frame.node, "code": "exp.state", "severity": "info", "data": decoded.model_dump()})
        elif frame.type == msg.TYPE_NET_REPORT and isinstance(decoded, msg.NetReport):
            await self._publish("node", {"node_id": frame.node, "net_report": decoded.model_dump()})
        elif frame.type == msg.TYPE_TIME_SYNC and isinstance(decoded, msg.TimeSync):
            self._on_time_sync_reply(frame.node, decoded)
        else:
            # BULK_* and 0x80-0xFF experiment-defined codes: pass through
            # opaquely, per PROTOCOL.md section 4's reserved-range note.
            await self._publish("raw", {"link_id": link.id, "node_id": frame.node, "type": frame.type, "opaque": True})

    async def _on_hello(self, link: Link, hello: msg.Hello) -> None:
        self.registry.on_hello(hello)
        self._hello_by_node[hello.node_id] = hello
        self.node_link[hello.node_id] = link.id
        self.store.upsert_node(
            hello.node_id, mac=hello.mac, label=hello.label,
            chip=hello.chip.model_dump(), fw=hello.fw.model_dump(), caps=hello.caps,
            ndb=[c.model_dump() for c in hello.ndb],
        )
        now_us = int(time.time() * 1_000_000)
        self.store.record_time_sync(hello.node_id, now_us, hello.boot.uptime_ms, hello.boot.uptime_ms, now_us)
        ack = msg.HelloAck(
            session=f"sess-{hello.node_id}-{int(time.time() * 1000)}",
            host_time=time.time(), accepted=True,
            policy=msg.HelloAckPolicy(telemetry_rate_limit_hz=float(protocol_spec.timing().get("telemetry_rate_limit_hz", 200))),
        )
        frame_bytes = encode_frame(msg.TYPE_HELLO_ACK, 0, link.next_seq(), ack.to_payload())
        await link.send_frame(frame_bytes)
        summary = self.node_summary(hello.node_id)
        if summary is not None:
            await self._publish("node", summary)

    async def _on_telemetry(self, node_id: int, frame_seq: int, telemetry: msg.Telemetry) -> None:
        ndb = self.registry.get(node_id)
        rows: list[tuple[int, float, float]] = []
        ws_samples: list[dict[str, Any]] = []
        for s in telemetry.samples:
            ts_ms = telemetry.base_ts_ms + s.dt_ms
            ts = self.store.to_epoch(node_id, ts_ms)
            try:
                value = ndb.convert(s.ch, s.value) if ndb is not None else s.value
            except UnknownChannelError:
                continue
            rows.append((s.ch, ts, float(value)))
            channel = ndb.by_id(s.ch).key if ndb is not None else str(s.ch)
            ws_samples.append({"channel": channel, "ts": ts, "value": value})

        run_id = self._active_run.get(node_id)
        self.store.commit_telemetry(node_id, frame_seq, rows, run_id=run_id)

        watermark = self.store.watermark(node_id)
        link = self.links.get(self.node_link.get(node_id, ""))
        if link is not None and watermark is not None:
            ack = msg.TelemAck(node=node_id, last_seq=watermark, flags=0)
            frame_bytes = encode_frame(msg.TYPE_TELEM_ACK, 0, link.next_seq(), ack.to_bytes())
            await link.send_frame(frame_bytes)

        replay = "replay" in telemetry.flag_set()
        for sample in ws_samples:
            await self._publish("telemetry", {"node_id": node_id, **sample, "replay": replay})

    def _on_exp_state(self, node_id: int, state: msg.ExpState) -> None:
        if state.state == "running":
            self._active_run[node_id] = state.run_id
        elif self._active_run.get(node_id) == state.run_id and state.state in ("done", "aborted"):
            del self._active_run[node_id]
        self.store.upsert_run(
            state.run_id, node_id, spec_hash=state.spec_hash, state=state.state,
            started_at=self.store.to_epoch(node_id, state.started_at_ms) if state.started_at_ms else None,
            ended_at=time.time() if state.state in ("done", "aborted") else None,
        )

    def _resolve_cmd_ack(self, node_id: int, ack: msg.CmdAck) -> None:
        fut = self._pending_cmds.pop((node_id, ack.id), None)
        if fut is not None and not fut.done():
            fut.set_result(ack)

    # -- outbound: commands, experiments -------------------------------

    async def send_command(self, node_id: int, op: str, args: dict[str, Any] | None = None) -> msg.CmdAck:
        link = self.links.get(self.node_link.get(node_id, ""))
        if link is None:
            raise KeyError(f"no link for node {node_id}")
        timing = protocol_spec.timing()
        timeout_s = float(timing.get("cmd_timeout_s", 2))
        retries = int(timing.get("cmd_retries", 2))
        self._cmd_id_seq += 1
        cmd_id = self._cmd_id_seq
        cmd = msg.Cmd(id=cmd_id, op=op, args=args or {})

        last_exc: Exception = CommandTimeoutError(f"CMD {op} to node {node_id} never sent")
        for attempt in range(retries + 1):
            fut: asyncio.Future = asyncio.get_running_loop().create_future()
            self._pending_cmds[(node_id, cmd_id)] = fut
            frame_bytes = encode_frame(msg.TYPE_CMD, node_id, link.next_seq(), cmd.to_payload())
            await link.send_frame(frame_bytes)
            try:
                return await asyncio.wait_for(fut, timeout_s)
            except asyncio.TimeoutError:
                self._pending_cmds.pop((node_id, cmd_id), None)
                last_exc = CommandTimeoutError(
                    f"CMD {op} (id={cmd_id}) to node {node_id} timed out after {attempt + 1} attempt(s)"
                )
        raise last_exc

    async def push_experiment(self, node_id: int, spec: msg.ExperimentSpec) -> list[str]:
        """EXPERIMENTS.md validation gate 1 (station-side), then send
        EXP_SET. Returns a list of validation errors (empty == sent)."""
        ndb = self.registry.get(node_id)
        if ndb is None:
            raise KeyError(f"no NDB for node {node_id} (no HELLO seen yet)")
        errors = ndb.validate_spec(spec)
        if errors:
            return errors
        link = self.links.get(self.node_link.get(node_id, ""))
        if link is None:
            raise KeyError(f"no link for node {node_id}")
        frame_bytes = encode_frame(msg.TYPE_EXP_SET, node_id, link.next_seq(), spec.to_payload())
        await link.send_frame(frame_bytes)
        self.store.put_experiment(spec.id, spec.model_dump(by_alias=True))
        return []

    # -- TIME_SYNC loop -------------------------------------------------

    async def _time_sync_loop(self) -> None:
        try:
            while True:
                await asyncio.sleep(30)
                for node_id, link_id in list(self.node_link.items()):
                    link = self.links.get(link_id)
                    if link is None:
                        continue
                    t1 = int(time.time() * 1_000_000)
                    self._time_sync_pending[node_id] = t1
                    ts = msg.TimeSync(t1_host_us=t1, t2_node_ms=0, t3_node_ms=0)
                    frame_bytes = encode_frame(msg.TYPE_TIME_SYNC, node_id, link.next_seq(), ts.to_bytes())
                    await link.send_frame(frame_bytes)
        except asyncio.CancelledError:
            return

    def _on_time_sync_reply(self, node_id: int, reply: msg.TimeSync) -> None:
        t1 = self._time_sync_pending.pop(node_id, None)
        if t1 is None or reply.t1_host_us != t1:
            return  # stale or unsolicited reply
        t4 = int(time.time() * 1_000_000)
        self.store.record_time_sync(node_id, t1, reply.t2_node_ms, reply.t3_node_ms, t4)

    # -- read models for the REST layer --------------------------------

    def node_summary(self, node_id: int) -> dict[str, Any] | None:
        row = self.store.get_node(node_id)
        if row is None:
            return None
        hello = self._hello_by_node.get(node_id)
        heartbeat = self._heartbeat_by_node.get(node_id)
        fw = json.loads(row["fw_json"]) if row["fw_json"] else {}
        link_id = self.node_link.get(node_id)
        link = self.links.get(link_id) if link_id else None
        return {
            "node_id": node_id,
            "mac": row["mac"],
            "label": row["label"],
            "state": heartbeat.state_name() if heartbeat else "boot",
            "online": bool(link and link.connected),
            "uptime_ms": heartbeat.uptime_ms if heartbeat else (hello.boot.uptime_ms if hello else 0),
            "heap_free": heartbeat.heap_free if heartbeat else 0,
            "rssi": heartbeat.rssi if heartbeat else 0,
            "fw": fw.get("version", "unknown"),
            "target": fw.get("target", "unknown"),
            "link_id": link_id,
            "last_seen": row["last_seen"],
        }

    def node_detail(self, node_id: int) -> dict[str, Any] | None:
        summary = self.node_summary(node_id)
        if summary is None:
            return None
        row = self.store.get_node(node_id)
        ndb = self.registry.get(node_id)
        hello = self._hello_by_node.get(node_id)
        summary["ndb"] = [c.model_dump() for c in ndb.channels_by_id.values()] if ndb else []
        summary["caps"] = json.loads(row["caps_json"]) if row and row["caps_json"] else []
        summary["boot"] = hello.boot.model_dump() if hello else {"count": 0, "reason": "unknown", "uptime_ms": 0}
        return summary

    def list_node_summaries(self) -> list[dict[str, Any]]:
        return [s for r in self.store.list_nodes() if (s := self.node_summary(r["node_id"])) is not None]

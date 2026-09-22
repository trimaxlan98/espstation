# GatewayRuntime: the live, in-memory session state that ties transports,
# the protocol codec, the NDB registry and the SQLite store together. Kept
# separate from app.py so the FastAPI layer stays a thin HTTP/WS adapter
# over something that's independently testable (and independently usable
# from __main__.py without spinning up uvicorn).
from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import Any, Awaitable, Callable

from .protocol import messages as msg
from .protocol import spec as protocol_spec
from .protocol.frames import Frame, encode as encode_frame
from .protocol.ndb import NodeRegistry, UnknownChannelError
from .store import Store
from .transports.base import Link, LinkEvent, RawFrameDecoder
from .transports.serial_port import SerialTransport
from .transports.morse_sketch import (
    MorseLogReplayTransport, MorseSketchDecoder, MorseSketchTransport,
)
from .transports.sim.network import SimNetwork
from .transports.sim.node import DioConfig, SimNode, SimTransport
from .transports.tcp import LengthPrefixDecoder, TcpTransport
from .protocol.frames import StreamingDecoder

log = logging.getLogger(__name__)

# Two HELLOs from the same node over the same link, less than this many
# seconds apart, are chunks of ONE announcement (a NDB too big for one frame
# goes out as several HELLOs, sent back to back: milliseconds apart) and share
# a session. A gap this long or longer means the node came back (reconnect,
# reboot, or the 30 s HELLO retry), so a new session opens. 5 s is far above
# the spacing of chunks on a slow serial link and far below the 30 s retry.
HELLO_ANNOUNCE_GAP_S = 5.0

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
        # node id -> channel id -> samples dropped because the channel is not
        # in that node's NDB. Filled in silence otherwise: with a chunked
        # HELLO, a lost chunk means charts that never appear.
        self.unknown_channel_samples: dict[int, dict[int, int]] = {}
        # node id -> (link id, monotonic time of the last HELLO chunk, session
        # id). Injectable clock so the announcement window is testable.
        self._now: Callable[[], float] = time.monotonic
        self._announcements: dict[int, tuple[str, float, str]] = {}
        self._last_session_ms: dict[int, int] = {}

        self._link_seq = 0
        self._cmd_id_seq = 0
        self._pending_cmds: dict[tuple[int, int], asyncio.Future] = {}
        self._active_run: dict[int, str] = {}
        self._time_sync_pending: dict[int, int] = {}
        self._subscribers: list[Subscriber] = []
        self._tasks: list[asyncio.Task] = []
        # link id -> the poll task that keeps a morse adapter's source alive
        self._morse_tasks: dict[str, asyncio.Task] = {}

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
        morse_tasks = list(self._morse_tasks.values())
        self._morse_tasks = {}
        for t in morse_tasks:
            t.cancel()
        for t in morse_tasks:
            # Awaiting is what stops "Task was destroyed but it is pending"
            # and, more usefully, guarantees the poll is not mid-write to a
            # port that link.stop() is about to close underneath it.
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

    def _morse_node_id(self, source: str) -> int:
        """Node id for a Morse adapter, derived from its source path.

        0x4D00 is 'M' in the high byte, so one of these is recognisable in a
        frame dump without looking anything up, and deriving the low byte
        from the path is what makes the same board keep its identity across
        reconnects -- the sketch never announces a MAC, so the path is the
        only identity there is.

        Only 8 bits of it survive, though, so two different sources CAN land
        on the same id (a bench board and a replay capture, or two ports
        whose names happen to sum alike). Two links sharing a node id merge
        into one node downstream and their TELEM_ACKs cross, so a collision
        with a link that is still attached is stepped over. A reconnect to a
        path nobody else holds still gets its own stable id.
        """
        base = 0x4D00 | (sum(source.encode()) & 0xFF)
        taken = {link.meta.get("node_id") for link in self.links.values()}
        node_id = base
        for _ in range(256):
            if node_id not in taken:
                return node_id
            node_id = 0x4D00 | ((node_id + 1) & 0xFF)
        return base

    async def attach_morse_sketch(self, path: str, baudrate: int = 115200,
                                  *, label: str = "", verbose: bool = True) -> Link:
        """Attach a board running the morse-duplex bench sketch.

        The sketch speaks plain text, not ENLP, so the adapter sits in the
        decoder slot and turns it into real frames (transports/morse_sketch.py).
        Downstream this is an ordinary node -- with `read_only` in its caps,
        because it is one.

        The node id is derived from the port path so the same board keeps its
        identity across reconnects; that is the only identity available, since
        the sketch never announces a MAC.
        """
        node_id = self._morse_node_id(path)
        transport = MorseSketchTransport(SerialTransport(path, baudrate))
        decoder = MorseSketchDecoder(node_id, label=label or f"morse {path}")
        link = Link(self._new_link_id("morse"), "morse", transport, decoder, self.registry)
        await self._start_link(
            link, {"path": path, "baudrate": baudrate, "node_id": node_id,
                   "adapter": "morse-duplex-sketch", "commands": False,
                   "primes": "r/v"})
        task = asyncio.create_task(
            self._morse_poll(link, transport, decoder, verbose=verbose),
            name=f"morse-poll-{link.id}")
        self._morse_tasks[link.id] = task
        # A poll that ends on its own (the port died, the link went down) must
        # not leave a finished Task in the dict: attach/detach cycles would
        # accumulate them for the lifetime of the gateway.
        task.add_done_callback(
            lambda t, lid=link.id: self._morse_tasks.pop(lid, None)
            if self._morse_tasks.get(lid) is t else None)
        return link

    async def attach_morse_replay(self, path: str, *, label: str = "",
                                  speed: float = 1.0, loop: bool = False) -> Link:
        """Attach a recorded bench capture as if it were a board.

        Same adapter, same decoder, same frames as attach_morse_sketch: only
        the source of the bytes changes. This is how the Morse practice is
        demonstrated with zero hardware, which is the promise the whole
        project makes about the simulator [D-8] -- and because it exercises
        the real path rather than a mock of it, a demo that works here is
        evidence the live path works too.
        """
        node_id = self._morse_node_id(path)
        transport = MorseSketchTransport(MorseLogReplayTransport(path, speed=speed, loop=loop))
        decoder = MorseSketchDecoder(node_id, label=label or f"replay {path}")
        link = Link(self._new_link_id("morse"), "morse", transport, decoder, self.registry)
        return await self._start_link(
            link, {"path": path, "node_id": node_id, "adapter": "morse-duplex-sketch",
                   "replay": True, "speed": speed, "commands": False})

    async def _morse_poll(self, link: Link, transport: MorseSketchTransport,
                          decoder: MorseSketchDecoder, *, verbose: bool,
                          period_s: float = 5.0) -> None:
        """Keeps the adapter's own data source producing.

        The sketch is silent when nobody keys, and it prints its counters only
        when asked. Opening the port also resets the board, which clears
        verbose -- so the channels this adapter declared would stay empty
        forever unless it asks. `r` is a pure read; `v` is sent at most once,
        and only if the board says verbose is off. Neither is an operator
        command: those still cannot reach the board (morse_sketch.send()).
        """
        primed = not verbose
        # A board that never answers must not be polled at the priming
        # cadence forever: ~10 s of asking, then settle to period_s and let
        # the empty channels say what they say.
        tries_left = 12
        try:
            await asyncio.sleep(1.2)          # let the boot banner finish
            await transport.write_text("r\n")
            while True:
                await asyncio.sleep(period_s if primed else 0.8)
                # Re-checked every cycle rather than exactly once, 0.8 s after
                # attach: if the board had not answered `r` yet -- a slow boot,
                # a reply split across reads, a USB hub that buffers --
                # `decoder.verbose` was still None, the single early check gave
                # up for the whole session and morse.pulse_ms / morse.gap_ms
                # stayed empty forever, which is the exact failure D-22 says
                # `v` exists to prevent. Still at most one `v` per attach.
                if not primed:
                    tries_left -= 1
                    if tries_left <= 0:
                        primed = True
                        log.warning(
                            "morse link %s: the board never reported its mode, "
                            "so verbose was not primed; morse.pulse_ms and "
                            "morse.gap_ms will stay empty for this session",
                            link.id)
                if not primed and decoder.verbose is not None:
                    primed = True
                    if decoder.verbose == 0:
                        await transport.write_text("v\n")
                        await asyncio.sleep(0.3)
                if not link.connected:
                    return
                await transport.write_text("r\n")
        except asyncio.CancelledError:
            raise
        except Exception as exc:              # the port went away; the pump reports it
            await self._publish("link", {"id": link.id, "poll_error": str(exc)})

    async def attach_tcp(self, host: str, port: int) -> Link:
        transport = TcpTransport(host, port)
        link = Link(self._new_link_id("tcp"), "tcp", transport, LengthPrefixDecoder(), self.registry)
        return await self._start_link(link, {"host": host, "port": port})

    async def attach_sim(self, count: int = 1, *, label_prefix: str = "sim") -> list[Link]:
        return await self._attach_sim_nodes(self.sim_network.spawn(count, label_prefix=label_prefix))

    async def attach_sim_dio_pair(
        self, *, label_prefix: str = "dio", delay_us: float = 40.0, loss: float = 0.005,
        glitch_rate: float = 0.2, bit_error_rate: float = 3e-4, jitter_us: float = 4.0,
        blink_hz: float = 0.5,
    ) -> list[Link]:
        """Two simulated nodes joined by a virtual two-wire cable (one wire
        per direction), with defaults lively enough to give the charts
        something to show: a little loss, the odd glitch, a small non-zero
        BER. `blink_hz` toggles each node's dio.tx as demo stimulus."""
        a, b = self.sim_network.spawn(2, label_prefix=label_prefix, dio=DioConfig(blink_hz=blink_hz))
        wire = dict(delay_us=delay_us, loss=loss, glitch_rate=glitch_rate,
                    bit_error_rate=bit_error_rate, jitter_us=jitter_us)
        self.sim_network.connect_wire(a.node_id, b.node_id, **wire)
        self.sim_network.connect_wire(b.node_id, a.node_id, **wire)
        return await self._attach_sim_nodes([a, b])

    async def _attach_sim_nodes(self, nodes: list[SimNode]) -> list[Link]:
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
        task = self._morse_tasks.pop(link_id, None)
        if task is not None:
            task.cancel()
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
        if event.kind == "closed":
            # The medium ended cleanly (a replayed capture ran out, a peer
            # closed). Not an error, but the link is done: say so, or the app
            # keeps a node online that can never produce another sample.
            link.connected = False
            await self._publish("link", self.link_summary(link))
            for node_id, lid in self.node_link.items():
                if lid == link.id:
                    summary = self.node_summary(node_id)
                    if summary is not None:
                        await self._publish("node", summary)
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
        # One anchor per chunk, on purpose: each chunk carries the node's
        # current uptime and is a valid (if coarse) measurement of the clock
        # offset, so a chunked announcement just gives a few more of them.
        self.store.record_time_sync(hello.node_id, now_us, hello.boot.uptime_ms, hello.boot.uptime_ms, now_us)
        session = self._announcement_session(link, hello.node_id)
        # An ACK for EVERY chunk, all carrying the announcement's session: the
        # node counts accepted ACKs to know its whole NDB was received.
        ack = msg.HelloAck(
            session=session,
            host_time=time.time(), accepted=True,
            policy=msg.HelloAckPolicy(telemetry_rate_limit_hz=float(protocol_spec.timing().get("telemetry_rate_limit_hz", 200))),
        )
        frame_bytes = encode_frame(msg.TYPE_HELLO_ACK, 0, link.next_seq(), ack.to_payload())
        await link.send_frame(frame_bytes)
        summary = self.node_summary(hello.node_id)
        if summary is not None:
            await self._publish("node", summary)

    def _announcement_session(self, link: Link, node_id: int) -> str:
        """Session id for a HELLO just received: the announcement's own if this
        is a follow-on chunk (same link, < HELLO_ANNOUNCE_GAP_S after the
        previous chunk), otherwise a new one fixed by this first chunk."""
        now = self._now()
        known = self._announcements.get(node_id)
        if known is not None and known[0] == link.id and now - known[1] < HELLO_ANNOUNCE_GAP_S:
            session = known[2]
        else:
            # Milliseconds, forced strictly increasing per node so two sessions
            # opened in the same millisecond still get different ids.
            ms = max(int(time.time() * 1000), self._last_session_ms.get(node_id, 0) + 1)
            self._last_session_ms[node_id] = ms
            session = f"sess-{node_id}-{ms}"
        self._announcements[node_id] = (link.id, now, session)
        return session

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
                self._note_unknown_channel(node_id, s.ch)
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

    def _note_unknown_channel(self, node_id: int, channel_id: int) -> None:
        counts = self.unknown_channel_samples.setdefault(node_id, {})
        first = channel_id not in counts
        counts[channel_id] = counts.get(channel_id, 0) + 1
        if first:  # once per (node, channel): the running count is on the node detail
            log.warning(
                "node %d: dropping samples for channel id %d, which is not in its NDB "
                "(a HELLO chunk that never arrived?); further drops are counted, not logged",
                node_id, channel_id,
            )

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
        summary["unknown_channel_samples"] = {
            str(ch): n for ch, n in sorted(self.unknown_channel_samples.get(node_id, {}).items())
        }
        summary["boot"] = hello.boot.model_dump() if hello else {"count": 0, "reason": "unknown", "uptime_ms": 0}
        return summary

    def list_node_summaries(self) -> list[dict[str, Any]]:
        return [s for r in self.store.list_nodes() if (s := self.node_summary(r["node_id"])) is not None]

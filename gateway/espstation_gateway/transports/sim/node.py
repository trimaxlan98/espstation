# SimNode: a simulated ESP32 node that speaks byte-identical ENLP through
# the real protocol codec (protocol.frames / protocol.messages) -- this is
# not a mock of the wire format, it's a real ENLP peer whose sensors are
# synthetic. The point (docs/ARCHITECTURE.md): "the desktop cannot develop
# against a fiction: contract drift is impossible by construction."
#
# One master tick drives everything (heartbeat, per-channel sampling at
# declared rates, experiment triggers, log/event chatter) rather than one
# asyncio.Task per channel -- simpler to reason about and cheap enough that
# a 20-node mesh (SimNetwork) doesn't spawn hundreds of tasks.
from __future__ import annotations

import asyncio
import math
import random
import time
from dataclasses import dataclass, field
from typing import Any, AsyncIterator, Callable

from ..base import Transport
from ...protocol import messages as msg
from ...protocol import spec as protocol_spec
from ...protocol.frames import Frame, encode as encode_frame

_MASTER_TICK_HZ = 20.0
_MAX_SAMPLES_PER_FRAME = 64
_MAX_BACKLOG = 2000  # samples buffered while link_drop fault is active


def _mac_from_node_id(node_id: int) -> str:
    b = node_id.to_bytes(2, "big")
    return f"24:6f:28:00:{b[0]:02x}:{b[1]:02x}"


@dataclass
class _ChannelRuntime:
    """Per-channel scheduling + signal-generation state."""

    ndb: msg.NdbChannel
    rate_hz: float
    enc_name: str | None = None
    scale: float | None = None
    _accum_s: float = 0.0
    stuck: bool = False
    _last_value: Any = None


class SimNode:
    """One simulated node. Owns its NDB, its synthetic sensors, its
    experiment runtime, and an outbound frame queue that SimTransport drains."""

    def __init__(self, node_id: int, label: str | None = None, *, seed: int | None = None) -> None:
        self.node_id = node_id
        self.label = label or f"sim-{node_id:04x}"
        self.mac = _mac_from_node_id(node_id)
        self._rng = random.Random(seed if seed is not None else node_id)

        self.boot_count = 1
        self.state = 0  # boot, per node_state enum
        self._boot_monotonic = time.monotonic()
        self.log_level = 3  # info

        # Synthetic sensor state.
        self._heap_total = 220_000
        self.heap_free = float(self._heap_total - 40_000)
        self.heap_min = self.heap_free
        self.rssi = -55.0
        self.vbat = 4.05
        self._temp_phase = self._rng.uniform(0, 2 * math.pi)
        self._adc_transient_ticks = 0

        self.ndb: list[msg.NdbChannel] = self._default_ndb()
        self._channels: dict[str, _ChannelRuntime] = {
            ch.key: _ChannelRuntime(ndb=ch, rate_hz=ch.rate_hz) for ch in self.ndb
        }

        self.outbox: "asyncio.Queue[bytes]" = asyncio.Queue()
        self._seq = 0
        self.session: str | None = None

        # Fault state.
        self._link_dropped = False
        self._link_was_lost_pending = False
        self._backlog: list[tuple[int, int, int, Any]] = []  # (ts_ms, ch_id, enc_code, semantic_value)
        self._brownout_since_boot = False
        self._heap_leak_rate = 0.0  # bytes/sec, 0 = off

        # Experiment runtime.
        self._spec: msg.ExperimentSpec | None = None
        self.run_id: str | None = None
        self.run_state = "idle"
        self._run_started_ms: int | None = None
        self._run_samples = 0
        self._run_seq = 0
        self._trigger_since: dict[int, float] = {}  # index into spec.triggers -> uptime_s condition became true
        self._trigger_fired: set[int] = set()  # trigger indices with once=true that already fired
        self._trigger_prev_value: dict[int, float] = {}  # for op="delta"
        self._burst_until: dict[str, float] = {}  # channel key -> uptime_s to revert burst rate
        self._burst_saved_rate: dict[str, float] = {}

        # Network mesh hook: SimNetwork installs this so net.scan / periodic
        # NET_REPORT reflect real configured peers instead of an empty list.
        self.peer_provider: Callable[[], list[dict[str, Any]]] | None = None

        self._tasks: list[asyncio.Task] = []
        self._running = False

    # -- NDB ------------------------------------------------------------

    def _default_ndb(self) -> list[msg.NdbChannel]:
        sys_defaults = {
            "sys.heap_free": dict(id=1, name="Heap free", unit="B", type="u32", rate_hz=1.0, group="system"),
            "sys.rssi": dict(id=2, name="WiFi RSSI", unit="dBm", type="i8", rate_hz=1.0, group="system"),
            "sys.uptime": dict(id=3, name="Uptime", unit="s", type="u32", rate_hz=0.2, group="system"),
            "sys.vbat": dict(id=4, name="Battery", unit="V", type="f32", rate_hz=0.2, group="system"),
            "sys.temp": dict(id=5, name="Board temp", unit="degC", type="f32", rate_hz=1.0, group="system"),
        }
        channels = [msg.NdbChannel(key=key, **fields) for key, fields in sys_defaults.items()]
        # One analog experiment channel, matching PROTOCOL.md section 4.1's
        # worked example exactly (id 16, adc.a0, 0..3.3V).
        channels.append(
            msg.NdbChannel(
                id=16, key="adc.a0", name="ADC ch0", unit="V", type="f32",
                rate_hz=50.0, group="analog", min=0.0, max=3.3,
            )
        )
        return channels

    # -- lifecycle --------------------------------------------------------

    async def start(self) -> None:
        self._running = True
        await self._enqueue_hello()
        self.state = 1  # idle, right after boot HELLO
        self._tasks = [
            asyncio.create_task(self._master_loop(), name=f"sim-{self.node_id}-tick"),
            asyncio.create_task(self._hello_retry_loop(), name=f"sim-{self.node_id}-hello"),
        ]

    async def stop(self) -> None:
        self._running = False
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            try:
                await t
            except (asyncio.CancelledError, Exception):
                pass
        self._tasks = []

    def uptime_ms(self) -> int:
        return int((time.monotonic() - self._boot_monotonic) * 1000)

    # -- outbound framing ---------------------------------------------------

    def _next_seq(self) -> int:
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFFFF
        return seq

    async def _send(self, type_code: int, payload: bytes) -> None:
        frame_bytes = encode_frame(type_code, self.node_id, self._next_seq(), payload)
        await self.outbox.put(frame_bytes)

    async def _enqueue_hello(self) -> None:
        hello = msg.Hello(
            mac=self.mac,
            node_id=self.node_id,
            label=self.label,
            chip=msg.ChipInfo(model="esp32", revision=3, cores=2, features=["wifi", "bt", "ble"]),
            fw=msg.FwInfo(version="0.1.0-sim", build="sim", idf="5.3.1", target="esp32"),
            caps=["telemetry", "experiment", "espnow", "store_forward"],
            boot=msg.BootInfo(count=self.boot_count, reason="power_on", uptime_ms=self.uptime_ms()),
            ndb=self.ndb,
        )
        await self._send(msg.TYPE_HELLO, hello.to_payload())

    async def _hello_retry_loop(self) -> None:
        retry_s = protocol_spec.timing().get("hello_retry_s", 30)
        try:
            while self._running:
                await asyncio.sleep(retry_s)
                if self.session is None:
                    await self._enqueue_hello()
        except asyncio.CancelledError:
            return

    # -- inbound (station -> node) -----------------------------------------

    async def on_station_frame(self, frame: Frame) -> None:
        try:
            decoded = msg.decode_message(frame.type, frame.payload)
        except msg.MessageDecodeError as exc:
            # EXP_SET has no CMD `id` to correlate a CMD_ACK against
            # (PROTOCOL.md is ambiguous here, see ExperimentSpec/NetCmd
            # docstrings), so a structurally-invalid spec is surfaced as an
            # EVENT rather than crashing the link pump on a malformed frame.
            if frame.type == msg.TYPE_EXP_SET:
                await self._emit_event("exp.reject", severity="error", data={"reason": str(exc)})
            else:
                await self._emit_log(1, "proto", f"failed to decode type={frame.type:#04x}: {exc}")
            return
        if frame.type == msg.TYPE_HELLO_ACK and isinstance(decoded, msg.HelloAck):
            self.session = decoded.session
        elif frame.type == msg.TYPE_CMD and isinstance(decoded, msg.Cmd):
            await self._handle_cmd(decoded)
        elif frame.type == msg.TYPE_EXP_SET and isinstance(decoded, msg.ExperimentSpec):
            await self._handle_exp_set(decoded)
        elif frame.type == msg.TYPE_TELEM_ACK and isinstance(decoded, msg.TelemAck):
            pass  # durability watermark is a station-side concern; nothing to free in the sim
        elif frame.type == msg.TYPE_TIME_SYNC and isinstance(decoded, msg.TimeSync):
            await self._handle_time_sync(decoded)
        # Unknown / experiment-defined (0x80-0xFF) frames: sim ignores them.

    async def emit_net_report(self, peers: list[dict[str, Any]], *, role: str = "peer", channel: int = 6) -> None:
        """Public hook SimNetwork calls with the mesh's computed peer rows
        for this node -- NET_REPORT (0x60) construction stays here, next to
        every other outbound message this node builds."""
        report = msg.NetReport(ts_ms=self.uptime_ms(), role=role, channel=channel, peers=[msg.NetPeer(**p) for p in peers])
        await self._send(msg.TYPE_NET_REPORT, report.to_payload())

    async def _handle_time_sync(self, ts: msg.TimeSync) -> None:
        now_ms = self.uptime_ms()
        reply = msg.TimeSync(t1_host_us=ts.t1_host_us, t2_node_ms=now_ms, t3_node_ms=self.uptime_ms(), reserved=0)
        await self._send(msg.TYPE_TIME_SYNC, reply.to_bytes())

    async def _emit_event(self, code: str, severity: str = "info", data: dict[str, Any] | None = None) -> None:
        ev = msg.Event(ts_ms=self.uptime_ms(), code=code, severity=severity, data=data or {})
        await self._send(msg.TYPE_EVENT, ev.to_payload())

    async def _emit_log(self, level: int, tag: str, text: str) -> None:
        line = msg.Log(ts_ms=self.uptime_ms(), level=level, tag=tag, msg=text)
        await self._send(msg.TYPE_LOG, line.to_bytes())

    async def _emit_exp_state(self, reason: str | None = None) -> None:
        spec = self._spec
        started = self._run_started_ms or 0
        elapsed = max(0, self.uptime_ms() - started) if self.run_state == "running" else 0
        progress = 0.0
        if spec is not None and spec.duration_ms:
            progress = min(1.0, elapsed / spec.duration_ms)
        state_msg = msg.ExpState(
            run_id=self.run_id or "",
            state=self.run_state,
            spec_hash=spec.id if spec is not None else "",
            started_at_ms=started,
            elapsed_ms=elapsed,
            progress=progress,
            samples=self._run_samples,
            buffered=len(self._backlog),
            reason=reason,
        )
        await self._send(msg.TYPE_EXP_STATE, state_msg.to_payload())

    # -- CMD handling ---------------------------------------------------

    async def _ack(self, cmd_id: int, ok: bool, data: dict[str, Any] | None = None, err: tuple[str, str] | None = None, is_async: bool = False) -> None:
        ack = msg.CmdAck(
            id=cmd_id, ok=ok, data=data,
            err=msg.CmdAckError(code=err[0], message=err[1]) if err else None,
            **{"async": is_async},
        )
        await self._send(msg.TYPE_CMD_ACK, ack.to_payload())

    async def _handle_cmd(self, cmd: msg.Cmd) -> None:
        op = cmd.op
        args = cmd.args
        try:
            if op == "node.ping":
                await self._ack(cmd.id, True, {"pong": True, "uptime_ms": self.uptime_ms()})
            elif op == "node.info":
                await self._ack(cmd.id, True, {"mac": self.mac, "node_id": self.node_id, "label": self.label})
            elif op == "node.reboot":
                await self._do_reboot()
                await self._ack(cmd.id, True, {"state": "boot"})
            elif op == "node.set_id":
                self.node_id = int(args["node_id"])
                await self._ack(cmd.id, True, {"node_id": self.node_id})
            elif op == "node.set_label":
                self.label = str(args["label"])
                await self._ack(cmd.id, True, {"label": self.label})
            elif op == "node.set_log_level":
                self.log_level = int(args["level"])
                await self._ack(cmd.id, True, {"log_level": self.log_level})
            elif op == "exp.start":
                await self._cmd_exp_start(cmd)
            elif op == "exp.stop":
                await self._cmd_exp_stop(cmd)
            elif op == "exp.state":
                await self._ack(cmd.id, True, {"run_id": self.run_id, "state": self.run_state})
                await self._emit_exp_state()
            elif op == "store.drain":
                drained = len(self._backlog)
                await self._ack(cmd.id, True, {"drained": drained})
            elif op == "store.erase":
                self._backlog.clear()
                await self._ack(cmd.id, True, {"erased": True})
            elif op == "net.scan":
                peers = self.peer_provider() if self.peer_provider else []
                await self._ack(cmd.id, True, {"peers": peers})
            else:
                await self._ack(cmd.id, False, err=("unknown_op", f"no such op {op!r}"))
        except KeyError as exc:
            await self._ack(cmd.id, False, err=("bad_args", f"missing arg {exc}"))

    async def _cmd_exp_start(self, cmd: msg.Cmd) -> None:
        if self._spec is None:
            await self._ack(cmd.id, False, err=("no_spec", "no experiment installed"))
            return
        if self.run_state == "running":
            await self._ack(cmd.id, False, err=("busy", f"run {self.run_id} active"))
            return
        await self._start_run()
        await self._ack(cmd.id, True, {"state": self.run_state, "run_id": self.run_id})

    async def _cmd_exp_stop(self, cmd: msg.Cmd) -> None:
        if self.run_state != "running":
            await self._ack(cmd.id, False, err=("not_running", "no active run"))
            return
        self.run_state = "done"
        self._apply_channel_overrides(None)
        await self._emit_exp_state(reason="stopped by operator")
        await self._emit_event("exp.stop", data={"run_id": self.run_id})
        await self._ack(cmd.id, True, {"state": self.run_state})

    async def _do_reboot(self) -> None:
        self.boot_count += 1
        self._boot_monotonic = time.monotonic()
        self.heap_free = float(self._heap_total - 40_000)
        self.heap_min = self.heap_free
        self.state = 0
        was_running_on_boot = (
            self._spec is not None and self._spec.persist and self._spec.start.mode == "on_boot"
        )
        self.run_state = "idle"
        await self._emit_event("sys.reboot", data={"boot_count": self.boot_count})
        await self._enqueue_hello()
        self.state = 1
        if was_running_on_boot:
            await self._start_run()

    # -- EXP_SET handling -------------------------------------------------

    async def _handle_exp_set(self, spec: msg.ExperimentSpec) -> None:
        errors = self._validate_spec(spec)
        if errors:
            await self._emit_event("exp.reject", severity="error", data={"reason": "; ".join(errors)})
            await self._emit_exp_state(reason="; ".join(errors))
            return
        self._spec = spec
        self.run_state = "armed"
        await self._emit_exp_state()
        if spec.start.mode == "on_boot":
            await self._start_run()
        # "manual" / "at_ms" / "on_trigger": handled by exp.start CMD or the
        # master loop's at_ms check.

    def _validate_spec(self, spec: msg.ExperimentSpec) -> list[str]:
        """Node-side validation, EXPERIMENTS.md gate 2: schema version,
        channel existence, rate sanity. Station-side gate 1 lives in
        protocol.ndb.NodeDatabase.validate_spec and runs before this."""
        errors = []
        if spec.schema_ != 1:
            errors.append(f"unsupported schema {spec.schema_}")
        for ch in spec.channels:
            if ch.key not in self._channels:
                errors.append(f"unknown channel {ch.key!r}")
            elif ch.rate_hz <= 0:
                errors.append(f"channel {ch.key!r} rate_hz must be > 0")
        return errors

    async def _start_run(self) -> None:
        self._run_seq += 1
        self.run_id = f"n{self.node_id}-{self.boot_count}-{self._run_seq}"
        self.run_state = "running"
        self._run_started_ms = self.uptime_ms()
        self._run_samples = 0
        self._trigger_since.clear()
        self._trigger_fired.clear()
        self._trigger_prev_value.clear()
        self._apply_channel_overrides(self._spec)
        await self._emit_exp_state()
        await self._emit_event("exp.start", data={"run_id": self.run_id})

    def _apply_channel_overrides(self, spec: msg.ExperimentSpec | None) -> None:
        """Install (or, if spec is None, revert) the per-channel rate/enc/
        scale overrides an EXP_SET declares (EXPERIMENTS.md `channels[]`).
        Applied only once a run actually starts -- an armed-but-not-started
        spec must not change what's already streaming."""
        for ch in self._channels.values():
            ch.rate_hz = ch.ndb.rate_hz
            ch.enc_name = None
            ch.scale = None
        if spec is None:
            return
        for ch_spec in spec.channels:
            rt = self._channels.get(ch_spec.key)
            if rt is None:
                continue
            rt.rate_hz = ch_spec.rate_hz
            rt.enc_name = ch_spec.enc
            rt.scale = ch_spec.scale

    # -- fault injection ----------------------------------------------------

    def apply_fault(self, kind: str, **kwargs: Any) -> dict[str, Any]:
        if kind == "link_drop":
            drop = bool(kwargs.get("drop", True))
            if drop and not self._link_dropped:
                self._link_dropped = True
            elif not drop and self._link_dropped:
                self._link_dropped = False
                self._link_was_lost_pending = True
            return {"link_dropped": self._link_dropped}
        if kind == "brownout":
            self._brownout_since_boot = True
            self.heap_free = max(0.0, self.heap_free - self._rng.uniform(5000, 20000))
            self.state = 4  # safe
            return {"brownout": True}
        if kind == "heap_leak":
            self._heap_leak_rate = float(kwargs.get("rate_bytes_per_s", 500.0)) if kwargs.get("enable", True) else 0.0
            return {"heap_leak_rate": self._heap_leak_rate}
        if kind == "stuck_sensor":
            key = kwargs["channel"]
            stuck = bool(kwargs.get("stuck", True))
            rt = self._channels.get(key)
            if rt is None:
                raise KeyError(f"no such channel {key!r}")
            rt.stuck = stuck
            return {"channel": key, "stuck": stuck}
        if kind == "packet_loss":
            self._packet_loss = float(kwargs.get("fraction", 0.0))
            return {"packet_loss": self._packet_loss}
        raise ValueError(f"unknown fault kind {kind!r}")

    # -- master tick: signals, sampling, experiment lifecycle ---------------

    async def _master_loop(self) -> None:
        dt = 1.0 / _MASTER_TICK_HZ
        heartbeat_accum = 0.0
        chatter_accum = 0.0
        try:
            while self._running:
                await asyncio.sleep(dt)
                self._advance_signals(dt)

                due_samples = self._collect_due_samples(dt)
                if due_samples:
                    await self._emit_samples(due_samples)

                self._evaluate_burst_expiry()
                if self.run_state == "running":
                    await self._advance_experiment()
                elif self.run_state == "armed" and self._spec is not None and self._spec.start.mode == "at_ms":
                    at_ms = self._spec.start.at_ms or 0
                    if self.uptime_ms() >= at_ms:
                        await self._start_run()

                heartbeat_accum += dt
                if heartbeat_accum >= 1.0:
                    heartbeat_accum -= 1.0
                    await self._emit_heartbeat()

                chatter_accum += dt
                if chatter_accum >= 4.0:
                    chatter_accum = 0.0
                    if self._rng.random() < 0.6:
                        await self._emit_log(3, "app", f"idle tick, heap_free={int(self.heap_free)}")
        except asyncio.CancelledError:
            return

    def _advance_signals(self, dt: float) -> None:
        # Heap: baseline jitter, plus fragmentation drift from heap_leak fault.
        self.heap_free += self._rng.uniform(-40, 40) - self._heap_leak_rate * dt
        self.heap_free = max(4096.0, min(float(self._heap_total), self.heap_free))
        self.heap_min = min(self.heap_min, self.heap_free)

        # RSSI: bounded random walk.
        self.rssi += self._rng.uniform(-1.5, 1.5)
        self.rssi = max(-95.0, min(-30.0, self.rssi))

        # Battery: slow linear drain plus noise.
        self.vbat = max(3.0, self.vbat - 1.5e-6 * dt + self._rng.uniform(-0.001, 0.001))

        # Temperature: 24h sinusoidal drift ("daily-drift") plus noise.
        t = time.time()
        self._temp_phase = (2 * math.pi * (t % 86400) / 86400.0)
        _ = self._temp_phase  # phase kept for readability of the formula below

    def _sample_channel_value(self, key: str) -> Any:
        t = time.time()
        if key == "sys.heap_free":
            return self.heap_free
        if key == "sys.rssi":
            return self.rssi
        if key == "sys.uptime":
            return self.uptime_ms() / 1000.0
        if key == "sys.vbat":
            return self.vbat
        if key == "sys.temp":
            daily = 6.0 * math.sin(2 * math.pi * (t % 86400) / 86400.0)
            return 23.0 + daily + self._rng.uniform(-0.15, 0.15)
        if key == "adc.a0":
            if self._adc_transient_ticks > 0:
                self._adc_transient_ticks -= 1
                return self._adc_transient_value
            if self._rng.random() < 0.002:  # occasional transient spike
                self._adc_transient_ticks = self._rng.randint(2, 6)
                self._adc_transient_value = self._rng.uniform(2.8, 3.3)
                return self._adc_transient_value
            return 1.65 + self._rng.uniform(-0.02, 0.02)
        return 0.0

    def _collect_due_samples(self, dt: float) -> list[tuple[int, int, Any]]:
        """Returns (channel_id, enc_code, semantic_value) for every channel
        due this tick."""
        out: list[tuple[int, int, Any]] = []
        for key, rt in self._channels.items():
            rt._accum_s += dt
            period = 1.0 / rt.rate_hz if rt.rate_hz > 0 else float("inf")
            if rt._accum_s < period:
                continue
            rt._accum_s = 0.0
            if rt.stuck and rt._last_value is not None:
                value = rt._last_value
            else:
                value = self._sample_channel_value(key)
                rt._last_value = value
            enc_name = rt.enc_name or rt.ndb.type
            enc_code = protocol_spec.encoding_by_name().get(enc_name)
            if enc_code is None:
                continue
            transport_value = value / rt.scale if rt.scale else value
            if enc_name != "f32" and enc_name != "bool":
                transport_value = int(round(transport_value))
            out.append((rt.ndb.id, enc_code, transport_value))
        return out

    async def _emit_samples(self, due: list[tuple[int, int, Any]]) -> None:
        base_ts = self.uptime_ms()
        entries = [(base_ts, ch_id, enc_code, value) for ch_id, enc_code, value in due]
        if self._link_dropped:
            self._backlog.extend(entries)
            if len(self._backlog) > _MAX_BACKLOG:
                del self._backlog[: len(self._backlog) - _MAX_BACKLOG]
            return
        replay_backlog = self._backlog
        self._backlog = []
        if replay_backlog:
            await self._send_telemetry_batches(replay_backlog, replay=True)
        gap_before = self._link_was_lost_pending
        self._link_was_lost_pending = False
        await self._send_telemetry_batches(entries, replay=False, gap_before=gap_before)
        if self.run_state == "running":
            self._run_samples += len(entries)

    async def _send_telemetry_batches(self, entries: list[tuple[int, int, int, Any]], *, replay: bool, gap_before: bool = False) -> None:
        for i in range(0, len(entries), _MAX_SAMPLES_PER_FRAME):
            chunk = entries[i:i + _MAX_SAMPLES_PER_FRAME]
            base_ts = chunk[0][0]
            samples = [
                msg.Sample(ch=ch_id, dt_ms=max(0, ts - base_ts), enc=enc_code, value=value)
                for ts, ch_id, enc_code, value in chunk
            ]
            flags = 0
            if replay:
                flags |= 1 << 0
            if gap_before:
                flags |= 1 << 1
            telemetry = msg.Telemetry(base_ts_ms=base_ts, flags=flags, samples=samples)
            await self._send(msg.TYPE_TELEMETRY, telemetry.to_bytes())

    def _evaluate_burst_expiry(self) -> None:
        now = time.monotonic()
        expired = [key for key, until in self._burst_until.items() if now >= until]
        for key in expired:
            del self._burst_until[key]
            rt = self._channels.get(key)
            if rt is not None and key in self._burst_saved_rate:
                rt.rate_hz = self._burst_saved_rate.pop(key)

    async def _emit_heartbeat(self) -> None:
        flags = 0
        if self._backlog:
            flags |= 1 << 0  # buffered_pending
        if self._link_dropped:
            flags |= 1 << 1  # link_was_lost (still down)
        if self._brownout_since_boot:
            flags |= 1 << 2
        hb = msg.Heartbeat(
            uptime_ms=self.uptime_ms(),
            heap_free=int(self.heap_free),
            heap_min=int(self.heap_min),
            state=self.state,
            flags=flags,
            rssi=int(round(self.rssi)),
        )
        await self._send(msg.TYPE_HEARTBEAT, hb.to_bytes())

    async def _advance_experiment(self) -> None:
        spec = self._spec
        if spec is None or self._run_started_ms is None:
            return
        elapsed_ms = self.uptime_ms() - self._run_started_ms
        if spec.duration_ms and elapsed_ms >= spec.duration_ms:
            self.run_state = "done"
            self._apply_channel_overrides(None)
            await self._emit_exp_state()
            await self._emit_event("exp.done", data={"run_id": self.run_id})
            return
        await self._evaluate_triggers(spec, elapsed_ms)

    async def _evaluate_triggers(self, spec: msg.ExperimentSpec, elapsed_ms: int) -> None:
        now = time.monotonic()
        for idx, trig in enumerate(spec.triggers):
            if trig.once and idx in self._trigger_fired:
                continue
            rt = self._channels.get(trig.when.channel)
            if rt is None or rt._last_value is None:
                continue
            value = rt._last_value
            condition = self._evaluate_condition(idx, value, trig.when.op, trig.when.value)
            if not condition:
                self._trigger_since.pop(idx, None)
                continue
            since = self._trigger_since.setdefault(idx, now)
            if (now - since) * 1000.0 < trig.when.for_ms:
                continue
            # Debounced condition satisfied: fire once, then require the
            # condition to go false before it can fire again (unless
            # `once`, which never re-arms).
            del self._trigger_since[idx]
            self._trigger_fired.add(idx)
            await self._fire_trigger(idx, trig, value)

    def _evaluate_condition(self, idx: int, value: float, op: str, threshold: float) -> bool:
        if op == "delta":
            # experiment.schema.json's "delta" op: fires on a jump larger
            # than `value` since the last sample, either direction.
            prev = self._trigger_prev_value.get(idx)
            self._trigger_prev_value[idx] = value
            return prev is not None and abs(value - prev) > threshold
        ops: dict[str, Callable[[float, float], bool]] = {
            ">": lambda a, b: a > b, ">=": lambda a, b: a >= b,
            "<": lambda a, b: a < b, "<=": lambda a, b: a <= b,
            "==": lambda a, b: a == b, "!=": lambda a, b: a != b,
        }
        fn = ops.get(op)
        return bool(fn(value, threshold)) if fn else False

    async def _fire_trigger(self, idx: int, trig: msg.ExperimentTrigger, value: float) -> None:
        code = trig.emit or f"exp.trigger.{trig.id or idx}"
        await self._emit_event(
            code, severity="warning",
            data={"run_id": self.run_id, "channel": trig.when.channel, "value": value, "threshold": trig.when.value},
        )
        for action in trig.do:
            kind = action.action
            if kind == "set_state":
                name_to_code = {v: k for k, v in protocol_spec.enums().get("node_state", {}).items()}
                code_ = name_to_code.get(action.state)
                if code_ is not None:
                    self.state = code_
            elif kind == "set_rate":
                rt = self._channels.get(action.channel or "")
                if rt is not None and action.rate_hz is not None:
                    rt.rate_hz = action.rate_hz
            elif kind == "burst":
                key = action.channel or ""
                rt = self._channels.get(key)
                if rt is not None and action.rate_hz is not None:
                    if key not in self._burst_saved_rate:
                        self._burst_saved_rate[key] = rt.rate_hz
                    rt.rate_hz = action.rate_hz
                    self._burst_until[key] = time.monotonic() + (action.ms or 1000) / 1000.0
            elif kind == "stop":
                self.run_state = "done"
                self._apply_channel_overrides(None)
                await self._emit_exp_state(reason="trigger stop")
            elif kind == "mark":
                await self._emit_event("exp.mark", data={"run_id": self.run_id, "label": action.label or ""})
            elif kind == "reboot":
                await self._do_reboot()
            elif kind == "set_gpio":
                await self._emit_log(3, "gpio", f"gpio={action.gpio} level={action.level}")


class SimTransport(Transport):
    """Transport wrapper: makes a SimNode addressable through the same
    Transport interface real hardware uses (so Link/app.py code paths are
    identical for sim and serial links). Framing is "raw" -- one queue item
    is exactly one frame body -- matching ESP-NOW's raw framing, which is
    the closest real transport to an in-process channel."""

    def __init__(self, node: SimNode) -> None:
        self.node = node
        self._own_lifecycle = False

    async def open(self) -> None:
        if not self.node._running:
            await self.node.start()
            self._own_lifecycle = True

    async def close(self) -> None:
        if self._own_lifecycle:
            await self.node.stop()

    async def send(self, frame_bytes: bytes) -> None:
        from ...protocol.frames import parse
        frame = parse(frame_bytes)
        await self.node.on_station_frame(frame)

    async def receive(self) -> AsyncIterator[bytes]:
        while True:
            chunk = await self.node.outbox.get()
            yield chunk

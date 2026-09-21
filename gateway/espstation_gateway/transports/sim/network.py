# SimNetwork: an ESP-NOW-like mesh of SimNode peers. Link quality (loss,
# latency) is configured per undirected node pair and periodically
# summarized into a real NET_REPORT (0x60) from each node's point of view --
# this is what makes /api/network/topology real data instead of a fixture,
# per docs/ARCHITECTURE.md ("a twenty-node mesh experiment can be built, run
# and demoed with zero hardware").
from __future__ import annotations

import asyncio
import heapq
import math
import random
from dataclasses import asdict, dataclass, fields
from typing import Any

from . import dio_link
from .node import DioConfig, SimNode
from ...protocol import messages as msg


@dataclass
class LinkQuality:
    loss: float = 0.02  # fraction of packets dropped, 0..1
    latency_ms: float = 5.0
    jitter_ms: float = 2.0


@dataclass
class WireConfig:
    """Electrical behaviour of one virtual cable (one direction).

    `glitch_rate` is spurious pulses per second on an otherwise idle line;
    `bit_error_rate` is the per-bit flip probability applied to test frames;
    `loss` is the probability that an edge, a handshake leg or a whole frame
    never arrives. Glitches only disturb the level (dio.rx): frame corruption
    is what `bit_error_rate` is for.
    """

    delay_us: float = 40.0
    loss: float = 0.0
    glitch_rate: float = 0.0
    bit_error_rate: float = 0.0
    jitter_us: float = 4.0
    glitch_width_us: float = 3.0

    def validate(self) -> None:
        """Raise ValueError (never TypeError) unless every field is a finite
        number in range. This is reachable from the REST fault endpoint, so
        null, lists, strings, booleans, NaN and inf (which is what 1e400
        parses to) all have to come out as a clear rejection: a NaN or inf
        delay would otherwise install a cable that silently carries nothing."""
        for f in fields(self):
            value = getattr(self, f.name)
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise ValueError(f"wire {f.name} must be a number, got {value!r}")
            try:
                finite = math.isfinite(value)
            except OverflowError:  # an integer literal too big for a float
                finite = False
            if not finite:
                raise ValueError(f"wire {f.name} must be finite, got {value!r}")
        for name in ("loss", "bit_error_rate"):
            if not 0.0 <= getattr(self, name) <= 1.0:
                raise ValueError(f"wire {name} must be in 0..1, got {getattr(self, name)}")
        for name in ("delay_us", "jitter_us", "glitch_rate", "glitch_width_us"):
            if getattr(self, name) < 0.0:
                raise ValueError(f"wire {name} must be >= 0, got {getattr(self, name)}")
        for f in fields(self):  # ints are fine on input; the model works in floats
            setattr(self, f.name, float(getattr(self, f.name)))


_EV_SET, _EV_GLITCH_ON, _EV_GLITCH_OFF = range(3)


class VirtualWire:
    """One direction of the two-wire cable: `tx` node's dio.tx -> `rx` node's
    dio.rx. It carries three things, all subject to the same impairments:

    - levels: edges scheduled at `delay + jitter`, lost with probability
      `loss` (the receiver then keeps the stale level until the next edge),
      plus spurious glitch pulses;
    - handshake legs: one-way transit time, or None if the leg is lost;
    - test frames: the bit stream of a frame, flipped bit by bit at
      `bit_error_rate`, or dropped whole with probability `loss`.

    Time is supplied by the callers (nodes) so the wire has no clock and a
    seeded rng: same seed, same run.
    """

    def __init__(self, tx_id: int, rx_id: int, config: WireConfig, rng: random.Random) -> None:
        self.tx_id = tx_id
        self.rx_id = rx_id
        self.config = config
        self._rng = rng
        self._heap: list[tuple[float, int, int, int]] = []  # (t_us, tiebreak, kind, value)
        self._tiebreak = 0
        self._last_sent = 0
        self._real = 0
        self._glitching = 0
        self._visible = 0
        self._last_arrival_us = 0.0
        self._last_poll_us: float | None = None
        self._frames: list[tuple[float, list[int]]] = []
        self.counters = {
            "edges_sent": 0, "edges_lost": 0, "glitches": 0,
            "frames_sent": 0, "frames_dropped": 0, "bits_flipped": 0,
            "legs_sent": 0, "legs_lost": 0,
        }

    @property
    def level(self) -> int:
        return self._visible

    def reset_level(self, level: int) -> None:
        """Set the line level without producing an edge (used when the cable
        is plugged in while the tx pin is already at `level`)."""
        self._last_sent = self._real = self._visible = level
        self._heap.clear()
        self._glitching = 0

    def _push(self, t_us: float, kind: int, value: int = 0) -> None:
        self._tiebreak += 1
        heapq.heappush(self._heap, (t_us, self._tiebreak, kind, value))

    def _one_way_us(self) -> float:
        cfg = self.config
        return max(0.0, cfg.delay_us + self._rng.uniform(-cfg.jitter_us, cfg.jitter_us))

    def send_level(self, level: int, t_us: float) -> None:
        if level == self._last_sent:
            return
        self._last_sent = level
        self.counters["edges_sent"] += 1
        if self._rng.random() < self.config.loss:
            self.counters["edges_lost"] += 1
            return
        # Edges never overtake each other, however much jitter is configured.
        arrival = max(t_us + self._one_way_us(), self._last_arrival_us)
        self._last_arrival_us = arrival
        self._push(arrival, _EV_SET, level)

    def poll(self, now_us: float) -> list[tuple[float, int]]:
        """Level changes visible at the receiver up to `now_us`, as
        (time_us, new_level). Also injects the glitches that fell in the
        interval since the previous poll."""
        self._inject_glitches(now_us)
        edges: list[tuple[float, int]] = []
        while self._heap and self._heap[0][0] <= now_us:
            t_us, _, kind, value = heapq.heappop(self._heap)
            if kind == _EV_SET:
                self._real = value
            elif kind == _EV_GLITCH_ON:
                self._glitching += 1
            else:
                self._glitching -= 1
            visible = self._real ^ (1 if self._glitching > 0 else 0)
            if visible != self._visible:
                self._visible = visible
                edges.append((t_us, visible))
        self._last_poll_us = now_us
        return edges

    def _inject_glitches(self, now_us: float) -> None:
        cfg = self.config
        last = self._last_poll_us
        if last is None or cfg.glitch_rate <= 0.0 or now_us <= last:
            return
        expected = cfg.glitch_rate * (now_us - last) / 1e6
        n = int(expected) + (1 if self._rng.random() < expected - int(expected) else 0)
        for _ in range(n):
            t = self._rng.uniform(last, now_us)
            self._push(t, _EV_GLITCH_ON)
            self._push(t + cfg.glitch_width_us, _EV_GLITCH_OFF)
            self.counters["glitches"] += 1

    def transit_us(self) -> float | None:
        """One handshake leg: transit time in us, or None if it was lost."""
        self.counters["legs_sent"] += 1
        if self._rng.random() < self.config.loss:
            self.counters["legs_lost"] += 1
            return None
        return self._one_way_us()

    def carry_frame(self, bits: list[int], t_us: float) -> None:
        self.counters["frames_sent"] += 1
        if self._rng.random() < self.config.loss:
            self.counters["frames_dropped"] += 1
            return
        ber = self.config.bit_error_rate
        if ber > 0.0:
            out = []
            for b in bits:
                if self._rng.random() < ber:
                    b ^= 1
                    self.counters["bits_flipped"] += 1
                out.append(b)
            bits = out
        arrival = max(t_us + self._one_way_us(), self._last_arrival_us)
        self._last_arrival_us = arrival
        self._frames.append((arrival, bits))

    def pop_frames(self, now_us: float) -> list[list[int]]:
        due = [bits for t, bits in self._frames if t <= now_us]
        self._frames = [(t, bits) for t, bits in self._frames if t > now_us]
        return due

    def describe(self) -> dict[str, Any]:
        return {"tx": self.tx_id, "rx": self.rx_id, **asdict(self.config), **self.counters}


def _node_id(value: Any, name: str) -> int:
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, float) and value.is_integer():  # 1004.0 from a JSON client; inf/nan are not integers
        return int(value)
    raise ValueError(f"{name} must be a node id (integer), got {value!r}")


class SimNetwork:
    """Owns a set of SimNodes plus the pairwise link-quality model between
    them. Each node's `net.scan` CMD and periodic NET_REPORT reflect this
    model rather than an empty peer list, so the topology graph and
    loss/latency matrix are backed by real (simulated) numbers."""

    def __init__(self, *, seed: int | None = None, report_interval_s: float = 5.0) -> None:
        self.nodes: dict[int, SimNode] = {}
        self._links: dict[frozenset[int], LinkQuality] = {}
        self._counters: dict[frozenset[int], dict[str, int]] = {}
        self._wires: dict[tuple[int, int], VirtualWire] = {}
        self._rng = random.Random(seed)
        self._report_interval_s = report_interval_s
        self._task: asyncio.Task | None = None
        self._running = False

    def spawn(
        self, count: int, *, start_id: int | None = None, label_prefix: str = "sim",
        dio: DioConfig | bool = False,
    ) -> list[SimNode]:
        spawned: list[SimNode] = []
        next_id = start_id if start_id is not None else (max(self.nodes) + 1 if self.nodes else 1001)
        for i in range(count):
            node_id = next_id + i
            node = SimNode(node_id, label=f"{label_prefix}-{node_id}", seed=self._rng.randint(0, 1_000_000), dio=dio)
            node.peer_provider = self._make_peer_provider(node_id)
            self.nodes[node_id] = node
            for other_id in list(self.nodes):
                if other_id == node_id:
                    continue
                key = frozenset((node_id, other_id))
                self._links.setdefault(key, LinkQuality())
                self._counters.setdefault(key, {"tx": 0, "rx": 0, "lost": 0})
            spawned.append(node)
        return spawned

    async def start_node(self, node: SimNode) -> None:
        """Start one node without touching the others -- used when a node
        is spawned into an already-running network (POST /api/sim/spawn)."""
        await node.start()

    def set_link_quality(
        self, a: int, b: int, *, loss: float | None = None, latency_ms: float | None = None, jitter_ms: float | None = None
    ) -> LinkQuality:
        key = frozenset((a, b))
        q = self._links.setdefault(key, LinkQuality())
        if loss is not None:
            q.loss = max(0.0, min(1.0, loss))
        if latency_ms is not None:
            q.latency_ms = max(0.0, latency_ms)
        if jitter_ms is not None:
            q.jitter_ms = max(0.0, jitter_ms)
        return q

    # -- virtual cable (SPEC-LINK) -------------------------------------------

    def connect_wire(
        self, tx_node_id: int, rx_node_id: int, *,
        delay_us: float = 40.0, loss: float = 0.0, glitch_rate: float = 0.0,
        bit_error_rate: float = 0.0, jitter_us: float = 4.0,
    ) -> WireConfig:
        """Wire `tx` node's dio.tx to `rx` node's dio.rx. One direction only:
        call it twice (swapped) for a bidirectional cable, which is also how
        the real A.26->B.25 / B.26->A.25 cabling works. Both nodes must have
        dio enabled, and a pin can only carry one wire."""
        tx, rx = self._wire_nodes(tx_node_id, rx_node_id)
        if (tx_node_id, rx_node_id) in self._wires:
            raise ValueError(f"wire {tx_node_id}->{rx_node_id} already exists; use set_wire()")
        if tx._dio.out_wire is not None:
            raise ValueError(f"node {tx_node_id} dio.tx is already wired")
        if rx._dio.in_wire is not None:
            raise ValueError(f"node {rx_node_id} dio.rx is already wired")
        config = WireConfig(delay_us=delay_us, loss=loss, glitch_rate=glitch_rate,
                            bit_error_rate=bit_error_rate, jitter_us=jitter_us)
        config.validate()
        wire = VirtualWire(tx_node_id, rx_node_id, config, random.Random(self._rng.randint(0, 2**31 - 1)))
        self._wires[(tx_node_id, rx_node_id)] = wire
        tx.dio_attach_out(wire)
        rx.dio_attach_in(wire)
        return config

    def set_wire(self, tx_node_id: int, rx_node_id: int, **changes: float) -> WireConfig:
        """Reconfigure a live wire. Unknown fields and out-of-range values
        raise; on failure the wire is left untouched."""
        wire = self._wires.get((tx_node_id, rx_node_id))
        if wire is None:
            raise KeyError(f"no wire {tx_node_id}->{rx_node_id}")
        valid = set(asdict(wire.config))
        unknown = set(changes) - valid
        if unknown:
            raise ValueError(f"unknown wire parameter(s): {sorted(unknown)}")
        candidate = WireConfig(**{**asdict(wire.config), **changes})
        candidate.validate()  # raises before anything is installed
        wire.config = candidate
        return candidate

    def wire(self, tx_node_id: int, rx_node_id: int) -> VirtualWire:
        return self._wires[(tx_node_id, rx_node_id)]

    def _wire_nodes(self, tx_id: int, rx_id: int) -> tuple[SimNode, SimNode]:
        if tx_id == rx_id:
            raise ValueError("a wire needs two different nodes")
        try:
            tx, rx = self.nodes[tx_id], self.nodes[rx_id]
        except KeyError as exc:
            raise KeyError(f"no such sim node {exc.args[0]}") from None
        for node in (tx, rx):
            if not node.dio_enabled:
                raise ValueError(f"node {node.node_id} has no dio link (spawn with dio=True / enable_dio())")
        return tx, rx

    def _make_peer_provider(self, node_id: int):
        def provider() -> list[dict[str, Any]]:
            return self._peer_rows(node_id)
        return provider

    def _peer_rows(self, node_id: int) -> list[dict[str, Any]]:
        rows: list[dict[str, Any]] = []
        for other_id, other in self.nodes.items():
            if other_id == node_id:
                continue
            key = frozenset((node_id, other_id))
            q = self._links.get(key, LinkQuality())
            counters = self._counters.setdefault(key, {"tx": 0, "rx": 0, "lost": 0})
            counters["tx"] += 1
            if self._rng.random() < q.loss:
                counters["lost"] += 1
            else:
                counters["rx"] += 1
            rtt = max(0.1, q.latency_ms * 2 + self._rng.uniform(-q.jitter_ms, q.jitter_ms))
            rows.append({
                "mac": other.mac,
                "node_id": other_id,
                "rssi": int(round(other.rssi)),
                "tx": counters["tx"],
                "rx": counters["rx"],
                "lost": counters["lost"],
                "rtt_ms": round(rtt, 2),
                "last_seen_ms": other.uptime_ms(),
            })
        return rows

    async def start(self) -> None:
        """Start every currently-spawned node plus the report loop. Used
        when a whole network is assembled up front (tests, `--sim` preload
        via a direct SimNetwork). Incremental spawns after that (POST
        /api/sim/spawn) start their own node via the Link/SimTransport they
        get wrapped in, then just call ensure_report_loop()."""
        self._running = True
        for node in self.nodes.values():
            await node.start()
        self.ensure_report_loop()

    def ensure_report_loop(self) -> None:
        if self._task is None or self._task.done():
            self._running = True
            self._task = asyncio.create_task(self._report_loop(), name="sim-network-reports")

    async def stop(self) -> None:
        self._running = False
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
            self._task = None
        for node in self.nodes.values():
            await node.stop()

    async def _report_loop(self) -> None:
        try:
            while self._running:
                await asyncio.sleep(self._report_interval_s)
                for node_id, node in self.nodes.items():
                    await node.emit_net_report(self._peer_rows(node_id))
        except asyncio.CancelledError:
            return

    def topology(self) -> dict[str, Any]:
        nodes = [{"node_id": nid, "label": n.label, "mac": n.mac} for nid, n in self.nodes.items()]
        edges = []
        for key, q in self._links.items():
            a, b = tuple(key) if len(key) == 2 else (next(iter(key)), next(iter(key)))
            counters = self._counters.get(key, {"tx": 0, "rx": 0, "lost": 0})
            edges.append({
                "a": a, "b": b, "loss": q.loss, "latency_ms": q.latency_ms,
                "tx": counters["tx"], "rx": counters["rx"], "lost": counters["lost"],
            })
        # `wires` is additive: the desktop's NetworkTopology ignores it.
        return {"nodes": nodes, "edges": edges, "wires": [w.describe() for w in self._wires.values()]}

    def apply_fault(self, kind: str, *, node_id: int | None = None, **kwargs: Any) -> dict[str, Any]:
        """Dispatch a fault: link-level faults (packet_loss between two
        named nodes) are handled here; everything else (brownout,
        heap_leak, stuck_sensor, a per-node link_drop) is forwarded to that
        SimNode's own apply_fault."""
        if kind == "packet_loss" and "a" in kwargs and "b" in kwargs:
            q = self.set_link_quality(int(kwargs["a"]), int(kwargs["b"]), loss=float(kwargs.get("fraction", 0.1)))
            return {"a": kwargs["a"], "b": kwargs["b"], "loss": q.loss}
        if kind == "wire":
            # kind=wire, tx=<id>, rx=<id>, plus any WireConfig field.
            params = dict(kwargs)
            try:
                tx, rx = params.pop("tx"), params.pop("rx")
            except KeyError as exc:
                raise ValueError(f"kind=wire needs {exc.args[0]!r}") from None
            return asdict(self.set_wire(_node_id(tx, "tx"), _node_id(rx, "rx"), **params))
        if node_id is not None:
            node = self.nodes.get(node_id)
            if node is None:
                raise KeyError(f"no such sim node {node_id}")
            return node.apply_fault(kind, **kwargs)
        raise ValueError("fault requires node_id (node-level), or a+b (link-level packet_loss)")

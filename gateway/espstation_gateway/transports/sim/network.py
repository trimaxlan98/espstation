# SimNetwork: an ESP-NOW-like mesh of SimNode peers. Link quality (loss,
# latency) is configured per undirected node pair and periodically
# summarized into a real NET_REPORT (0x60) from each node's point of view --
# this is what makes /api/network/topology real data instead of a fixture,
# per docs/ARCHITECTURE.md ("a twenty-node mesh experiment can be built, run
# and demoed with zero hardware").
from __future__ import annotations

import asyncio
import random
from dataclasses import dataclass
from typing import Any

from .node import SimNode
from ...protocol import messages as msg


@dataclass
class LinkQuality:
    loss: float = 0.02  # fraction of packets dropped, 0..1
    latency_ms: float = 5.0
    jitter_ms: float = 2.0


class SimNetwork:
    """Owns a set of SimNodes plus the pairwise link-quality model between
    them. Each node's `net.scan` CMD and periodic NET_REPORT reflect this
    model rather than an empty peer list, so the topology graph and
    loss/latency matrix are backed by real (simulated) numbers."""

    def __init__(self, *, seed: int | None = None, report_interval_s: float = 5.0) -> None:
        self.nodes: dict[int, SimNode] = {}
        self._links: dict[frozenset[int], LinkQuality] = {}
        self._counters: dict[frozenset[int], dict[str, int]] = {}
        self._rng = random.Random(seed)
        self._report_interval_s = report_interval_s
        self._task: asyncio.Task | None = None
        self._running = False

    def spawn(self, count: int, *, start_id: int | None = None, label_prefix: str = "sim") -> list[SimNode]:
        spawned: list[SimNode] = []
        next_id = start_id if start_id is not None else (max(self.nodes) + 1 if self.nodes else 1001)
        for i in range(count):
            node_id = next_id + i
            node = SimNode(node_id, label=f"{label_prefix}-{node_id}", seed=self._rng.randint(0, 1_000_000))
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
        return {"nodes": nodes, "edges": edges}

    def apply_fault(self, kind: str, *, node_id: int | None = None, **kwargs: Any) -> dict[str, Any]:
        """Dispatch a fault: link-level faults (packet_loss between two
        named nodes) are handled here; everything else (brownout,
        heap_leak, stuck_sensor, a per-node link_drop) is forwarded to that
        SimNode's own apply_fault."""
        if kind == "packet_loss" and "a" in kwargs and "b" in kwargs:
            q = self.set_link_quality(int(kwargs["a"]), int(kwargs["b"]), loss=float(kwargs.get("fraction", 0.1)))
            return {"a": kwargs["a"], "b": kwargs["b"], "loss": q.loss}
        if node_id is not None:
            node = self.nodes.get(node_id)
            if node is None:
                raise KeyError(f"no such sim node {node_id}")
            return node.apply_fault(kind, **kwargs)
        raise ValueError("fault requires node_id (node-level), or a+b (link-level packet_loss)")

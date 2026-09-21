# Virtual cable + dio channels of the simulated node. Everything here drives
# time by hand (a fake clock and SimNode.dio_step), so runs are deterministic
# and take milliseconds; only the final gateway-level test uses real ticks.
from __future__ import annotations

import asyncio
import json
from typing import Any

import httpx
import pytest

from espstation_gateway.app import DEFAULT_TOKEN, Settings, create_app
from espstation_gateway.protocol import frames
from espstation_gateway.protocol import messages as msg
from espstation_gateway.runtime import GatewayRuntime
from espstation_gateway.store import Store
from espstation_gateway.transports.sim import SimNetwork, SimNode
from espstation_gateway.transports.sim import dio_link as dl
from espstation_gateway.transports.sim.node import DioConfig

DT = 0.05  # the master tick
DIO_KEYS = ["dio.tx", "dio.rx", "link.rtt_us", "link.frames_ok", "link.frames_err", "link.ber"]


class FakeClock:
    def __init__(self) -> None:
        self.us = 0

    def __call__(self) -> int:
        return self.us


class Bench:
    """Two dio nodes A->B and B->A on a fake clock."""

    def __init__(self, *, seed: int = 7, cfg: dict[str, Any] | None = None, **wire: Any) -> None:
        self.clock = FakeClock()
        self.net = SimNetwork(seed=seed)
        self.a, self.b = self.net.spawn(2, dio=DioConfig(clock_us=self.clock, **(cfg or {})))
        self.net.connect_wire(self.a.node_id, self.b.node_id, **wire)
        self.net.connect_wire(self.b.node_id, self.a.node_id, **wire)

    async def run(self, seconds: float) -> None:
        for _ in range(round(seconds / DT)):
            self.clock.us += round(DT * 1_000_000)
            await self.a.dio_step(DT)
            await self.b.dio_step(DT)


def drain(node: SimNode) -> list[tuple[int, Any]]:
    out = []
    while not node.outbox.empty():
        frame = frames.parse(node.outbox.get_nowait())
        out.append((frame.type, msg.decode_message(frame.type, frame.payload)))
    return out


def events(items: list[tuple[int, Any]], code: str) -> list[msg.Event]:
    return [d for t, d in items if t == msg.TYPE_EVENT and d.code == code]


def samples(items: list[tuple[int, Any]], channel_id: int) -> list[Any]:
    return [s.value for t, d in items if t == msg.TYPE_TELEMETRY for s in d.samples if s.ch == channel_id]


# -- opt-in: nothing changes for nodes that do not ask -----------------------------

def test_default_ndb_is_untouched_without_dio() -> None:
    node = SimNode(1001)
    assert not node.dio_enabled
    assert [(c.id, c.key) for c in node.ndb] == [
        (1, "sys.heap_free"), (2, "sys.rssi"), (3, "sys.uptime"), (4, "sys.vbat"), (5, "sys.temp"),
        (16, "adc.a0"),
    ]
    with pytest.raises(ValueError):  # the link getter is dio-only
        node.dio_stats()


def test_spawn_is_dio_free_by_default_and_topology_has_empty_wires() -> None:
    net = SimNetwork(seed=1)
    nodes = net.spawn(3)
    assert all(not n.dio_enabled and "adc.a0" in {c.key for c in n.ndb} for n in nodes)
    topo = net.topology()
    assert topo["wires"] == []
    assert {"nodes", "edges"} <= topo.keys()


def test_enable_dio_publishes_the_six_spec_channels_and_drops_adc() -> None:
    node = SimNode(1001, dio=True)
    by_key = {c.key: c for c in node.ndb}
    assert "adc.a0" not in by_key
    assert [(by_key[k].id, by_key[k].type, by_key[k].group) for k in DIO_KEYS] == [
        (16, "u8", "digital"), (17, "u8", "digital"), (18, "u32", "link"),
        (19, "u32", "link"), (20, "u32", "link"), (21, "f32", "link"),
    ]
    assert by_key["link.rtt_us"].unit == "us"
    assert len({c.id for c in node.ndb}) == len(node.ndb)  # no id collisions
    assert set(DIO_KEYS) <= set(node._channels)


def hello_keys(items: list[tuple[int, Any]]) -> set[str]:
    return {c.key for t, d in items if t == msg.TYPE_HELLO for c in d.ndb}


def test_default_node_still_announces_its_ndb_in_a_single_hello() -> None:
    assert len(SimNode(1001)._hello_payloads()) == 1


def test_a_dio_ndb_does_not_fit_one_hello_so_it_is_announced_in_chunks() -> None:
    # 5 sys + 6 dio channels are ~1450 B of HELLO: over the 1024 B frame limit.
    node = SimNode(1001, dio=True)
    payloads = node._hello_payloads()
    assert len(payloads) >= 2 and all(len(p) <= frames.MAX_PAYLOAD for p in payloads)
    hellos = [msg.Hello.from_payload(p) for p in payloads]
    assert all(h.node_id == 1001 and h.mac == node.mac for h in hellos)  # each one is a complete HELLO
    assert [c.key for h in hellos for c in h.ndb] == [c.key for c in node.ndb]  # nothing lost, nothing repeated


async def test_enable_dio_after_start_is_refused_not_left_half_done() -> None:
    node = SimNode(1001)
    await node.start()
    try:
        before = [c.key for c in node.ndb]
        with pytest.raises(RuntimeError, match="before start"):
            node.enable_dio()
        assert not node.dio_enabled
        assert [c.key for c in node.ndb] == before and "adc.a0" in before  # nothing was half-applied
    finally:
        await node.stop()


async def test_enable_dio_after_a_hello_was_enqueued_is_refused_even_if_not_running() -> None:
    node = SimNode(1001)
    await node._enqueue_hello()  # announced, though start() never ran
    with pytest.raises(RuntimeError, match="before start"):
        node.enable_dio()


async def test_the_supported_dio_paths_do_not_trip_the_guard() -> None:
    net = SimNetwork(seed=1)
    (spawned,) = net.spawn(1, dio=True)  # what --sim-dio / attach_sim_dio_pair use
    assert spawned.dio_enabled
    node = SimNode(2001)
    node.enable_dio()  # before start(): fine
    await node.start()
    try:
        node.enable_dio(blink_hz=2.0)  # already dio: only retunes the config, NDB untouched
        assert node._dio.config.blink_hz == 2.0
    finally:
        await node.stop()


async def test_started_dio_node_sends_every_hello_chunk_at_boot() -> None:
    node = SimNode(1001, dio=True)
    await node.start()
    try:
        items = drain(node)
        assert len([1 for t, _ in items if t == msg.TYPE_HELLO]) >= 2
        assert hello_keys(items) == {c.key for c in node.ndb}
    finally:
        await node.stop()


def test_wire_needs_dio_nodes_and_free_pins() -> None:
    net = SimNetwork(seed=1)
    plain, a, b, c = net.spawn(1)[0], *net.spawn(3, dio=True)
    with pytest.raises(ValueError, match="no dio"):
        net.connect_wire(plain.node_id, a.node_id)
    with pytest.raises(KeyError):
        net.connect_wire(a.node_id, 9999)
    with pytest.raises(ValueError):
        net.connect_wire(a.node_id, a.node_id)
    net.connect_wire(a.node_id, b.node_id)
    with pytest.raises(ValueError, match="already exists"):
        net.connect_wire(a.node_id, b.node_id)
    with pytest.raises(ValueError, match="dio.tx is already wired"):
        net.connect_wire(a.node_id, c.node_id)
    with pytest.raises(ValueError, match="dio.rx is already wired"):
        net.connect_wire(c.node_id, b.node_id)
    with pytest.raises(ValueError):
        net.connect_wire(c.node_id, a.node_id, loss=1.5)
    assert len(net.topology()["wires"]) == 1  # the failed attempts left nothing behind


# -- level ---------------------------------------------------------------------------

async def test_set_gpio_26_reaches_the_other_nodes_dio_rx_after_the_delay() -> None:
    bench = Bench(cfg={"manual": True}, delay_us=200_000, jitter_us=0)  # 0.2 s: visible at tick resolution
    assert await bench.a.set_gpio(26, 1)
    assert bench.a.gpio_out[26] == 1 and bench.a.dio_stats()["tx"] == 1
    await bench.run(0.1)
    assert bench.b.dio_stats()["rx"] == 0  # still in flight
    await bench.run(0.25)
    assert bench.b.dio_stats()["rx"] == 1
    assert bench.a.dio_stats()["rx"] == 0  # the return wire carries B's tx, not A's

    assert await bench.a.set_gpio(26, 0)
    await bench.run(0.35)
    assert bench.b.dio_stats()["rx"] == 0

    sent = drain(bench.a)
    assert [(e.data["gpio"], e.data["level"]) for e in events(sent, "dio.gpio")] == [(26, 1), (26, 0)]
    edges = events(drain(bench.b), "dio.edge")
    assert edges and edges[0].data == {"level": 1, "count": 1}


async def test_dio_channels_report_the_levels() -> None:
    bench = Bench(cfg={"manual": True}, delay_us=100, jitter_us=0)
    await bench.a.set_gpio(26, 1)
    await bench.run(0.2)
    by_id = {ch: (val) for ch, _enc, val in bench.b._collect_due_samples(1.0)}
    assert by_id[17] == 1  # dio.rx
    assert by_id[16] == 0  # dio.tx of B untouched
    assert 18 not in by_id  # link.rtt_us is event-driven: no fake zero when idle
    assert {19, 20, 21} <= by_id.keys()


async def test_other_allowed_gpio_is_stored_not_wired() -> None:
    bench = Bench(delay_us=100, jitter_us=0)
    assert await bench.a.set_gpio(4, 1)
    await bench.run(0.2)
    assert bench.a.gpio_out == {4: 1}
    assert bench.a.dio_stats()["tx"] == 0 and bench.b.dio_stats()["rx"] == 0
    evs = events(drain(bench.a), "dio.gpio")
    assert [e.data for e in evs] == [{"gpio": 4, "level": 1}]

    # readable through node.info
    cmd = msg.Cmd(id=1, op="node.info")
    await bench.a.on_station_frame(frames.parse(frames.encode(msg.TYPE_CMD, bench.a.node_id, 0, cmd.to_payload())))
    (ack,) = [d for t, d in drain(bench.a) if t == msg.TYPE_CMD_ACK]
    assert ack.ok and ack.data["gpio_out"] == {"4": 1}


@pytest.mark.parametrize("gpio", [12, 8, 34, 99])
async def test_forbidden_gpio_is_rejected_loudly_and_changes_nothing(gpio: int) -> None:
    bench = Bench(delay_us=100, jitter_us=0)
    drain(bench.a)
    assert not await bench.a.set_gpio(gpio, 1)
    await bench.run(0.2)
    assert bench.a.gpio_out == {}
    assert bench.a.dio_stats()["tx"] == 0 and bench.b.dio_stats()["rx"] == 0
    items = drain(bench.a)
    (rejected,) = events(items, "dio.gpio_rejected")
    assert rejected.severity == "warning"
    assert rejected.data == {"gpio": gpio, "level": 1, "reason": "pin_not_allowed"}
    assert not events(items, "dio.gpio")


async def test_gpio12_rejection_says_why() -> None:
    node = SimNode(1001)
    await node.set_gpio(12, 1)
    (rejected,) = events(drain(node), "dio.gpio_rejected")
    assert rejected.data["reason"] == "pin_not_allowed"  # the firmware's string, checked before anything else


async def test_set_gpio_with_a_bad_level_is_rejected_too() -> None:
    node = SimNode(1001)
    assert not await node.set_gpio(4, 2)
    assert not await node.set_gpio(4, None)
    assert not await node.set_gpio(None, 1)
    assert [e.data for e in events(drain(node), "dio.gpio_rejected")] == [
        {"gpio": 4, "level": 2, "reason": "bad_level"},
        {"gpio": 4, "level": -1, "reason": "bad_level"},
        {"gpio": -1, "level": 1, "reason": "pin_not_allowed"},
    ]
    assert node.gpio_out == {}


async def test_experiment_trigger_set_gpio_goes_through_the_same_validation() -> None:
    node = SimNode(1001, dio=True)
    when = msg.ExperimentTriggerWhen(channel="sys.temp", op=">", value=0)

    def trigger(gpio: int) -> msg.ExperimentTrigger:
        return msg.ExperimentTrigger(
            when=when, do=[msg.ExperimentTriggerAction(action="set_gpio", gpio=gpio, level=1)])

    await node._fire_trigger(0, trigger(12), 1.0)  # forbidden
    await node._fire_trigger(0, trigger(26), 1.0)  # owned by the link, node not in manual mode
    await node._fire_trigger(0, trigger(4), 1.0)   # free pin: the way to demo set_gpio with --sim-dio
    items = drain(node)
    assert [(e.data["gpio"], e.data["reason"]) for e in events(items, "dio.gpio_rejected")] == [
        (12, "pin_not_allowed"), (26, "owned_by_link")]
    assert [e.data["gpio"] for e in events(items, "dio.gpio")] == [4]
    assert node.gpio_out == {4: 1}


async def test_lossy_wire_leaves_the_receiver_at_its_stale_level() -> None:
    bench = Bench(cfg={"manual": True}, delay_us=100, jitter_us=0, loss=1.0)
    await bench.a.set_gpio(26, 1)
    await bench.run(0.5)
    assert bench.b.dio_stats()["rx"] == 0
    wire = bench.net.wire(bench.a.node_id, bench.b.node_id)
    assert wire.counters["edges_sent"] == 1 and wire.counters["edges_lost"] == 1


async def test_glitches_show_as_rate_limited_edge_events() -> None:
    bench = Bench(delay_us=0, jitter_us=0, glitch_rate=40.0)
    await bench.run(5.0)
    wire = bench.net.wire(bench.a.node_id, bench.b.node_id)
    st = bench.b.dio_stats()
    assert wire.counters["glitches"] > 100
    assert st["edges_rx"] > 100
    evs = events(drain(bench.b), "dio.edge")
    assert 1 <= len(evs) <= 25  # 5 per 1000 ms window, not one per edge
    assert all(e.severity == "debug" for e in evs)
    assert evs[-1].data["count"] <= st["edges_rx"]


async def test_blink_stimulus_toggles_dio_tx() -> None:
    bench = Bench(cfg={"blink_hz": 1.0}, delay_us=100, jitter_us=0)
    seen = set()
    for _ in range(40):
        await bench.run(DT)
        seen.add(bench.a.gpio_out.get(26, 0))
    assert seen == {0, 1}


@pytest.mark.parametrize("gpio", [14, 25, 26, 27])
async def test_link_owned_pins_are_rejected_in_normal_mode(gpio: int) -> None:
    bench = Bench(delay_us=100, jitter_us=0)
    drain(bench.a)
    assert not await bench.a.set_gpio(gpio, 1)
    (rejected,) = events(drain(bench.a), "dio.gpio_rejected")
    assert rejected.severity == "warning"
    assert rejected.data == {"gpio": gpio, "level": 1, "reason": "owned_by_link"}
    assert gpio not in bench.a.gpio_out


@pytest.mark.parametrize("gpio", [14, 25])
async def test_link_input_pins_are_rejected_even_in_manual_mode(gpio: int) -> None:
    manual = Bench(cfg={"manual": True}, delay_us=100, jitter_us=0)
    drain(manual.a)
    assert not await manual.a.set_gpio(gpio, 1)
    (rejected,) = events(drain(manual.a), "dio.gpio_rejected")
    assert rejected.data == {"gpio": gpio, "level": 1, "reason": "owned_by_link"}
    assert manual.a.gpio_out == {}


@pytest.mark.parametrize("gpio", [26, 27])
async def test_link_output_pins_are_freed_by_manual_mode(gpio: int) -> None:
    manual = Bench(cfg={"manual": True}, delay_us=100, jitter_us=0)
    drain(manual.a)
    assert await manual.a.set_gpio(gpio, 1)
    assert manual.a.gpio_out == {gpio: 1}
    items = drain(manual.a)
    assert not events(items, "dio.gpio_rejected")
    assert [e.data for e in events(items, "dio.gpio")] == [{"gpio": gpio, "level": 1}]


async def test_a_node_without_dio_rejects_all_four_link_pins() -> None:
    node = SimNode(1001)
    for gpio in (14, 25, 26, 27):
        assert not await node.set_gpio(gpio, 1)
    assert [e.data["reason"] for e in events(drain(node), "dio.gpio_rejected")] == ["owned_by_link"] * 4
    assert node.gpio_out == {}


async def test_free_pins_act_normally_in_both_modes() -> None:
    for cfg in ({}, {"manual": True}):
        node = SimNode(1001, dio=DioConfig(**cfg))
        for gpio in (4, 13, 16, 17, 18, 19, 21, 22, 23, 32, 33):
            assert await node.set_gpio(gpio, 1), (cfg, gpio)
        assert sorted(node.gpio_out) == [4, 13, 16, 17, 18, 19, 21, 22, 23, 32, 33]
        assert not events(drain(node), "dio.gpio_rejected")


async def test_manual_node_schedules_no_link_phases_but_still_receives() -> None:
    clock = FakeClock()
    net = SimNetwork(seed=3)
    a, b = net.spawn(2, dio=DioConfig(clock_us=clock, manual=True, blink_hz=5.0))
    net.connect_wire(a.node_id, b.node_id, delay_us=100, jitter_us=0)
    net.connect_wire(b.node_id, a.node_id, delay_us=100, jitter_us=0)
    for _ in range(80):  # 4 s: long enough for blinks, handshakes and bursts if any were scheduled
        clock.us += 50_000
        await a.dio_step(DT)
        await b.dio_step(DT)
    for node in (a, b):
        st = node.dio_stats()
        assert st["tx"] == 0 and st["edges_rx"] == 0
        assert st["frames_sent"] == 0 and st["handshakes_ok"] == 0 and st["handshakes_timeout"] == 0
        assert st["link_state"] == "init"
    assert await a.set_gpio(26, 1)  # only set_gpio moves dio.tx
    for _ in range(4):
        clock.us += 50_000
        await a.dio_step(DT)
        await b.dio_step(DT)
    assert b.dio_stats()["rx"] == 1


# -- handshake (N2) ------------------------------------------------------------------

async def test_handshake_rtt_is_twice_the_one_way_delay() -> None:
    bench = Bench(delay_us=40, jitter_us=0)
    await bench.run(3.5)
    items = drain(bench.a)
    rtts = samples(items, 18)
    assert len(rtts) == 3 and set(rtts) == {80}
    (up,) = events(items, "link.up")
    assert up.data["rtt_us"] == 80
    assert bench.a.dio_stats()["link_state"] == "up"


async def test_handshake_jitter_stays_within_bounds_and_is_deterministic() -> None:
    async def rtts(seed: int) -> list[int]:
        bench = Bench(seed=seed, delay_us=40, jitter_us=4)
        await bench.run(10.5)
        return samples(drain(bench.a), 18)

    first = await rtts(3)
    assert len(first) == 10 and all(72 <= r <= 88 for r in first)
    assert len(set(first)) > 1  # there is jitter
    assert first == await rtts(3)
    assert first != await rtts(4)


async def test_total_loss_times_out_and_raises_link_lost_once_then_recovers() -> None:
    bench = Bench(delay_us=40, jitter_us=0, loss=1.0)
    await bench.run(6.5)
    items = drain(bench.a)
    assert samples(items, 18) == []  # no RTT is published for a timeout
    (lost,) = events(items, "link.lost")
    assert lost.severity == "warning" and lost.data["reason"] == "handshake_timeout"
    st = bench.a.dio_stats()
    assert st["handshakes_ok"] == 0 and st["handshakes_timeout"] == 6 and st["link_state"] == "lost"
    assert st["frames_ok"] == 0 and st["frames_err"] == 0  # nothing got through

    # Plug it back in live.
    cfg = bench.net.set_wire(bench.a.node_id, bench.b.node_id, loss=0.0)
    bench.net.set_wire(bench.b.node_id, bench.a.node_id, loss=0.0)
    assert cfg.loss == 0.0
    await bench.run(2.5)
    items = drain(bench.a)
    assert len(events(items, "link.up")) == 1
    assert set(samples(items, 18)) == {80}
    assert bench.a.dio_stats()["link_state"] == "up"


async def test_unidirectional_wire_cannot_complete_a_handshake() -> None:
    clock = FakeClock()
    net = SimNetwork(seed=1)
    a, b = net.spawn(2, dio=DioConfig(clock_us=clock))
    net.connect_wire(a.node_id, b.node_id)
    for _ in range(80):
        clock.us += 50_000
        await a.dio_step(DT)
        await b.dio_step(DT)
    assert a.dio_stats()["handshakes_ok"] == 0 and a.dio_stats()["link_state"] == "lost"
    assert b.dio_stats()["frames_ok"] > 0  # the one wire that exists still carries frames


# -- frames (N3) --------------------------------------------------------------------

async def test_perfect_wire_delivers_every_frame_with_zero_ber() -> None:
    bench = Bench(delay_us=40, jitter_us=0)
    await bench.run(3.5)  # three bursts of 20, plus time for the last to land
    for rx, tx in ((bench.b, bench.a), (bench.a, bench.b)):
        st = rx.dio_stats()
        assert tx.dio_stats()["frames_sent"] == 60
        assert st["frames_ok"] == 60
        assert st["frames_err"] == 0 and st["bit_errors"] == 0 and st["ber"] == 0.0
        assert st["bits_rx"] > 0 and st["expected_seq"] == 60
    items = drain(bench.b)
    assert not events(items, "link.crc_err") and not events(items, "link.lost")
    assert events(items, "link.frame_ok")  # rate-limited, but present


async def test_noisy_wire_produces_errors_and_a_nonzero_ber() -> None:
    bench = Bench(delay_us=40, jitter_us=0, bit_error_rate=0.01)
    await bench.run(5.5)
    st = bench.b.dio_stats()
    assert st["frames_err"] > 0 and st["ber"] > 0.0
    assert st["frames_ok"] > 0  # not everything is destroyed at 1e-2
    assert st["bit_errors"] > 0 and st["bit_errors"] < st["bits_rx"]
    assert st["frames_ok"] + st["frames_err"] <= bench.a.dio_stats()["frames_sent"]
    items = drain(bench.b)
    crc = events(items, "link.crc_err")
    assert crc and crc[0].severity == "warning"
    assert len(crc) <= 6  # rate-limited, not one per bad frame
    # the counters are also what the NDB channels publish
    by_id = {ch: val for ch, _enc, val in bench.b._collect_due_samples(1.0)}
    assert by_id[20] == st["frames_err"] and by_id[19] == st["frames_ok"] and by_id[21] == pytest.approx(st["ber"])


async def test_total_frame_loss_counts_nothing_received() -> None:
    bench = Bench(delay_us=40, jitter_us=0, loss=1.0)
    await bench.run(3.5)
    st = bench.b.dio_stats()
    assert bench.a.dio_stats()["frames_sent"] == 60
    assert st["frames_ok"] == 0 and st["bits_rx"] == 0 and st["ber"] == 0.0


async def test_partial_frame_loss_is_seen_as_missed_frames_via_resync() -> None:
    bench = Bench(delay_us=40, jitter_us=0, loss=0.3)
    await bench.run(6.5)
    st = bench.b.dio_stats()
    assert 0 < st["frames_ok"] < 120
    assert st["bit_errors"] == 0  # a lost frame is not a bit error
    lost = [e for e in events(drain(bench.b), "link.lost") if e.data.get("reason") == "frames_missed"]
    assert lost and all(e.data["missed"] >= 1 for e in lost)


async def test_same_seed_same_run() -> None:
    async def run(seed: int) -> dict[str, Any]:
        bench = Bench(seed=seed, delay_us=40, loss=0.01, bit_error_rate=0.002, glitch_rate=1.0)
        await bench.run(5.0)
        return bench.b.dio_stats()

    assert await run(11) == await run(11)
    assert await run(11) != await run(12)


# -- event shapes (SPEC-LINK "Semántica de eventos": same keys as the firmware) -------

async def test_dio_edge_event_shape() -> None:
    bench = Bench(cfg={"manual": True}, delay_us=100, jitter_us=0)
    await bench.a.set_gpio(26, 1)
    await bench.run(0.2)
    (ev,) = events(drain(bench.b), "dio.edge")
    assert ev.severity == "debug" and ev.data == {"level": 1, "count": 1}


async def test_link_up_frame_ok_event_shapes_on_a_clean_link() -> None:
    bench = Bench(delay_us=40, jitter_us=0)
    await bench.run(2.5)
    items = drain(bench.b)
    (up,) = events(items, "link.up")
    assert up.severity == "info" and up.data == {"rtt_us": 80, "timeouts": 0}
    ok = events(items, "link.frame_ok")[0]
    assert ok.severity == "info"
    assert set(ok.data) == {"frames_ok", "len"}  # nothing the firmware does not send, no suppressed yet
    assert ok.data["frames_ok"] >= 1 and 1 <= ok.data["len"] <= 32


async def test_link_lost_by_handshake_event_shape_and_link_up_counts_timeouts() -> None:
    bench = Bench(delay_us=40, jitter_us=0, loss=1.0)
    await bench.run(3.5)
    (lost,) = events(drain(bench.a), "link.lost")
    assert lost.severity == "warning"
    assert lost.data == {"timeouts": 3, "samples": 0, "reason": "handshake_timeout"}

    bench.net.set_wire(bench.a.node_id, bench.b.node_id, loss=0.0)
    bench.net.set_wire(bench.b.node_id, bench.a.node_id, loss=0.0)
    await bench.run(1.5)
    (up,) = events(drain(bench.a), "link.up")
    assert up.data == {"rtt_us": 80, "timeouts": bench.a.dio_stats()["handshakes_timeout"]}
    assert up.data["timeouts"] >= 3


async def test_link_lost_by_frames_event_shape() -> None:
    bench = Bench(delay_us=40, jitter_us=0, loss=0.3)
    await bench.run(6.5)
    lost = [e for e in events(drain(bench.b), "link.lost") if e.data.get("reason") == "frames_missed"]
    assert lost
    first = lost[0]
    assert first.severity == "warning"
    assert set(first.data) == {"missed", "seq", "reason"}  # `suppressed` only once something was swallowed
    assert first.data["missed"] >= 1 and first.data["seq"] >= first.data["missed"]


async def test_link_crc_err_event_shape_for_a_crc_error() -> None:
    bench = Bench(delay_us=40, jitter_us=0, bit_error_rate=0.01)
    await bench.run(3.5)
    first = events(drain(bench.b), "link.crc_err")[0]
    assert first.severity == "warning"
    assert set(first.data) == {"frames_err", "len", "reason"}
    assert first.data["reason"] in ("crc", "len") and first.data["frames_err"] >= 1


async def test_a_len_error_emits_link_crc_err_with_reason_len() -> None:
    bench = Bench(delay_us=40, jitter_us=0)
    dio = bench.b._dio
    now = dio.config.clock_us()
    for report in dio.monitor.feed_bits(dl.frame_to_bits(bytes.fromhex("aa00"))):  # LEN = 0
        await bench.b._dio_frame_report(dio, report, now)
    (ev,) = events(drain(bench.b), "link.crc_err")
    assert ev.severity == "warning"
    assert ev.data == {"frames_err": 1, "len": 0, "reason": "len"}
    assert bench.b.dio_stats()["frames_err"] == 1 and bench.b.dio_stats()["expected_seq"] == 0


async def test_gpio_event_shapes() -> None:
    node = SimNode(1001)
    await node.set_gpio(4, 1)
    await node.set_gpio(12, 0)
    items = drain(node)
    (ok,) = events(items, "dio.gpio")
    (rej,) = events(items, "dio.gpio_rejected")
    assert (ok.severity, ok.data) == ("info", {"gpio": 4, "level": 1})
    assert (rej.severity, rej.data) == ("warning", {"gpio": 12, "level": 0, "reason": "pin_not_allowed"})


# -- event rate limits ---------------------------------------------------------------

async def test_event_limits_are_n_per_window_and_suppressed_is_reported_once() -> None:
    bench = Bench(delay_us=40, jitter_us=0)
    node = bench.b
    drain(node)

    async def burst(now_ms: int, n: int, code: str, key: str) -> None:
        for _ in range(n):
            await node._dio_event_limited(key, code, now_ms * 1000, "info", {"x": 1})

    # dio.edge: 5 per 1000 ms
    await burst(0, 8, "dio.edge", "dio.edge")
    await burst(999, 2, "dio.edge", "dio.edge")
    assert len(events(drain(node), "dio.edge")) == 5
    await burst(1000, 1, "dio.edge", "dio.edge")  # the window restarted: admitted, carrying the 5 swallowed
    (again,) = events(drain(node), "dio.edge")
    assert again.data == {"x": 1, "suppressed": 5}
    await burst(1001, 5, "dio.edge", "dio.edge")
    (last,) = events(drain(node), "dio.edge")[-1:]
    assert "suppressed" not in last.data  # nothing was swallowed since the previous emission

    # link.frame_ok: 1 per 5000 ms, link.crc_err: 2 per 5000 ms, link.lost by frames: 1 per 5000 ms
    for code, key, n in (("link.frame_ok", "link.frame_ok", 1), ("link.crc_err", "link.crc_err", 2),
                         ("link.lost", "link.lost.frames", 1)):
        await burst(10_000, 6, code, key)
        assert len(events(drain(node), code)) == n, code
        await burst(14_999, 1, code, key)
        assert not events(drain(node), code), code  # same window
        await burst(15_000, 1, code, key)
        (ev,) = events(drain(node), code)
        assert ev.data["suppressed"] == 6 - n + 1, code


async def test_crc_err_limiter_end_to_end_loses_nothing_it_does_not_report() -> None:
    bench = Bench(delay_us=40, jitter_us=0, bit_error_rate=0.02)
    await bench.run(12.0)
    evs = events(drain(bench.b), "link.crc_err")
    st = bench.b.dio_stats()
    assert st["frames_err"] > 6
    # 2 per 5 s window: at most 3 windows fit in 12 s
    assert 2 <= len(evs) <= 6
    swallowed = sum(e.data.get("suppressed", 0) for e in evs)
    pending = bench.b._dio.limiters["link.crc_err"].suppressed
    assert len(evs) + swallowed + pending == st["frames_err"]  # every error is emitted, reported or pending
    assert "suppressed" not in evs[0].data and any("suppressed" in e.data for e in evs[2:])


# -- live reconfiguration and topology ------------------------------------------------

def test_set_wire_updates_topology_and_rejects_bad_input() -> None:
    net = SimNetwork(seed=1)
    a, b = net.spawn(2, dio=True)
    net.connect_wire(a.node_id, b.node_id, delay_us=40)
    cfg = net.set_wire(a.node_id, b.node_id, delay_us=120, bit_error_rate=0.001)
    assert (cfg.delay_us, cfg.bit_error_rate, cfg.loss) == (120, 0.001, 0.0)
    (wire,) = net.topology()["wires"]
    assert wire["tx"] == a.node_id and wire["rx"] == b.node_id
    assert wire["delay_us"] == 120 and wire["bit_error_rate"] == 0.001
    assert {"loss", "glitch_rate", "jitter_us", "edges_sent", "frames_sent"} <= wire.keys()

    with pytest.raises(ValueError):
        net.set_wire(a.node_id, b.node_id, loss=2.0)
    with pytest.raises(ValueError):
        net.set_wire(a.node_id, b.node_id, delay_us=-1)
    with pytest.raises(ValueError, match="unknown"):
        net.set_wire(a.node_id, b.node_id, voltage=3.3)
    with pytest.raises(KeyError):
        net.set_wire(b.node_id, a.node_id, loss=0.1)  # only A->B exists
    assert net.wire(a.node_id, b.node_id).config.delay_us == 120  # untouched by the failures


@pytest.mark.parametrize("bad", [float("inf"), float("-inf"), float("nan"), 10**400, None, "1", [1], True])
@pytest.mark.parametrize("field", ["delay_us", "jitter_us", "loss", "glitch_rate", "bit_error_rate"])
def test_a_badly_configured_wire_is_never_installed(field: str, bad: Any) -> None:
    net = SimNetwork(seed=1)
    a, b = net.spawn(2, dio=True)
    with pytest.raises(ValueError):
        net.connect_wire(a.node_id, b.node_id, **{field: bad})
    assert net.topology()["wires"] == []  # nothing half-installed...
    assert a._dio.out_wire is None and b._dio.in_wire is None
    net.connect_wire(a.node_id, b.node_id)  # ...and the pins are still free

    before = net.wire(a.node_id, b.node_id).config
    snapshot = (before.delay_us, before.loss, before.glitch_rate, before.bit_error_rate, before.jitter_us)
    with pytest.raises(ValueError):
        net.set_wire(a.node_id, b.node_id, **{field: bad})
    after = net.wire(a.node_id, b.node_id).config
    assert (after.delay_us, after.loss, after.glitch_rate, after.bit_error_rate, after.jitter_us) == snapshot


def test_wire_fault_kind_reconfigures_through_apply_fault() -> None:
    net = SimNetwork(seed=1)
    a, b = net.spawn(2, dio=True)
    net.connect_wire(a.node_id, b.node_id)
    out = net.apply_fault("wire", tx=a.node_id, rx=b.node_id, loss=0.5)
    assert out["loss"] == 0.5 and net.wire(a.node_id, b.node_id).config.loss == 0.5
    with pytest.raises(ValueError):
        net.apply_fault("wire", tx=a.node_id, rx=b.node_id, loss=9)


async def test_reboot_clears_pins_and_link_counters_and_reannounces_dio() -> None:
    bench = Bench(delay_us=40, jitter_us=0)
    await bench.a.set_gpio(4, 1)
    await bench.run(2.5)
    assert bench.a.dio_stats()["frames_ok"] > 0
    drain(bench.a)
    await bench.a._do_reboot()
    assert bench.a.gpio_out == {}
    st = bench.a.dio_stats()
    assert st["frames_ok"] == 0 and st["link_state"] == "init" and st["tx"] == 0
    assert hello_keys(drain(bench.a)) >= set(DIO_KEYS)


# -- through the gateway ---------------------------------------------------------------

AUTH = {"Authorization": f"Bearer {DEFAULT_TOKEN}"}


async def _wait_for(predicate, timeout: float = 8.0) -> Any:
    deadline = asyncio.get_running_loop().time() + timeout
    while asyncio.get_running_loop().time() < deadline:
        value = predicate()
        if value:
            return value
        await asyncio.sleep(0.02)
    raise AssertionError("condition not met in time")


async def test_gateway_dio_pair_streams_link_channels_through_the_normal_pipeline(tmp_path) -> None:
    store = Store(tmp_path / "dio.db")
    runtime = GatewayRuntime(store)
    seen: list[dict] = []
    runtime.subscribe(seen.append)
    await runtime.start()
    try:
        await runtime.attach_sim_dio_pair(blink_hz=5.0)
        a, b = (runtime.sim_network.nodes[i] for i in sorted(runtime.sim_network.nodes))
        for node in (a, b):
            node._dio.config.handshake_hz = 10.0  # do not wait a second per RTT
            node._dio.config.burst_period_s = 0.2

        def got(channel: str, node_id: int):
            return [e for e in seen if e["kind"] == "telemetry" and e["node_id"] == node_id
                    and e["data"]["channel"] == channel]

        await _wait_for(lambda: got("link.rtt_us", a.node_id) and got("dio.rx", b.node_id)
                        and got("link.ber", b.node_id) and got("link.frames_ok", b.node_id))
        rtts = [e["data"]["value"] for e in got("link.rtt_us", a.node_id)]
        assert all(1 <= r < 200 for r in rtts)
        ndb = runtime.registry.get(a.node_id)
        assert {c.key for c in ndb.channels_by_id.values()} >= set(DIO_KEYS)
        assert runtime.sim_network.topology()["wires"]
        # The NDB went out in several partial HELLOs; the store keeps the union.
        stored = json.loads(store.get_node(a.node_id)["ndb_json"])
        assert {c["key"] for c in stored} == {c.key for c in a.ndb}
    finally:
        await runtime.shutdown()
        store.close()


async def test_sim_default_keeps_three_plain_nodes_and_sim_dio_adds_two(tmp_path) -> None:
    async def fleet(settings: Settings, name: str, expected: int) -> list[dict]:
        store = Store(tmp_path / name)
        app = create_app(settings, store=store)
        async with app.router.lifespan_context(app), httpx.AsyncClient(
            transport=httpx.ASGITransport(app=app), base_url="http://test"
        ) as client:
            nodes: list[dict] = []
            for _ in range(200):
                nodes = (await client.get("/api/nodes", headers=AUTH)).json()
                if len(nodes) == expected:
                    break
                await asyncio.sleep(0.01)
            details = [(await client.get(f"/api/nodes/{n['node_id']}", headers=AUTH)).json() for n in nodes]
            topology = (await client.get("/api/network/topology", headers=AUTH)).json()
        return [{"id": d["node_id"], "keys": {c["key"] for c in d["ndb"]}, "wires": topology["wires"]} for d in details]

    plain = await fleet(Settings(sim_preload=3), "plain.db", 3)
    assert len(plain) == 3
    assert all("adc.a0" in n["keys"] and not (set(DIO_KEYS) & n["keys"]) for n in plain)
    assert plain[0]["wires"] == []

    both = await fleet(Settings(sim_preload=3, sim_dio=True), "both.db", 5)
    assert len(both) == 5
    with_dio = [n for n in both if "dio.tx" in n["keys"]]
    assert len(with_dio) == 2 and all(set(DIO_KEYS) <= n["keys"] and "adc.a0" not in n["keys"] for n in with_dio)
    assert sum("adc.a0" in n["keys"] for n in both) == 3
    assert {(w["tx"], w["rx"]) for w in both[0]["wires"]} == {
        (with_dio[0]["id"], with_dio[1]["id"]), (with_dio[1]["id"], with_dio[0]["id"])}


# -- outbox never blocks or raises the node ------------------------------------------

async def test_a_full_bounded_outbox_drops_and_counts_instead_of_raising() -> None:
    node = SimNode(1001)
    node.outbox = asyncio.Queue(maxsize=2)
    await node.set_gpio(4, 1)  # -> dio.gpio event
    await node.set_gpio(4, 0)
    assert node.outbox.qsize() == 2 and node.outbox_dropped == 0
    await node.set_gpio(12, 1)  # queue full: must not raise, must not block
    node._send_nowait(msg.TYPE_LOG, b"")  # the sync path too
    assert node.outbox.qsize() == 2 and node.outbox_dropped == 2
    # the node keeps working: once the station drains the queue, frames flow again
    drain(node)
    await node.set_gpio(13, 1)
    assert [e.data["gpio"] for e in events(drain(node), "dio.gpio")] == [13]
    assert node.outbox_dropped == 2
    # and the count is visible through node.info
    cmd = msg.Cmd(id=7, op="node.info")
    await node.on_station_frame(frames.parse(frames.encode(msg.TYPE_CMD, node.node_id, 0, cmd.to_payload())))
    (ack,) = [d for t, d in drain(node) if t == msg.TYPE_CMD_ACK]
    assert ack.data["outbox_dropped"] == 2


async def test_dropped_frames_still_spend_their_sequence_number() -> None:
    node = SimNode(1001)
    node.outbox = asyncio.Queue(maxsize=1)
    await node.set_gpio(4, 1)
    await node.set_gpio(4, 0)  # dropped
    await node.set_gpio(13, 1)  # dropped
    (first,) = [frames.parse(node.outbox.get_nowait())]
    await node.set_gpio(16, 1)
    second = frames.parse(node.outbox.get_nowait())
    assert second.seq - first.seq == 3  # the station can see that two frames went missing

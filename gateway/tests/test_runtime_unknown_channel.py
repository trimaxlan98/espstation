# A telemetry sample for a channel id the node never announced (typically a
# HELLO chunk that got lost) used to be dropped without a trace.
from __future__ import annotations

import logging

from espstation_gateway.protocol import messages as msg
from espstation_gateway.runtime import GatewayRuntime
from espstation_gateway.store import Store


def _hello(node_id: int) -> msg.Hello:
    return msg.Hello(
        mac="24:6f:28:00:00:01", node_id=node_id, label="n",
        chip=msg.ChipInfo(model="esp32", revision=3, cores=2, features=[]),
        fw=msg.FwInfo(version="0", build="t", idf="5", target="esp32"),
        caps=[], boot=msg.BootInfo(count=1, reason="power_on", uptime_ms=0),
        ndb=[msg.NdbChannel(id=1, key="sys.a", name="A", unit="", type="u32", rate_hz=1.0, group="system")],
    )


def _telemetry(channel_ids: list[int], n: int) -> msg.Telemetry:
    return msg.Telemetry(base_ts_ms=1000, flags=0, samples=[
        msg.Sample(ch=ch, dt_ms=i, enc=0x03, value=i) for i in range(n) for ch in channel_ids])


async def test_unknown_channel_is_logged_once_per_node_and_channel_and_counted(tmp_path, caplog) -> None:
    store = Store(tmp_path / "unknown.db")
    runtime = GatewayRuntime(store)
    hello = _hello(42)
    runtime.registry.on_hello(hello)
    store.upsert_node(42, mac=hello.mac, label="n", ndb=[c.model_dump() for c in hello.ndb])

    with caplog.at_level(logging.WARNING, logger="espstation_gateway.runtime"):
        await runtime._on_telemetry(42, 1, _telemetry([1, 16], 50))  # 50 samples of channel 16, unannounced
        await runtime._on_telemetry(42, 2, _telemetry([16, 17], 10))  # more of 16, and a first one of 17
    warnings = [r for r in caplog.records if r.levelno == logging.WARNING]
    assert len(warnings) == 2  # one for (42, 16), one for (42, 17): not one per sample
    assert "channel id 16" in warnings[0].getMessage() and "node 42" in warnings[0].getMessage()
    assert "channel id 17" in warnings[1].getMessage()
    assert runtime.unknown_channel_samples == {42: {16: 60, 17: 10}}
    assert runtime.node_detail(42)["unknown_channel_samples"] == {"16": 60, "17": 10}
    # the announced channel was stored normally
    assert store.query_samples(42, channel_ids=[1])
    store.close()


async def test_known_channels_never_warn(tmp_path, caplog) -> None:
    store = Store(tmp_path / "known.db")
    runtime = GatewayRuntime(store)
    hello = _hello(43)
    runtime.registry.on_hello(hello)
    store.upsert_node(43, mac=hello.mac, label="n", ndb=[c.model_dump() for c in hello.ndb])
    with caplog.at_level(logging.WARNING, logger="espstation_gateway.runtime"):
        await runtime._on_telemetry(43, 1, _telemetry([1], 5))
    assert not caplog.records and runtime.unknown_channel_samples == {}
    store.close()

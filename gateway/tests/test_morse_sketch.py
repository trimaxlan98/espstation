# The morse-duplex sketch adapter: plain text in, real ENLP frames out.
#
# The point of these tests is that nothing downstream needs a special case.
# So they do not inspect the adapter's internals: they feed it the bytes a
# real board actually produced, parse what comes out with the real codec, and
# push it through the real NodeRegistry.
from __future__ import annotations

from pathlib import Path

import pytest

from espstation_gateway.protocol import messages as msg
from espstation_gateway.protocol.ndb import NodeRegistry
from espstation_gateway.transports import morse_sketch as MS
from espstation_gateway.transports.base import TransportError
from espstation_gateway.transports.sim import morse_link as M

EVIDENCIA = (
    Path(__file__).resolve().parents[2]
    / "bench" / "practicas" / "morse-duplex" / "evidencia"
)


def decode_all(events):
    """(kind, payload) events -> [(type name, decoded message)]."""
    out = []
    for kind, payload in events:
        assert kind == "frame", f"adapter emitted a {kind}, not a frame"
        out.append((payload.type, msg.decode_message(payload.type, payload.payload)))
    return out


def test_first_feed_announces_a_hello_the_registry_accepts():
    dec = MS.MorseSketchDecoder(node_id=7, label="P1")
    out = decode_all(dec.feed(b""))
    assert out and all(t == msg.TYPE_HELLO for t, _ in out)
    # The table does not fit one frame, so it arrives as several HELLOs that
    # the registry merges (PROTOCOL.md 4.1) -- exactly like a real node's.
    reg = NodeRegistry()
    for _, hello in out:
        ndb = reg.on_hello(hello)
    assert len(reg) == 1
    # every Morse channel resolves by key and by id, which is what the
    # station needs to chart them without hard-coding anything
    for ch_id, key, _name, _unit, type_, _group in M.NDB_CHANNELS:
        assert ndb.by_key(key).id == ch_id
        assert ndb.by_id(ch_id).type == type_


def test_hello_does_not_claim_to_be_a_real_node():
    dec = MS.MorseSketchDecoder(node_id=7)
    hello = decode_all(dec.feed(b""))[0][1]
    assert hello.fw.build == "arduino-sketch"
    assert "experiment" not in hello.caps and "store_forward" not in hello.caps
    assert "read_only" in hello.caps


def test_symbols_letters_and_words_become_events():
    dec = MS.MorseSketchDecoder(node_id=7)
    dec.feed(b"")  # consume the HELLO
    out = decode_all(dec.feed(
        b"RX .\nRX -\nRX [letra: A] [bin: 01000001]\nRX [palabra]\n"
        b"TX [letra: ?] [morse: ........+]\n"
    ))
    assert [t for t, _ in out] == [msg.TYPE_EVENT] * 5
    evs = [e for _, e in out]
    assert [e.code for e in evs] == [
        M.EV_SYMBOL, M.EV_SYMBOL, M.EV_LETTER, M.EV_WORD, M.EV_UNKNOWN,
    ]
    assert evs[0].data == {"dir": "RX", "symbol": "."}
    assert evs[2].data == {"dir": "RX", "letter": "A", "byte": 0b01000001}
    assert evs[4].data == {"dir": "TX", "code": "........+"}
    assert evs[4].severity == "warning"


def test_rx_pulse_and_gap_become_telemetry_on_the_declared_channels():
    dec = MS.MorseSketchDecoder(node_id=7)
    dec.feed(b"")
    out = decode_all(dec.feed(b"# RX pulso_ms=180\n# RX silencio_ms=420\n"))
    assert [t for t, _ in out] == [msg.TYPE_TELEMETRY, msg.TYPE_TELEMETRY]
    pulse, gap = out[0][1], out[1][1]
    assert {(s.ch, s.value) for s in pulse.samples} == {(24, 180), (23, 0)}
    assert {(s.ch, s.value) for s in gap.samples} == {(25, 420), (23, 1)}


def test_key_line_publishes_bounces():
    dec = MS.MorseSketchDecoder(node_id=7)
    dec.feed(b"")
    out = decode_all(dec.feed(b"# llave debounce_ms=40 crudos=318 aceptados=98\n"))
    tel = out[0][1]
    assert {(s.ch, s.value) for s in tel.samples} == {(29, 318 - 98)}


def test_tx_edge_publishes_the_level_and_anchors_the_clock():
    dec = MS.MorseSketchDecoder(node_id=7)
    dec.feed(b"")
    out = decode_all(dec.feed(b"# TX flanco t_ms=50511 nivel=1\n"))
    tel = out[0][1]
    assert {(s.ch, s.value) for s in tel.samples} == {(22, 1)}
    # the board's own millisecond clock is now the base, not the station's
    assert tel.base_ts_ms >= 50511


def test_lines_split_across_chunks_are_reassembled():
    dec = MS.MorseSketchDecoder(node_id=7)
    dec.feed(b"")
    assert dec.feed(b"RX [letra: S] [bin: 010") == []
    out = decode_all(dec.feed(b"10011]\n"))
    assert out[0][1].data["letter"] == "S"


def test_garbage_bytes_do_not_destroy_the_line():
    dec = MS.MorseSketchDecoder(node_id=7)
    dec.feed(b"")
    out = decode_all(dec.feed(b"\xff" * 64 + b"RX [letra: O] [bin: 01001111]\n"))
    assert out[0][1].data["letter"] == "O"


def test_unrecognised_non_comment_lines_are_counted_not_swallowed():
    dec = MS.MorseSketchDecoder(node_id=7)
    dec.feed(b"")
    dec.feed(b"something the sketch never prints\n")
    assert dec.unparsed == 1


class _Dummy:
    async def open(self): ...
    async def close(self): ...
    async def send(self, b): ...
    def receive(self): ...


@pytest.mark.asyncio
async def test_a_command_is_refused_loudly_not_swallowed():
    from espstation_gateway.protocol import frames

    t = MS.MorseSketchTransport(_Dummy())
    cmd = frames.encode(msg.TYPE_CMD, 7, 0,
                        msg.Cmd(id=1, op="node.reboot").to_payload())
    with pytest.raises(TransportError, match="read-only"):
        await t.send(cmd)
    assert t.absorbed == 0


@pytest.mark.asyncio
async def test_housekeeping_acks_are_absorbed_so_the_handshake_completes():
    """Regression: refusing HELLO_ACK made the station's reply to our own
    HELLO kill the link, and the node showed up permanently offline."""
    from espstation_gateway.protocol import frames

    t = MS.MorseSketchTransport(_Dummy())
    ack = msg.HelloAck(session="s", host_time=0.0, accepted=True)
    for type_, payload in (
        (msg.TYPE_HELLO_ACK, ack.to_payload()),
        (msg.TYPE_TELEM_ACK, msg.TelemAck(node=7, last_seq=1, flags=0).to_bytes()),
    ):
        await t.send(frames.encode(type_, 7, 0, payload))
    assert t.absorbed == 2


# -- end to end over a real bench log --------------------------------------
@pytest.mark.parametrize("log", ["tanda_k40_l600_A.log", "tanda_k40_l600_B.log"])
def test_a_whole_real_session_replays_into_valid_frames(log):
    """Every byte a real board printed, through the adapter, into the
    registry. Nothing may raise, the node must end up on the books, and the
    letters that come out as events must be exactly the ones in the log."""
    path = EVIDENCIA / log
    if not path.is_file():
        pytest.skip(f"missing evidence {log}")

    # strip the capture's host timestamp: this is what the wire carried
    wire = b""
    printed: list[str] = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw[10:] if len(raw) > 10 else raw
        if line.startswith("#MARK") or line.startswith(">>>"):
            continue
        wire += line.encode("latin1", "replace") + b"\n"
        parsed = M.parse_line(line)
        if parsed is not None and parsed.kind == "letter":
            printed.append(parsed.value)

    dec = MS.MorseSketchDecoder(node_id=9, label=log)
    reg = NodeRegistry()
    letters: list[str] = []
    telemetry = 0
    # feed in small chunks, so chunk boundaries fall mid-line on purpose
    for i in range(0, len(wire), 37):
        for kind, frame in dec.feed(wire[i:i + 37]):
            assert kind == "frame"
            decoded = msg.decode_message(frame.type, frame.payload)
            if frame.type == msg.TYPE_HELLO:
                reg.on_hello(decoded)
            elif frame.type == msg.TYPE_TELEMETRY:
                telemetry += 1
                for s in decoded.samples:
                    reg.require(9).by_id(s.ch)      # every channel was declared
            elif frame.type == msg.TYPE_EVENT and decoded.code == M.EV_LETTER:
                letters.append(decoded.data["letter"])

    assert len(reg) == 1
    assert letters == printed and letters, f"{len(letters)} vs {len(printed)}"
    assert telemetry > 20
    assert dec.unparsed == 0, "the adapter did not understand a line of a real log"

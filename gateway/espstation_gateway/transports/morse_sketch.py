# Adapter: the morse-duplex Arduino sketch, seen by the station as a node.
#
# The bench practice (bench/practicas/morse-duplex/) runs an Arduino sketch
# that prints plain text at 115200, not ENLP. This module makes those boards
# observable from the app WITHOUT reflashing them and WITHOUT a second
# protocol path: it is a FrameDecoder, so the text goes in and *real ENLP
# frames* come out, built with the one real codec (protocol.frames /
# protocol.messages) and immediately re-parsed, which is what proves they are
# well-formed. Everything downstream -- NodeRegistry, the store, REST, WS and
# the desktop -- sees an ordinary node and needs no special case. D-8 holds:
# there is still exactly one codec.
#
# What this is NOT: these boards are not espstation-fw nodes. They run no
# experiment runtime, they have no NVS spec, they do not store and forward,
# and they cannot be commanded from the app (see "Commands" below). The HELLO
# says so in `fw.build` and `caps`, so nothing downstream can mistake one for
# a real node. The real-firmware path is firmware/components/esps_morse/.
#
# Commands: NOT supported in this slice, on purpose. Setting p/l/w/d/k on the
# boards would need a `morse.*` op in the command table, and that is a
# protocol change -- atomic across PROTOCOL.md, the YAML, esps_proto and the
# gateway (AGENTS.md hard rule 1). Until that is done this adapter is
# read-only and the thresholds are set the way the practice already sets them,
# with bench/practicas/clave-morse/herramientas/puente_serie.py.
from __future__ import annotations

import asyncio
import pathlib
import time
from typing import AsyncIterator

from ..protocol import frames, messages as msg, spec as protocol_spec
from .base import Transport, TransportError
from .sim import morse_link as M

# The sketch prints nothing in idle, so a line is always an event; 4 KiB is
# far more than any single line and bounds the buffer if the port goes mad.
MAX_LINE = 4096


def morse_ndb() -> list[msg.NdbChannel]:
    """The NDB this adapter announces: uptime plus the eight Morse channels.

    PROTOCOL.md asks every node for the system channels, and this adapter
    deliberately declares only sys.uptime of them: it has no way to know the
    board's heap or RSSI, and announcing a channel it can never sample would
    be a lie the station would chart as a gap. That is a real limitation of
    adapting a text protocol and it is why esps_morse exists.
    """
    out = [
        msg.NdbChannel(id=3, key="sys.uptime", name="Uptime", unit="s", type="u32",
                       rate_hz=0.2, group="system"),
    ]
    for ch_id, key, name, unit, type_, group in M.NDB_CHANNELS:
        out.append(msg.NdbChannel(id=ch_id, key=key, name=name, unit=unit,
                                  type=type_, rate_hz=0.0, group=group))
    return out


# channel id -> the encoding code of its declared semantic type, resolved
# once from the protocol spec so a rename in the YAML cannot pass unnoticed.
_ENC_OF_CHANNEL: dict[int, int] = {
    ch.id: protocol_spec.encoding_by_name()[ch.type] for ch in morse_ndb()
}


class MorseSketchDecoder:
    """FrameDecoder: morse-duplex sketch text in, ENLP frames out.

    Timestamps. PROTOCOL.md wants `uint32` monotonic milliseconds from the
    node. The sketch stamps only two kinds of line (`# TX flanco t_ms=` and
    `# resumen t_ms=`), so this adapter anchors on the last stamp it saw and
    extrapolates with the station's own monotonic clock between anchors. Until
    the first stamp arrives the clock is purely station-side. Those timestamps
    are therefore a reconstruction, not the board's own -- which is stated
    here because the alternative is a number that looks authoritative and is
    not.
    """

    def __init__(self, node_id: int, *, label: str = "", mac: str = "") -> None:
        self.node_id = node_id
        self.label = label or f"morse-{node_id}"
        self.mac = mac or f"02:00:00:00:00:{node_id & 0xFF:02x}"
        self._buf = b""
        self._seq = 0
        self._announced = False
        self._t0 = time.monotonic()
        self._anchor_board_ms: int | None = None
        self._anchor_host: float = self._t0
        self._tx_level = 0
        self._counters: dict[str, dict[str, int]] = {"RX": {}, "TX": {}}
        self._key: dict[str, int] = {}
        # Lines that were not understood, so a caller can notice a sketch
        # whose format drifted instead of silently seeing no telemetry.
        self.unparsed = 0
        self.lines = 0
        # Last `# modo ... verbose=N` the board announced. The cadence
        # channels only exist when verbose is on, so the adapter has to
        # know; None means it has not said yet.
        self.verbose: int | None = None

    # -- clock ---------------------------------------------------------
    def _now_ms(self) -> int:
        host_ms = int((time.monotonic() - self._anchor_host) * 1000)
        if self._anchor_board_ms is None:
            return int((time.monotonic() - self._t0) * 1000) & 0xFFFFFFFF
        return (self._anchor_board_ms + host_ms) & 0xFFFFFFFF

    def _anchor(self, board_ms: int) -> None:
        self._anchor_board_ms = board_ms
        self._anchor_host = time.monotonic()

    # -- frame helpers -------------------------------------------------
    def _frame(self, type_code: int, payload: bytes) -> frames.Frame:
        """Builds a real ENLP frame and parses it straight back. The round
        trip is the point: if anything here produced a malformed frame, it
        would raise now rather than reach the registry."""
        body = frames.encode(type_code, self.node_id, self._seq, payload)
        self._seq = (self._seq + 1) & 0xFFFF
        return frames.parse(body)

    def _hello(self, ndb: list[msg.NdbChannel]) -> msg.Hello:
        return msg.Hello(
            mac=self.mac,
            node_id=self.node_id,
            label=self.label,
            chip=msg.ChipInfo(model="esp32", revision=3, cores=2, features=[]),
            fw=msg.FwInfo(version="bench-morse-duplex", build="arduino-sketch",
                          idf="n/a", target="esp32"),
            # No experiment runtime, no store-and-forward, no commands: the
            # caps list is the honest description of what this really is.
            caps=["telemetry", "morse", "read_only"],
            boot=msg.BootInfo(count=0, reason="adapter_attach", uptime_ms=self._now_ms()),
            ndb=ndb,
        )

    def _hello_frames(self) -> list[tuple[str, object]]:
        """The NDB as one or more HELLOs.

        Nine channels of descriptor do not fit in MAX_PAYLOAD, and
        PROTOCOL.md 4.1 lets a node extend its NDB by re-sending HELLO -- the
        station merges them. Same rule, same chunking, as the simulator's
        _hello_payloads(); a node that fits still sends exactly one.
        """
        out: list[tuple[str, object]] = []
        chunk: list[msg.NdbChannel] = []
        payloads: list[bytes] = []
        for ch in morse_ndb():
            candidate = self._hello(chunk + [ch]).to_payload()
            if len(candidate) > frames.MAX_PAYLOAD and chunk:
                payloads.append(self._hello(chunk).to_payload())
                chunk = [ch]
            else:
                chunk.append(ch)
        payloads.append(self._hello(chunk).to_payload())
        for payload in payloads:
            out.append(("frame", self._frame(msg.TYPE_HELLO, payload)))
        return out

    def _event(self, code: str, severity: str, data: dict) -> tuple[str, object]:
        ev = msg.Event(ts_ms=self._now_ms(), code=code, severity=severity, data=data)
        return ("frame", self._frame(msg.TYPE_EVENT, ev.to_payload()))

    def _telemetry(self, samples: list[tuple[int, int]]) -> tuple[str, object]:
        """samples: (channel id, value). The transport encoding of each
        sample is the channel's own declared semantic type, so the station
        never has to guess (PROTOCOL.md 4.4 allows them to differ; there is
        no reason for them to here)."""
        tel = msg.Telemetry(
            base_ts_ms=self._now_ms(),
            flags=0,
            samples=[msg.Sample(ch=ch, dt_ms=0, enc=_ENC_OF_CHANNEL[ch], value=value)
                     for ch, value in samples],
        )
        return ("frame", self._frame(msg.TYPE_TELEMETRY, tel.to_bytes()))

    # -- FrameDecoder --------------------------------------------------
    def feed(self, chunk: bytes) -> list[tuple[str, object]]:
        out: list[tuple[str, object]] = []
        if not self._announced:
            self._announced = True
            out += self._hello_frames()
        if not chunk:
            return out
        self._buf += chunk
        if len(self._buf) > MAX_LINE:
            # Keep the tail: a line longer than this is garbage, not a line.
            self._buf = self._buf[-MAX_LINE:]
        while b"\n" in self._buf:
            raw, self._buf = self._buf.split(b"\n", 1)
            out += self._line(raw)
        return out

    def flush(self) -> list[tuple[str, object]]:
        if not self._buf:
            return []
        raw, self._buf = self._buf, b""
        return self._line(raw)

    # -- one line ------------------------------------------------------
    def _line(self, raw: bytes) -> list[tuple[str, object]]:
        # 0xFF blocks show up in bench captures (README: cause not found);
        # dropping them keeps a usable line instead of discarding the event.
        text = raw.replace(b"\xff", b"").decode("latin1").replace("\r", "").strip()
        if not text:
            return []
        self.lines += 1
        line = M.parse_line(text)
        if line is None:
            return []
        out: list[tuple[str, object]] = []
        d = line.sentido or "?"

        if line.kind == "symbol":
            out.append(self._event(M.EV_SYMBOL, "debug",
                                   {"dir": d, "symbol": line.value}))
        elif line.kind == "letter":
            out.append(self._event(M.EV_LETTER, "info",
                                   {"dir": d, "letter": line.value, "byte": line.byte}))
        elif line.kind == "unknown":
            out.append(self._event(M.EV_UNKNOWN, "warning",
                                   {"dir": d, "code": line.value}))
        elif line.kind == "word":
            out.append(self._event(M.EV_WORD, "debug", {"dir": d}))
        elif line.kind == "pulse":
            if line.filtered:
                out.append(self._event(M.EV_FILTERED, "warning",
                                       {"dir": d, "ms": line.ms}))
            elif d == "RX":
                # The pulse is over, so the incoming line was 1 for ms and is
                # 0 now: both samples are known and both are published.
                out.append(self._telemetry([(24, line.ms), (23, 0)]))
        elif line.kind == "gap":
            if d == "RX":
                out.append(self._telemetry([(25, line.ms), (23, 1)]))
        elif line.kind == "edge":
            if line.t_ms is not None:
                self._anchor(line.t_ms)
            self._tx_level = line.level or 0
            out.append(self._telemetry([(22, self._tx_level)]))
        elif line.kind == "counters":
            f = line.fields
            self._counters[d] = f
            if d == "RX":
                out.append(self._telemetry([
                    (26, f.get("puntos", 0) + f.get("rayas", 0)),
                    (27, f.get("letras", 0)),
                    (28, f.get("desconocidas", 0)),
                ]))
        elif line.kind == "mode":
            self.verbose = line.fields.get("verbose")
        elif line.kind == "key":
            self._key = line.fields
            bounces = max(0, line.fields.get("crudos", 0) - line.fields.get("aceptados", 0))
            out.append(self._telemetry([(29, bounces)]))
        elif line.kind == "summary":
            if "t_ms" in line.fields:
                self._anchor(line.fields["t_ms"])
            out.append(self._telemetry([(3, self._now_ms() // 1000)]))
        elif line.kind == "thresholds":
            out.append(self._event("morse.thresholds", "info",
                                   dict({"dir": d}, **line.fields)))
        elif line.kind == "text":
            # Banners and rejections are worth surfacing; anything else is
            # counted so a format drift is visible instead of silent.
            if text.startswith("#"):
                out.append(self._event("morse.note", "info", {"text": text[:180]}))
            else:
                self.unparsed += 1
        return out


# Station -> node frames a read-only adapter may absorb. These are pure
# housekeeping: the station acknowledging something the board never asked for
# and cannot act on. Dropping them is correct; the board is not waiting.
_ABSORBED = frozenset({msg.TYPE_HELLO_ACK, msg.TYPE_TELEM_ACK, msg.TYPE_TIME_SYNC})


class MorseSketchTransport(Transport):
    """Wraps a byte transport and does not write ENLP to the board.

    The sketch speaks text and a handful of single letters, not ENLP, so
    nothing the station encodes can reach it. Two different things follow,
    and conflating them is what broke this adapter the first time:

      - HELLO_ACK / TELEM_ACK / TIME_SYNC are the station acknowledging the
        adapter's own frames. The board never sent them and is not waiting
        for them, so they are absorbed. Refusing these breaks the HELLO
        handshake and takes the link down.
      - A CMD or an EXP_SET is an operator asking the board to DO something.
        Swallowing one would let the app believe it landed, so it raises.

    When `morse.*` commands exist in the protocol, their translation to the
    sketch's `p/l/w/d/k` letters belongs here.
    """

    def __init__(self, inner: Transport) -> None:
        self._inner = inner
        self.absorbed = 0

    async def open(self) -> None:
        await self._inner.open()

    async def close(self) -> None:
        await self._inner.close()

    async def send(self, frame_bytes: bytes) -> None:
        try:
            type_ = frames.parse(frame_bytes).type
        except frames.FrameError:
            type_ = None
        if type_ in _ABSORBED:
            self.absorbed += 1
            return
        name = (protocol_spec.message_names().get(type_, hex(type_))
                if type_ is not None else "an unparseable frame")
        raise TransportError(
            f"the morse-duplex sketch does not speak ENLP, so {name} cannot reach it: "
            "this link is read-only (see transports/morse_sketch.py, 'Commands')"
        )

    async def write_text(self, text: str) -> None:
        """Write one of the sketch's own plain-text commands.

        This is the adapter configuring ITS OWN data source, not the station
        commanding a node: the only things ever written here are `r` (a pure
        read of thresholds and counters) and, at most once per attach, `v`
        (so the board emits the pulse and gap durations this adapter already
        declared as channels). An operator command still cannot reach the
        board -- see send() above.
        """
        writer = getattr(self._inner, "write_raw", None)
        if writer is None:
            raise TransportError("the inner transport cannot write raw bytes")
        await writer(text.encode("ascii"))

    def receive(self) -> AsyncIterator[bytes]:
        return self._inner.receive()


class MorseLogReplayTransport(Transport):
    """Replays a recorded capture as if a board were talking.

    Same adapter, same decoder, same frames: the only thing swapped out is
    where the bytes come from. That is what makes it a demo of the real path
    and not a mock of it -- the app cannot tell the difference, which is the
    whole point of being able to show this without hardware.

    The capture is a captura_serie.py log: `%9.3f ` and then the line the
    board printed. Those timestamps are replayed, so the Morse comes out at
    the speed a human actually keyed it; `speed` divides the waits and gaps
    longer than `max_gap_s` are compressed, exactly like the bench viewer's
    replay does, because nobody wants to watch 60 s of nothing.
    """

    def __init__(self, path: str, *, speed: float = 1.0, max_gap_s: float = 3.0,
                 loop: bool = False) -> None:
        self.path = path
        self.speed = max(speed, 0.01)
        self.max_gap_s = max_gap_s
        self.loop = loop
        self._closing = False
        self._lines: list[tuple[float, bytes]] = []

    def _load(self) -> None:
        import re

        out: list[tuple[float, bytes]] = []
        t0: float | None = None
        for raw in pathlib.Path(self.path).read_bytes().split(b"\n"):
            txt = raw.replace(b"\xff", b"").decode("latin1").replace("\r", "")
            m = re.match(r"^\s*(\d+\.\d+) ?(.*)$", txt)
            if not m:
                continue
            t, rest = float(m.group(1)), m.group(2)
            if rest.startswith("#MARK") or rest.startswith(">>>") or not rest:
                continue
            if t0 is None:
                t0 = t
            out.append((t - t0, rest.encode("latin1") + b"\n"))
        self._lines = out

    async def open(self) -> None:
        self._load()
        self._closing = False
        if not self._lines:
            raise TransportError(f"{self.path} has no replayable lines")

    async def close(self) -> None:
        self._closing = True

    async def send(self, frame_bytes: bytes) -> None:
        return   # a recording cannot be talked to; absorbing is the honest no-op

    async def write_text(self, text: str) -> None:
        return   # ditto: the adapter's priming has nothing to prime

    async def receive(self) -> AsyncIterator[bytes]:
        while not self._closing:
            prev = 0.0
            for t, line in self._lines:
                if self._closing:
                    return
                gap = min(t - prev, self.max_gap_s) / self.speed
                if gap > 0:
                    await asyncio.sleep(gap)
                prev = t
                yield line
            if not self.loop:
                return
            await asyncio.sleep(1.0)

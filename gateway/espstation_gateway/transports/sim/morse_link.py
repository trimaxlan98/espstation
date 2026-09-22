# The full-duplex Morse link contract, Python side.
#
# Contract: bench/practicas/morse-duplex/SPEC-DUPLEX.md (pins, the two
# decoders, thresholds, output format, NDB channels, golden vectors).
#
# Why this file duplicates logic that also exists in C
# (firmware/components/esps_morse/) and in an Arduino sketch
# (bench/practicas/morse-duplex/transceptor/transceptor.ino): SPEC-DUPLEX is
# NOT part of ENLP, exactly like SPEC-LINK is not. ENLP is what a node tells
# the station and that still goes through the one real codec
# (protocol.frames / protocol.messages), so D-8 holds. This module is the
# node-to-node data link -- what actually travels over the two crossed wires --
# and it exists independently in the sketch, in esps_morse (C11) and here.
# The only defence against the three drifting apart is the golden vectors in
# SPEC-DUPLEX.md, transcribed verbatim in tests/test_morse_link.py. If you
# change anything here, change SPEC-DUPLEX.md, esps_morse and its host tests
# in the same commit.
#
# Pure on purpose: no asyncio, no global state, no I/O. The simulator
# (node.py / network.py) and the serial adapter drive it; it never reaches
# back into them.
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Iterable, Literal

# -- pins (SPEC-DUPLEX "Pines") --------------------------------------------
# 25/26 are deliberately the same pins as the digital link: it is the same
# physical cable on the bench. The two practices are never flashed at once.
PIN_KEY = 13      # INPUT_PULLDOWN, the operator's key
PIN_TX_DATA = 26  # OUTPUT, mirrors the debounced key towards the other board
PIN_RX_DATA = 25  # INPUT_PULLDOWN, comes from the other board's TX_DATA
PIN_LED_TX = 4    # OUTPUT, own key
PIN_LED_RX = 16   # OUTPUT, incoming level

MAX_SYMBOL = 8    # symbols per letter before the code is declared overflowed

# -- NDB channels (SPEC-DUPLEX "Canales NDB") ------------------------------
# Ids 22-29: the node-defined range is 16-127 (PROTOCOL.md channel_id_ranges)
# and 16-21 already belong to the digital link. Nothing here is a protocol
# change: the station learns these from HELLO like any other node channel.
NDB_CHANNELS: tuple[tuple[int, str, str, str, str, str], ...] = (
    (22, "morse.tx", "Morse TX", "", "u8", "morse"),
    (23, "morse.rx", "Morse RX", "", "u8", "morse"),
    (24, "morse.pulse_ms", "Last pulse", "ms", "u32", "morse"),
    (25, "morse.gap_ms", "Last gap", "ms", "u32", "morse"),
    (26, "morse.symbols", "Symbols", "", "u32", "morse"),
    (27, "morse.letters", "Letters", "", "u32", "morse"),
    (28, "morse.unknown", "Unknown codes", "", "u32", "morse"),
    (29, "morse.bounces", "Key bounces", "", "u32", "morse"),
)

# -- event codes (SPEC-DUPLEX "Semantica de eventos") ----------------------
EV_SYMBOL = "morse.symbol"    # a dot or a dash was decided
EV_LETTER = "morse.letter"    # a letter closed
EV_WORD = "morse.word"        # a word gap closed
EV_UNKNOWN = "morse.unknown"  # a code not in the table, or overflowed
EV_FILTERED = "morse.filtered"  # a pulse shorter than debounce_ms

# -- the table (SPEC-DUPLEX "Tabla Morse") ---------------------------------
# International Morse, letters and digits. Written as code -> char so a
# lookup is what the decoder needs; TEXT_TO_CODE is derived, never edited.
CODE_TO_CHAR: dict[str, str] = {
    ".-": "A", "-...": "B", "-.-.": "C", "-..": "D", ".": "E",
    "..-.": "F", "--.": "G", "....": "H", "..": "I", ".---": "J",
    "-.-": "K", ".-..": "L", "--": "M", "-.": "N", "---": "O",
    ".--.": "P", "--.-": "Q", ".-.": "R", "...": "S", "-": "T",
    "..-": "U", "...-": "V", ".--": "W", "-..-": "X", "-.--": "Y",
    "--..": "Z",
    "-----": "0", ".----": "1", "..---": "2", "...--": "3", "....-": "4",
    ".....": "5", "-....": "6", "--...": "7", "---..": "8", "----.": "9",
}
CHAR_TO_CODE: dict[str, str] = {c: k for k, c in CODE_TO_CHAR.items()}


# -- thresholds ------------------------------------------------------------
@dataclass
class Thresholds:
    """The four numbers a decoder needs. Defaults are SPEC-DUPLEX's start
    values, which are explicitly NOT calibrated: the bench measured
    punto_raya_ms ~= 300 with a wide empty band, and found no valid letter_ms
    for the operator tested (README "Resultados de la tanda 2")."""

    dot_dash_ms: int = 300
    letter_ms: int = 700
    word_ms: int = 1800
    debounce_ms: int = 15

    def validated(self) -> "Thresholds":
        """The same coherence rules the sketch enforces on p/l/w/d."""
        if not 1 <= self.dot_dash_ms <= 60000:
            raise ValueError("dot_dash_ms out of 1..60000")
        if not 1 <= self.letter_ms <= 60000:
            raise ValueError("letter_ms out of 1..60000")
        if not 1 <= self.word_ms <= 60000:
            raise ValueError("word_ms out of 1..60000")
        if not 0 <= self.debounce_ms <= 200:
            raise ValueError("debounce_ms out of 0..200")
        if self.letter_ms >= self.word_ms:
            raise ValueError("letter_ms must be smaller than word_ms")
        return self


@dataclass(frozen=True)
class MorseEvent:
    """One thing the decoder decided. `ms` carries the duration that caused
    it (pulse for symbols, gap for letters/words) so the station can chart a
    cadence without re-deriving it."""

    kind: Literal["symbol", "letter", "word", "unknown", "filtered"]
    value: str = ""    # "." / "-" for symbols, the char for letters, the code for unknown
    ms: int = 0
    byte: int | None = None   # ASCII byte of a decoded letter, None otherwise


# -- uint32 clock arithmetic ----------------------------------------------
# The node's clocks are uint32 and they wrap: micros() every ~71.6 min,
# millis() every ~49.7 days [D-10]. In C -- both in the sketch and in
# esps_morse -- unsigned subtraction is DEFINED to wrap, which is what makes
# `(uint32_t)(now - then) / 1000UL` stay correct across the rollover; decode.c
# and key.c both say so at the line. Python ints are unbounded, so the mirror
# has to mask explicitly or it stops being a mirror: `now - then` across the
# wrap yields a large NEGATIVE number, and then
#   * a pulse straddling the wrap is below every debounce threshold, so C
#     reports a symbol and Python reported `filtered`;
#   * a gap straddling the wrap never reaches letter_ms, so C closes the
#     letter and Python lost it forever;
#   * the key's stability window never elapses, so the key jams.
UINT32 = 0xFFFFFFFF


def elapsed_ms(now: int, then: int) -> int:
    """Whole milliseconds between two uint32 stamps, wrap-safe.

    Mirrors `elapsed_ms()` of firmware/components/esps_morse/src/decode.c and
    the `(uint32_t)(t_us - d.t_subida_us) / 1000UL` the sketch writes inline.
    Truncating division on purpose -- do not round it, the +-1 ms agreement
    the bench measured depends on it.
    """
    return ((now - then) & UINT32) // 1000


def _tick_stamps(start_us: int, span_us: int, *, inclusive: bool) -> Iterable[int]:
    """The 1 ms stamps a loop would have ticked at over `span_us`, wrapping
    like micros() does. Used only by Decoder.feed()."""
    step = 1000
    while step < span_us or (inclusive and step <= span_us):
        yield (start_us + step) & UINT32
        step += 1000


# -- the decoder -----------------------------------------------------------
@dataclass
class Decoder:
    """The pulse/silence state machine, one instance per direction.

    Mirrors `struct Dec` + `procesarFlanco` + `revisarSilencio` of
    transceptor.ino line by line, including the order in which events come
    out: a symbol is emitted the moment the pulse ends, and the letter is
    emitted during the following silence, BEFORE the gap that produced it is
    reported. Anything that changes that order breaks the golden vectors.

    Time is microseconds, like micros() on the node. Durations are truncated
    to whole milliseconds by integer division, which is what makes the
    +-1 ms discrepancy the bench measured (README "El enlace conserva las
    duraciones") reproducible here.
    """

    th: Thresholds = field(default_factory=Thresholds)
    level: int = 0
    in_pulse: bool = False
    have_fall: bool = False
    measuring: bool = False
    overflowed: bool = False
    letter_since_word: bool = False
    t_rise_us: int = 0
    t_fall_us: int = 0
    symbol: str = ""
    # counters, same names as the sketch's `r` output
    filtered: int = 0
    dots: int = 0
    dashes: int = 0
    letters: int = 0
    unknown: int = 0
    repeated: int = 0
    last_pulse_ms: int = 0
    last_gap_ms: int = 0

    # -- edges --------------------------------------------------------
    def edge(self, level: int, t_us: int) -> list[MorseEvent]:
        """One edge, already timestamped. Returns what it decided."""
        out: list[MorseEvent] = []
        level = 1 if level else 0
        if level == self.level:
            self.repeated += 1          # read after the line had already gone back
            return out
        self.level = level

        if level:                                   # rising
            if self.have_fall:
                self.last_gap_ms = elapsed_ms(t_us, self.t_fall_us)
            self.t_rise_us = t_us
            self.in_pulse = True
            return out

        if not self.in_pulse:                       # started with the key closed
            return out
        self.in_pulse = False
        dur_ms = elapsed_ms(t_us, self.t_rise_us)
        self.last_pulse_ms = dur_ms

        if dur_ms < self.th.debounce_ms:            # noise: not a symbol, and it
            self.filtered += 1                      # does not break the silence
            return [MorseEvent("filtered", "", dur_ms)]

        s = "." if dur_ms < self.th.dot_dash_ms else "-"
        if s == ".":
            self.dots += 1
        else:
            self.dashes += 1
        if len(self.symbol) < MAX_SYMBOL:
            self.symbol += s
        else:
            self.overflowed = True
        out.append(MorseEvent("symbol", s, dur_ms))
        self.t_fall_us = t_us
        self.have_fall = True
        self.measuring = True
        return out

    # -- silence ------------------------------------------------------
    def tick(self, now_us: int) -> list[MorseEvent]:
        """Call as often as the loop would. Closes letters and words."""
        out: list[MorseEvent] = []
        if not self.measuring or self.level != 0:
            return out
        gap_ms = elapsed_ms(now_us, self.t_fall_us)
        if self.symbol and gap_ms >= self.th.letter_ms:
            out.append(self._close_letter(gap_ms))
        if self.letter_since_word and gap_ms >= self.th.word_ms:
            out.append(MorseEvent("word", " ", gap_ms))
            self.letter_since_word = False
        # Nothing left to wait for: stop measuring (and never compare two
        # absolute micros() across the ~71 min wrap).
        if not self.symbol and not self.letter_since_word:
            self.measuring = False
        return out

    def _close_letter(self, gap_ms: int) -> MorseEvent:
        code, self.symbol = self.symbol, ""
        overflowed, self.overflowed = self.overflowed, False
        self.letter_since_word = True
        char = CODE_TO_CHAR.get(code)
        if char is None or overflowed:
            self.unknown += 1
            return MorseEvent("unknown", code + ("+" if overflowed else ""), gap_ms)
        self.letters += 1
        return MorseEvent("letter", char, gap_ms, byte=ord(char))

    # -- convenience ---------------------------------------------------
    def feed(self, edges: Iterable[tuple[int, int]], settle_us: int = 0) -> list[MorseEvent]:
        """Drives a whole sequence of (level, t_us) edges, ticking between
        them at 1 ms like the loop does, and finally lets `settle_us` of
        silence elapse. Returns every event in order -- this is exactly what
        the golden vectors compare against."""
        out: list[MorseEvent] = []
        prev_us: int | None = None
        for level, t_us in edges:
            if prev_us is not None:
                span = (t_us - prev_us) & UINT32
                for t in _tick_stamps(prev_us, span, inclusive=False):
                    out += self.tick(t)
            out += self.tick(t_us)
            out += self.edge(level, t_us)
            prev_us = t_us
        if prev_us is not None and settle_us:
            for t in _tick_stamps(prev_us, settle_us, inclusive=True):
                out += self.tick(t)
        return out

    def text(self, events: Iterable[MorseEvent]) -> str:
        """The decoded text of a run of events, for a one-line assertion."""
        return "".join(
            e.value if e.kind in ("letter", "word") else ("¿" if e.kind == "unknown" else "")
            for e in events
        )


# -- the key (SPEC-DUPLEX "Transmisor") ------------------------------------
@dataclass
class Key:
    """The operator's key with its debounce, mirroring `atenderLlave`.

    A reading change only becomes an edge once the reading has held for
    `debounce_ms`. Both edges are delayed by the same amount, so the pulse
    duration the far end measures is the real one -- the bench proved this to
    the millisecond (`# TX pulso_ms=200` for a 200 ms press).
    """

    debounce_ms: int = 15
    stable: int = 0
    candidate: int = 0
    t_candidate_ms: int = 0
    raw_changes: int = 0
    accepted: int = 0

    def sample(self, reading: int, now_ms: int) -> int | None:
        """One poll of the pin. Returns the new wire level when an edge is
        accepted, None otherwise."""
        reading = 1 if reading else 0
        if reading != self.candidate:
            self.candidate = reading
            self.t_candidate_ms = now_ms
            self.raw_changes += 1
            return None
        # Wrap-safe, never a comparison of two absolute stamps: the same
        # note key.c carries at this exact line.
        if (self.candidate != self.stable
                and ((now_ms - self.t_candidate_ms) & UINT32) >= self.debounce_ms):
            self.stable = self.candidate
            self.accepted += 1
            return self.stable
        return None

    @property
    def bounces(self) -> int:
        """Reading changes that never became edges: the morse.bounces channel.

        Clamped at 0 like `esps_morse_key_bounces()`: accepted can never
        exceed raw_changes, but a half-initialised struct must read 0 rather
        than a negative count the station would chart.
        """
        if self.accepted > self.raw_changes:
            return 0
        return self.raw_changes - self.accepted


# -- generator (for the simulator) -----------------------------------------
def encode_text(text: str, unit_ms: int = 120, t0_us: int = 0) -> list[tuple[int, int]]:
    """Text -> the (level, t_us) edges a well-timed operator would produce.

    Standard Morse timing, which is what the bench found the human operator
    was NOT doing (README: element gaps 2-4.6x too long, which is why no
    letter_ms worked): dot = 1 unit, dash = 3, element gap = 1, letter gap =
    3, word gap = 7. A simulated node keys perfectly on purpose -- it is the
    reference the human is compared against.
    """
    edges: list[tuple[int, int]] = []
    t = t0_us
    u = unit_ms * 1000
    for wi, word in enumerate(text.upper().split()):
        if wi:
            t += 7 * u                       # word gap, before every word but the first
        for ci, ch in enumerate(word):
            code = CHAR_TO_CODE.get(ch)
            if code is None:
                continue
            if ci:
                t += 3 * u                   # letter gap, between letters of a word
            for si, sym in enumerate(code):
                if si:
                    t += u                   # element gap, between symbols of a letter
                edges.append((1, t))
                t += (3 * u) if sym == "-" else u
                edges.append((0, t))
    return edges


# -- the sketch's serial grammar (SPEC-DUPLEX "Formato de salida") ---------
# The transceptor prints plain text, not ENLP. This is the canonical parser
# for those lines; transports/morse_sketch.py turns the result into real ENLP
# frames. The bench's puente_serie.py keeps its own copy so it can run
# standalone without the gateway installed -- tests/test_morse_link.py checks
# the two agree on a corpus of real log lines, so they cannot drift silently.
_RE_SYMBOL = re.compile(r"^(RX|TX) ([.\-])$")
_RE_LETTER = re.compile(r"^(RX|TX) \[letra: (.)\] \[bin: ([01]{8})\]$")
_RE_UNKNOWN = re.compile(r"^(RX|TX) \[letra: \?\] \[morse: ([.\-]*\+?)\]$")
_RE_WORD = re.compile(r"^(RX|TX) \[palabra\]$")
_RE_PULSE = re.compile(r"^# (RX|TX) pulso_ms=(\d+)( filtrado)?$")
_RE_GAP = re.compile(r"^# (RX|TX) silencio_ms=(\d+)$")
_RE_EDGE = re.compile(r"^# TX flanco t_ms=(\d+) nivel=([01])$")
_RE_LABELLED = re.compile(r"^# (umbrales|contadores) (RX|TX) ")
_RE_KV = re.compile(r"(\w+)=(\d+)")


@dataclass(frozen=True)
class Line:
    """One parsed line of the sketch's output."""

    kind: str                      # symbol|letter|word|unknown|pulse|gap|edge|
                                   # thresholds|counters|key|mode|summary|text
    sentido: str = ""              # "RX" (the other hand) | "TX" (own echo) | ""
    value: str = ""
    ms: int = 0
    level: int | None = None
    t_ms: int | None = None
    byte: int | None = None
    filtered: bool = False
    fields: dict[str, int] = field(default_factory=dict)


def parse_line(text: str) -> Line | None:
    """Parses one line. Returns None for an empty line, and a `text` Line for
    anything unrecognised -- never raises, because a serial line can arrive
    truncated or with boot garbage in front of it."""
    text = text.strip()
    if not text:
        return None
    if any(ord(c) < 32 or ord(c) > 126 for c in text):
        return Line("text", value=text)

    m = _RE_SYMBOL.match(text)
    if m:
        return Line("symbol", m.group(1), m.group(2))
    m = _RE_LETTER.match(text)          # before _RE_UNKNOWN: this one needs [bin:
    if m:
        return Line("letter", m.group(1), m.group(2), byte=int(m.group(3), 2))
    m = _RE_UNKNOWN.match(text)
    if m:
        return Line("unknown", m.group(1), m.group(2))
    m = _RE_WORD.match(text)
    if m:
        return Line("word", m.group(1), " ")
    m = _RE_PULSE.match(text)
    if m:
        return Line("pulse", m.group(1), ms=int(m.group(2)), filtered=bool(m.group(3)))
    m = _RE_GAP.match(text)
    if m:
        return Line("gap", m.group(1), ms=int(m.group(2)))
    m = _RE_EDGE.match(text)
    if m:
        return Line("edge", "TX", t_ms=int(m.group(1)), level=int(m.group(2)))
    m = _RE_LABELLED.match(text)        # needs the label: "# contadores a cero" is text
    if m:
        kind = "thresholds" if m.group(1) == "umbrales" else "counters"
        return Line(kind, m.group(2), fields={k: int(v) for k, v in _RE_KV.findall(text)})
    for prefix, kind in (("# llave ", "key"), ("# modo ", "mode"), ("# resumen", "summary")):
        if text.startswith(prefix):
            return Line(kind, fields={k: int(v) for k, v in _RE_KV.findall(text)})
    return Line("text", value=text)

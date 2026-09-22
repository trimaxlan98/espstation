# Golden vectors for the full-duplex Morse link (SPEC-DUPLEX.md).
#
# This file is the defence against the three implementations of the link
# drifting apart: the Arduino sketch (bench/practicas/morse-duplex/
# transceptor/transceptor.ino), the C11 component (firmware/components/
# esps_morse/) and morse_link.py. Every vector here is either transcribed
# from SPEC-DUPLEX.md or taken from a real bench log, and the same vectors
# appear in the host tests of the other two.
from __future__ import annotations

import re
from pathlib import Path

import pytest

from espstation_gateway.transports.sim import morse_link as M

EVIDENCIA = (
    Path(__file__).resolve().parents[2]
    / "bench" / "practicas" / "morse-duplex" / "evidencia"
)

# International Morse, written out again so a typo in the module's table
# cannot be validated by itself (same trick as the bench's run_tests.py).
INDEPENDENT = {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.",
    "G": "--.", "H": "....", "I": "..", "J": ".---", "K": "-.-", "L": ".-..",
    "M": "--", "N": "-.", "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.",
    "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-",
    "Y": "-.--", "Z": "--..", "0": "-----", "1": ".----", "2": "..---",
    "3": "...--", "4": "....-", "5": ".....", "6": "-....", "7": "--...",
    "8": "---..", "9": "----.",
}


# -- the table -------------------------------------------------------------
def test_table_matches_an_independent_dictionary():
    assert M.CODE_TO_CHAR == {code: ch for ch, code in INDEPENDENT.items()}
    assert M.CHAR_TO_CODE == INDEPENDENT


def test_table_has_no_duplicate_codes():
    assert len(M.CODE_TO_CHAR) == len(INDEPENDENT)


# -- NDB channels ----------------------------------------------------------
def test_ndb_channel_ids_are_in_the_node_defined_range_and_clear_of_dio():
    from espstation_gateway.transports.sim import dio_link

    ids = [c[0] for c in M.NDB_CHANNELS]
    assert ids == sorted(ids), "declare them in id order"
    assert all(16 <= i <= 127 for i in ids), "PROTOCOL.md: node range is 16-127"
    dio_ids = {c[0] for c in dio_link.NDB_CHANNELS}
    assert not dio_ids & set(ids), "a node could publish both practices' channels"
    keys = [c[1] for c in M.NDB_CHANNELS]
    assert len(set(keys)) == len(keys)


# -- golden vector: SOS ----------------------------------------------------
def _edges(pattern, dot_ms=100, dash_ms=400, gap_ms=150, letter_gap_ms=900, t0=1_000_000):
    """Builds edges for e.g. '... --- ...' with the given timing."""
    edges, t = [], t0
    for li, letter in enumerate(pattern.split()):
        if li:
            t += letter_gap_ms * 1000
        for si, sym in enumerate(letter):
            if si:
                t += gap_ms * 1000
            edges.append((1, t))
            t += (dash_ms if sym == "-" else dot_ms) * 1000
            edges.append((0, t))
    return edges


def test_sos_golden_vector():
    d = M.Decoder(M.Thresholds())
    evs = d.feed(_edges("... --- ..."), settle_us=3_000_000)
    kinds = [(e.kind, e.value) for e in evs]
    assert kinds == [
        ("symbol", "."), ("symbol", "."), ("symbol", "."), ("letter", "S"),
        ("symbol", "-"), ("symbol", "-"), ("symbol", "-"), ("letter", "O"),
        ("symbol", "."), ("symbol", "."), ("symbol", "."), ("letter", "S"),
        ("word", " "),
    ]
    assert d.text(evs) == "SOS "
    assert (d.dots, d.dashes, d.letters, d.unknown) == (6, 3, 3, 0)
    letters = [e for e in evs if e.kind == "letter"]
    assert [e.byte for e in letters] == [0b01010011, 0b01001111, 0b01010011]


def test_word_is_reported_once_per_gap():
    d = M.Decoder(M.Thresholds())
    evs = d.feed(_edges("."), settle_us=10_000_000)
    assert [e.kind for e in evs] == ["symbol", "letter", "word"]


def test_unknown_and_overflowed_codes():
    d = M.Decoder(M.Thresholds())
    evs = d.feed(_edges("......."), settle_us=1_000_000)
    assert [(e.kind, e.value) for e in evs if e.kind == "unknown"] == [("unknown", ".......")]
    d2 = M.Decoder(M.Thresholds())
    evs2 = d2.feed(_edges("........."), settle_us=1_000_000)
    assert [(e.kind, e.value) for e in evs2 if e.kind == "unknown"] == [("unknown", "........+")]


@pytest.mark.parametrize("dur_ms,expected", [(299, "E"), (300, "T")])
def test_dot_dash_threshold_is_inclusive_upwards(dur_ms, expected):
    d = M.Decoder(M.Thresholds())
    evs = d.feed([(1, 1_000_000), (0, 1_000_000 + dur_ms * 1000)], settle_us=1_000_000)
    assert [e.value for e in evs if e.kind == "letter"] == [expected]


def test_pulses_below_debounce_are_filtered_and_do_not_break_the_silence():
    d = M.Decoder(M.Thresholds())
    evs = d.feed([(1, 1_000_000), (0, 1_005_000)], settle_us=2_000_000)
    assert [e.kind for e in evs] == ["filtered"]
    assert d.filtered == 1 and d.dots == 0 and d.letters == 0


def test_thresholds_reject_incoherent_values():
    with pytest.raises(ValueError):
        M.Thresholds(letter_ms=2000, word_ms=1800).validated()
    with pytest.raises(ValueError):
        M.Thresholds(debounce_ms=500).validated()
    M.Thresholds().validated()


# -- the key ---------------------------------------------------------------
def test_key_debounce_swallows_bounces_and_preserves_duration():
    k = M.Key(debounce_ms=15)
    accepted: list[tuple[int, int]] = []
    # closes at t=0 with 4 bounces, stays closed until t=200, opens with 2 more
    script = [(0, 0)] + [(t, 1 if t % 2 == 0 else 0) for t in range(1, 9)]
    reading = 0
    for ms in range(0, 400):
        for t, lv in script:
            if t == ms:
                reading = lv
        if ms == 8:
            reading = 1
        if ms == 200:
            reading = 0
        r = k.sample(reading, ms)
        if r is not None:
            accepted.append((ms, r))
    assert [lv for _, lv in accepted] == [1, 0]
    rise, fall = accepted[0][0], accepted[1][0]
    assert fall - rise == 200 - 8, "both edges delayed by the same debounce"
    assert k.accepted == 2 and k.bounces == k.raw_changes - 2


def test_key_with_debounce_zero_lets_every_change_through():
    # Even with d=0 a change is accepted on the NEXT poll, not the one that
    # saw it: the first poll only records the candidate. The sketch polls
    # thousands of times per millisecond, so on hardware that is immediate --
    # here it has to be written out, two polls per level.
    k = M.Key(debounce_ms=0)
    seen = []
    for ms, lv in enumerate([1, 1, 0, 0, 1, 1, 0, 0]):
        seen.append(k.sample(lv, ms))
    assert [s for s in seen if s is not None] == [1, 0, 1, 0]
    assert k.bounces == 0, "with no filter nothing is suppressed"


# -- generator round-trip --------------------------------------------------
@pytest.mark.parametrize("text", ["SOS", "E", "HELLO WORLD", "ESP 32"])
def test_encode_then_decode_is_the_identity(text):
    unit = 120
    edges = M.encode_text(text, unit_ms=unit)
    th = M.Thresholds(dot_dash_ms=2 * unit, letter_ms=2 * unit, word_ms=5 * unit,
                      debounce_ms=15).validated()
    d = M.Decoder(th)
    evs = d.feed(edges, settle_us=10 * unit * 1000)
    assert d.text(evs).strip() == text.upper()
    assert d.unknown == 0 and d.filtered == 0


# -- against real hardware evidence ---------------------------------------
def _stream_from_log(path: Path, sentido: str):
    """Rebuilds (edges, what the board printed) for one direction of a log.

    The board prints `pulso_ms` when a pulse ends and `silencio_ms` when the
    next one starts, so the two interleave; that is enough to reconstruct the
    edge times exactly. Letters are read back to compare against.
    """
    pulses: list[int] = []
    gaps: list[int] = []
    order: list[str] = []
    printed: list[tuple[str, str]] = []
    re_pulse = re.compile(rf"# {sentido} pulso_ms=(\d+)(?: (filtrado))?$")
    re_gap = re.compile(rf"# {sentido} silencio_ms=(\d+)$")
    re_letter = re.compile(rf"^{sentido} \[letra: (.)\] \[bin: [01]{{8}}\]$")
    re_unknown = re.compile(rf"^{sentido} \[letra: \?\] \[morse: ([.\-]*\+?)\]$")
    re_word = re.compile(rf"^{sentido} \[palabra\]$")
    started = False
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw[10:].strip() if len(raw) > 10 else raw.strip()
        if line.startswith("#MARK") and "INICIO" in line:
            started = True
            continue
        if not started or line.startswith(">>>"):
            continue
        m = re_pulse.search(line)
        if m:
            assert m.group(2) is None, "this vector assumes filtrados=0"
            pulses.append(int(m.group(1)))
            order.append("p")
            continue
        m = re_gap.search(line)
        if m:
            gaps.append(int(m.group(1)))
            order.append("g")
            continue
        for rx, kind in ((re_letter, "letter"), (re_unknown, "unknown"), (re_word, "word")):
            m = rx.match(line)
            if m:
                printed.append((kind, m.group(1) if kind != "word" else " "))
                break
    # p g p g ... — rebuild absolute edges from the durations
    edges: list[tuple[int, int]] = []
    t = 1_000_000
    pi = gi = 0
    for kind in order:
        if kind == "p":
            edges.append((1, t))
            t += pulses[pi] * 1000
            edges.append((0, t))
            pi += 1
        else:
            t += gaps[gi] * 1000
            gi += 1
    return edges, printed


@pytest.mark.parametrize("log,sentido", [
    ("tanda_k40_l600_A.log", "RX"),
    ("tanda_k40_l600_B.log", "RX"),
    ("tanda_k40_l600_A.log", "TX"),
    ("tanda_k40_l600_B.log", "TX"),
])
def test_decoder_reproduces_what_the_real_board_printed(log, sentido):
    """The strongest vector available: real pulses measured by a real ESP32,
    re-decoded here with the same thresholds, must give the same letters.

    The tanda ran with p=300 l=600 w=1800 d=40 frozen on both boards and
    filtrados=0, which is why the reconstruction from durations is exact.
    """
    path = EVIDENCIA / log
    if not path.is_file():
        pytest.skip(f"missing evidence {log}")
    edges, printed = _stream_from_log(path, sentido)
    assert edges, "no pulses found in the log"

    d = M.Decoder(M.Thresholds(dot_dash_ms=300, letter_ms=600, word_ms=1800,
                               debounce_ms=40).validated())
    evs = d.feed(edges, settle_us=3_000_000)
    got = [(e.kind, e.value) for e in evs if e.kind in ("letter", "unknown", "word")]

    # The board's last word gap has no following edge in the log, so the
    # reconstruction can emit one final `word` the log does not show.
    if len(got) == len(printed) + 1 and got[-1][0] == "word":
        got = got[:-1]
    assert got == printed, f"{len(got)} decoded vs {len(printed)} printed"
    assert d.filtered == 0

# Golden vectors are transcribed from bench/practicas/enlace-digital/SPEC-LINK.md
# and must stay identical in the three implementations of that contract (the
# Arduino sketches, firmware/components/esps_dio and this module). If a vector
# here changes, SPEC-LINK.md and the other two change in the same commit.
from __future__ import annotations

import pytest

from espstation_gateway.transports.sim import dio_link as dl

# -- SPEC-LINK "Vectores dorados" ---------------------------------------------

CRC_VECTORS = [
    (b"123456789", 0xF4),
    (bytes.fromhex("00"), 0x00),
    (bytes.fromhex("0100"), 0x15),
]

FRAME_VECTORS = [
    ("[0x00]", "00", "aa010015"),
    ("ESP", "455350", "aa03455350f8"),
    ("seq=1", "012f", "aa02012f0e"),
    ("seq=999", "e741", "aa02e7413e"),
    ("seq=0", "004a9d11b4cb431957fd2d5e40a7affdd0", "aa11004a9d11b4cb431957fd2d5e40a7affdd017"),
    ("seq=2", "022464f86b91dc51d3fac2fb8ca4561b5197dee1", "aa14022464f86b91dc51d3fac2fb8ca4561b5197dee1a0"),
]

SEQ_VECTORS = {0: FRAME_VECTORS[4], 1: FRAME_VECTORS[2], 2: FRAME_VECTORS[5], 999: FRAME_VECTORS[3]}

SPEC_ALLOWED_OUTPUTS = (4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33)


def bits_of(hexstr: str) -> list[int]:
    return dl.frame_to_bits(bytes.fromhex(hexstr))


def payload_frame(payload: bytes) -> list[int]:
    return dl.frame_to_bits(dl.build_frame(payload))


# -- CRC-8 --------------------------------------------------------------------

@pytest.mark.parametrize("data,expected", CRC_VECTORS)
def test_crc8_golden(data: bytes, expected: int) -> None:
    assert dl.crc8(data) == expected


def test_crc8_accepts_any_byte_iterable() -> None:
    assert dl.crc8(list(b"123456789")) == 0xF4
    assert dl.crc8(bytearray(b"123456789")) == 0xF4


# -- framing ------------------------------------------------------------------

@pytest.mark.parametrize("name,payload,frame", FRAME_VECTORS)
def test_build_frame_golden(name: str, payload: str, frame: str) -> None:
    assert dl.build_frame(bytes.fromhex(payload)).hex() == frame


@pytest.mark.parametrize("bad", [b"", bytes(33), bytes(100)])
def test_build_frame_rejects_bad_length(bad: bytes) -> None:
    with pytest.raises(ValueError):
        dl.build_frame(bad)


def test_build_frame_accepts_limits() -> None:
    assert len(dl.build_frame(b"x")) == 4
    assert len(dl.build_frame(bytes(32))) == 35


def test_frame_to_bits_is_msb_first() -> None:
    assert dl.frame_to_bits(b"\xaa") == [1, 0, 1, 0, 1, 0, 1, 0]
    assert dl.frame_to_bits(b"\x01\x80") == [0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0]


# -- generator ----------------------------------------------------------------

def test_xorshift32_known_value() -> None:
    assert dl.xorshift32(1) == 270369  # the classic first step of Marsaglia's xorshift32


@pytest.mark.parametrize("seq", sorted(SEQ_VECTORS))
def test_testframe_payload_golden(seq: int) -> None:
    _, payload, frame = SEQ_VECTORS[seq]
    generated = dl.testframe_payload(seq)
    assert generated.hex() == payload
    assert dl.build_frame(generated).hex() == frame


def test_testframe_payload_properties() -> None:
    lengths = set()
    for seq in range(2000):
        p = dl.testframe_payload(seq)
        assert 1 <= len(p) <= dl.MAX_PAYLOAD
        assert p[0] == seq & 0xFF
        assert p == dl.testframe_payload(seq)  # deterministic
        lengths.add(len(p))
    assert min(lengths) < 8 and max(lengths) > 24  # the whole 1..32 range is exercised


def test_testframe_payload_handles_uint32_edges() -> None:
    assert dl.testframe_payload(2**32 - 1)[0] == 0xFF
    assert dl.testframe_payload(2**32) == dl.testframe_payload(0)  # uint32 wrap


def test_resync_index() -> None:
    assert dl.resync_index(0, 0) == 0
    assert dl.resync_index(0, 5) == 5
    assert dl.resync_index(10, 12) == 12
    assert dl.resync_index(300, 0x2E) == 302  # k=0x12C, byte0 wrapped past 0xFF
    assert dl.resync_index(255, 0) == 256
    assert dl.resync_index(7, 6) == 7 + 255  # a "backwards" byte0 is read as a full-wrap forward jump


# -- receiver ----------------------------------------------------------------------

@pytest.mark.parametrize("name,payload,frame", FRAME_VECTORS)
def test_frame_rx_decodes_golden_frames(name: str, payload: str, frame: str) -> None:
    rx = dl.FrameRx()
    results = rx.feed_bits(bits_of(frame))
    assert results == [dl.RxResult("ok", len(payload) // 2, bytes.fromhex(payload))]
    assert rx.hunting


def test_frame_rx_ignores_garbage_before_sync() -> None:
    junk = [0] * 13 + [1] * 7 + bits_of("a9a8") + [0, 1, 1]  # near-misses of 0xAA, no 8-bit window equals it
    results = dl.FrameRx().feed_bits(junk + bits_of(FRAME_VECTORS[1][2]))
    assert [r.status for r in results] == ["ok"]
    assert results[0].payload == b"ESP"


def test_frame_rx_resyncs_on_sync_pattern_at_any_bit_offset() -> None:
    for offset in range(8):
        stream = [1, 0, 0] * 2 + [0] * offset + bits_of(FRAME_VECTORS[2][2])
        results = dl.FrameRx().feed_bits(stream)
        assert [r.payload for r in results] == [bytes.fromhex("012f")], offset


@pytest.mark.parametrize("bad_len", [0x00, 0x21, 0x80, 0xAA, 0xFF])
def test_frame_rx_rejects_invalid_len_and_recovers(bad_len: int) -> None:
    stream = bits_of("aa" + f"{bad_len:02x}") + [0] * dl.IDLE_GAP_BITS + bits_of(FRAME_VECTORS[1][2])
    results = dl.FrameRx().feed_bits(stream)
    assert [(r.status, r.length) for r in results] == [("len_err", 0), ("ok", 3)]  # len is 0 after LEN_ERR
    assert results[0].payload == b""


def test_frame_rx_flags_corrupt_crc_and_keeps_payload() -> None:
    frame = bytearray.fromhex(FRAME_VECTORS[1][2])
    frame[-1] ^= 0x01
    (result,) = dl.FrameRx().feed_bits(dl.frame_to_bits(bytes(frame)))
    assert result == dl.RxResult("crc_err", 3, b"ESP")


def test_frame_rx_flags_every_single_bit_error_in_len_payload_crc() -> None:
    # CRC-8 catches all single-bit errors. Flipping LEN can also turn the frame
    # into a different length that eats following idle bits, so give it a gap.
    frame = bytes.fromhex(FRAME_VECTORS[4][2])
    for i in range(8, len(frame) * 8):  # everything after the sync byte
        bits = dl.frame_to_bits(frame)
        bits[i] ^= 1
        results = dl.FrameRx().feed_bits(bits + [0] * 300)
        assert results and results[0].status in ("crc_err", "len_err"), i


def test_frame_rx_glued_frames_do_not_false_sync_when_crc_ends_in_1010() -> None:
    # CRC low nibble 0xA is 1010; the next frame's 0xAA starts 1010 1010, so a
    # sliding register that survived the CRC byte would see 0xAA four bits early.
    first = next(bytes([n]) for n in range(1, 256) if dl.crc8(bytes([1, n])) & 0x0F == 0x0A)
    assert dl.build_frame(first)[-1] & 0x0F == 0x0A
    for second in (b"ESP", bytes.fromhex("012f")):
        stream = payload_frame(first) + payload_frame(second)  # no idle gap at all
        assert dl.FrameRx().feed_bits(stream) == [
            dl.RxResult("ok", 1, first), dl.RxResult("ok", len(second), second)]


def test_frame_rx_back_to_back_frames_and_arbitrary_chunking() -> None:
    stream = []
    for _, _, frame in FRAME_VECTORS:
        stream += bits_of(frame) + [0] * dl.IDLE_GAP_BITS
    whole = dl.FrameRx().feed_bits(stream)
    assert len(whole) == len(FRAME_VECTORS)
    assert all(r.status == "ok" for r in whole)

    rx = dl.FrameRx()
    piecewise: list[dl.RxResult] = []
    for i in range(0, len(stream), 7):  # feed_bit state must survive any split
        piecewise += rx.feed_bits(stream[i:i + 7])
    assert piecewise == whole


def test_frame_rx_reset_discards_partial_frame() -> None:
    rx = dl.FrameRx()
    rx.feed_bits(bits_of(FRAME_VECTORS[4][2])[:40])
    assert not rx.hunting
    rx.reset()
    assert rx.hunting
    assert [r.payload for r in rx.feed_bits(bits_of(FRAME_VECTORS[1][2]))] == [b"ESP"]


# -- accounting ----------------------------------------------------------------------

def test_payload_bit_errors() -> None:
    assert dl.payload_bit_errors(b"\x00\xff", b"\x00\xff") == 0
    assert dl.payload_bit_errors(b"\x00", b"\xff") == 8
    assert dl.payload_bit_errors(b"\x03", b"\x01") == 1
    # Length mismatch follows the C implementation: `len(received)` bytes are
    # compared against the zero-padded expected payload.
    assert dl.payload_bit_errors(b"\x00\x01", b"\x00") == 1  # extra received byte vs zero padding
    assert dl.payload_bit_errors(b"\x00\xff", b"\x00") == 8
    assert dl.payload_bit_errors(b"\x00", b"\x00\xff") == 0  # expected bytes that never arrived are not counted
    assert dl.payload_bit_errors(b"", b"\x00") == 0


def test_stats_ber_is_zero_with_no_bits() -> None:
    stats = dl.LinkStats()
    assert stats.ber == 0.0
    assert stats.frames_err == 0
    stats.frames_crc_err, stats.frames_len_err = 2, 3
    assert stats.frames_err == 5


def test_monitor_counts_a_clean_stream() -> None:
    mon = dl.LinkMonitor()
    total_bits = 0
    for seq in range(6):
        p = dl.testframe_payload(seq)
        total_bits += (2 + len(p)) * 8
        (report,) = mon.feed_bits(payload_frame(p) + [0] * dl.IDLE_GAP_BITS)
        assert (report.status, report.expected_seq, report.missed, report.bit_errors) == ("ok", seq, 0, 0)
    assert mon.stats.frames_ok == 6
    assert mon.stats.bits_rx == total_bits
    assert mon.stats.bit_errors == 0 and mon.stats.ber == 0.0
    assert mon.expected == 6


def test_monitor_crc_error_counts_bit_errors_against_current_index() -> None:
    mon = dl.LinkMonitor()
    mon.feed_bits(payload_frame(dl.testframe_payload(0)))  # ok, expected -> 1
    bad = bytearray(dl.testframe_payload(1))
    bad[-1] ^= 0b101  # two bit errors in the payload
    frame = bytearray(dl.build_frame(dl.testframe_payload(1)))
    frame[2 + len(bad) - 1] ^= 0b101  # same corruption on the wire, CRC now wrong
    (report,) = mon.feed_bits(dl.frame_to_bits(bytes(frame)))
    assert (report.status, report.expected_seq, report.bit_errors) == ("crc_err", 1, 2)
    assert mon.stats.frames_crc_err == 1 and mon.stats.frames_ok == 1
    assert mon.stats.bit_errors == 2
    assert mon.stats.bits_rx == (2 + len(dl.testframe_payload(0))) * 8 + (2 + len(bad)) * 8
    assert mon.stats.ber == pytest.approx(2 / mon.stats.bits_rx)
    assert mon.expected == 2  # a corrupt frame still advances the index, without resync


def test_monitor_bit_errors_with_a_length_that_differs_from_the_expected_one() -> None:
    # SPEC-LINK "Contabilidad": the `len` received bytes are compared against
    # the expected payload zero-padded to 32; missing bytes are not counted, so
    # bit_errors never exceeds the bits actually received and BER <= 1.
    expected = dl.testframe_payload(0)  # 17 bytes
    longer = expected + b"\xff\x01"  # CRC-valid, 2 extra bytes vs zero padding: 8 + 1 errors
    mon = dl.LinkMonitor()
    (report,) = mon.feed_bits(payload_frame(longer))
    assert (report.status, report.bit_errors) == ("ok", 9)
    shorter = dl.testframe_payload(2)[:10]  # 10 of 20 bytes arrive: the 10 missing ones are not counted
    (report,) = mon.feed_bits(payload_frame(shorter))
    assert (report.status, report.expected_seq, report.bit_errors) == ("ok", 2, 0)
    assert 0 <= mon.stats.bit_errors <= mon.stats.bits_rx and mon.stats.ber <= 1.0


def test_monitor_counts_crc_collision_in_bit_errors() -> None:
    # Valid CRC, wrong payload: still one bit error, and it must be visible.
    mon = dl.LinkMonitor()
    wrong = bytearray(dl.testframe_payload(0))
    wrong[-1] ^= 0x01
    (report,) = mon.feed_bits(payload_frame(bytes(wrong)))
    assert report.status == "ok" and report.bit_errors == 1
    assert mon.stats.frames_ok == 1 and mon.stats.bit_errors == 1


def test_monitor_len_error_counts_frame_but_not_bits() -> None:
    mon = dl.LinkMonitor()
    (report,) = mon.feed_bits(bits_of("aa00"))
    assert report.status == "len_err"
    assert mon.stats.frames_len_err == 1 and mon.stats.frames_err == 1
    assert mon.stats.bits_rx == 0 and mon.stats.ber == 0.0
    assert mon.expected == 0


def test_monitor_resyncs_after_lost_frames_using_byte0() -> None:
    mon = dl.LinkMonitor()
    mon.feed_bits(payload_frame(dl.testframe_payload(0)))
    # frames 1..4 never arrive; 5 does
    (report,) = mon.feed_bits(payload_frame(dl.testframe_payload(5)))
    assert (report.expected_seq, report.missed, report.bit_errors) == (5, 4, 0)
    assert mon.expected == 6
    assert mon.stats.bit_errors == 0  # lost frames are not bit errors


def test_monitor_resync_across_the_byte0_wrap() -> None:
    mon = dl.LinkMonitor()
    mon.expected = 255
    (report,) = mon.feed_bits(payload_frame(dl.testframe_payload(256)))  # byte0 == 0
    assert (report.expected_seq, report.missed, report.bit_errors) == (256, 1, 0)


# -- pins ---------------------------------------------------------------------------

def test_allowed_outputs_match_the_spec_exactly() -> None:
    assert dl.allowed_output_pins() == SPEC_ALLOWED_OUTPUTS
    assert all(dl.is_allowed_output(p) for p in SPEC_ALLOWED_OUTPUTS)
    assert dl.output_rejection_reason(4) is None


@pytest.mark.parametrize("gpio", [0, 2, 5, 6, 7, 8, 9, 10, 11, 12, 15, 34, 35, 36, 39, 40, 48, 99, -1])
def test_forbidden_outputs_are_rejected_with_a_reason(gpio: int) -> None:
    assert not dl.is_allowed_output(gpio)
    assert dl.output_rejection_reason(gpio) == "pin_not_allowed"


@pytest.mark.parametrize("gpio", [14, 25])
def test_link_input_pins_are_owned_in_every_mode(gpio: int) -> None:
    assert dl.is_allowed_output(gpio)  # in the SPEC allow-list, yet never writable
    assert dl.output_rejection_reason(gpio) == "owned_by_link"
    assert dl.output_rejection_reason(gpio, manual=True) == "owned_by_link"


@pytest.mark.parametrize("gpio", [26, 27])
def test_link_output_pins_are_freed_only_by_manual_mode(gpio: int) -> None:
    assert dl.output_rejection_reason(gpio) == "owned_by_link"
    assert dl.output_rejection_reason(gpio, manual=True) is None


def test_free_pins_are_never_owned() -> None:
    for gpio in (4, 13, 16, 17, 18, 19, 21, 22, 23, 32, 33):
        assert dl.output_rejection_reason(gpio) is None
        assert dl.output_rejection_reason(gpio, manual=True) is None


def test_manual_mode_does_not_unlock_forbidden_pins() -> None:
    for gpio in (12, 8, 34, 99):
        assert dl.output_rejection_reason(gpio, manual=True) == "pin_not_allowed"


def test_gpio12_is_reported_as_not_allowed_like_the_firmware_does() -> None:
    # esps_dio_gpio_validate checks the allow-list first, so GPIO12 is
    # "pin_not_allowed" rather than some softer reason.
    assert dl.output_rejection_reason(12) == "pin_not_allowed"


def test_ndb_channel_table_matches_the_spec() -> None:
    assert [(c[0], c[1], c[4], c[5]) for c in dl.NDB_CHANNELS] == [
        (16, "dio.tx", "u8", "digital"),
        (17, "dio.rx", "u8", "digital"),
        (18, "link.rtt_us", "u32", "link"),
        (19, "link.frames_ok", "u32", "link"),
        (20, "link.frames_err", "u32", "link"),
        (21, "link.ber", "f32", "link"),
    ]


# -- drift gate against the C implementation -----------------------------------------
#
# SPEC-LINK "Debilidad conocida del formato": one bit flip at a time over the
# frames seq 0..999, each followed by 36 bytes of idle line. The C receiver
# (firmware/components/esps_dio) measured exactly 13 spurious FRAME_OK from LEN
# flips and 0 from PAYLOAD/CRC flips. These numbers are not tunable: if this
# fails, the two receivers behave differently -- find out which one is wrong.

def test_bit_flip_sweep_matches_the_c_implementation() -> None:
    idle = [0] * (36 * 8)
    len_flips = len_spurious = 0
    body_flips = body_spurious = 0
    for seq in range(1000):
        frame = dl.build_frame(dl.testframe_payload(seq))
        bits = dl.frame_to_bits(frame)
        for i in range(8, len(bits)):  # everything after the 0xAA sync byte
            flipped = bits[:]
            flipped[i] ^= 1
            spurious = sum(r.status == "ok" for r in dl.FrameRx().feed_bits(flipped + idle))
            if i < 16:
                len_flips += 1
                len_spurious += spurious
            else:
                body_flips += 1
                body_spurious += spurious
    assert (len_flips, body_flips) == (8000, 139976)
    assert len_spurious == 13
    assert body_spurious == 0


# -- event rate limiter (port of esps_dio_ratelimit_*) --------------------------------

def test_ratelimit_admits_n_per_window_and_counts_the_rest() -> None:
    rl = dl.RateLimit(window_ms=1000, max_in_window=5)
    assert [rl.allow(100 + i) for i in range(8)] == [True] * 5 + [False] * 3
    assert rl.take_suppressed() == 3
    assert rl.take_suppressed() == 0  # reading clears it


def test_ratelimit_window_restarts_at_the_first_call_after_expiry() -> None:
    rl = dl.RateLimit(window_ms=1000, max_in_window=1)
    assert rl.allow(0)
    assert not rl.allow(999)
    assert rl.allow(1000)  # expired exactly at window_ms
    assert not rl.allow(1999)  # the new window started at 1000, not on a fixed grid
    assert rl.allow(5000)  # a long silence banks nothing...
    assert not rl.allow(5001)  # ...and gives one event, not a catch-up burst
    assert rl.take_suppressed() == 3


def test_ratelimit_suppressed_survives_window_rollover_until_taken() -> None:
    rl = dl.RateLimit(window_ms=100, max_in_window=1)
    assert rl.allow(0) and not rl.allow(1) and not rl.allow(2)
    assert rl.allow(200)  # a new window admits again; the count is still pending
    assert rl.take_suppressed() == 2


def test_ratelimit_zero_budget_silences_and_zero_window_never_limits() -> None:
    silent = dl.RateLimit(window_ms=1000, max_in_window=0)
    assert not silent.allow(0) and not silent.allow(5000)
    assert silent.take_suppressed() == 2
    open_ = dl.RateLimit(window_ms=0, max_in_window=1)
    assert all(open_.allow(t) for t in (0, 0, 1, 1))


def test_ratelimit_survives_the_uint32_millisecond_wrap() -> None:
    rl = dl.RateLimit(window_ms=1000, max_in_window=1)
    start = 0xFFFFFFFF - 200  # ~49.7 days of uptime
    assert rl.allow(start)
    assert not rl.allow(start + 300)  # 300 ms later, across the wrap: still the same window
    assert rl.allow((start + 1200) & 0xFFFFFFFF)

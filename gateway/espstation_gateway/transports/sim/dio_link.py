# The two-wire digital link contract, Python side.
#
# Contract: bench/practicas/enlace-digital/SPEC-LINK.md (pins, frame layout,
# CRC-8, bit-by-bit receiver, accounting, test-frame generator, NDB channels).
#
# Why this file duplicates logic that also exists in C
# (firmware/components/esps_dio/): SPEC-LINK is NOT part of ENLP. ENLP is what
# a node tells the station, and that still goes through the one real codec
# (protocol.frames / protocol.messages), so D-8 holds. This module is the
# node-to-node data link -- the thing that travels over the physical wire --
# and it exists independently in the Arduino sketches, in esps_dio (C11) and
# here. The only defence against the three drifting apart is the golden
# vectors in SPEC-LINK.md, transcribed verbatim in tests/test_dio_link.py.
# If you change anything here, change SPEC-LINK.md, esps_dio and its host
# tests in the same commit.
#
# Pure on purpose: no asyncio, no global state, no I/O. The simulator
# (node.py / network.py) drives it; it never reaches back into them.
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Literal

SYNC = 0xAA
MAX_PAYLOAD = 32
CRC8_POLY = 0x07
IDLE_GAP_BITS = 16  # minimum idle gap between frames, in clock periods

PIN_TX_DATA = 26
PIN_RX_DATA = 25
PIN_TX_CLK = 27
PIN_RX_CLK = 14
PIN_LED = 4

# Pins the link owns (SPEC-LINK "Pines del enlace y modo manual"). The two
# INPUTS (14, 25) are refused by set_gpio always: a node cannot know whether
# the far end of that cable is an output. The two OUTPUTS (26, 27) are refused
# too, except in manual mode, where the node runs no link phases.
LINK_INPUT_PINS: frozenset[int] = frozenset({PIN_RX_CLK, PIN_RX_DATA})
LINK_OUTPUT_PINS: frozenset[int] = frozenset({PIN_TX_DATA, PIN_TX_CLK})
LINK_OWNED_PINS: frozenset[int] = LINK_INPUT_PINS | LINK_OUTPUT_PINS

# Salidas permitidas para set_gpio (SPEC-LINK "Pines"). 25 and 14 are the
# RX pins but the SPEC lists them as allowed outputs, so they are here.
_ALLOWED_OUTPUTS: frozenset[int] = frozenset(
    {4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33}
)

# SPEC-LINK "Canales NDB": (id, key, name, unit, type, group).
NDB_CHANNELS: tuple[tuple[int, str, str, str, str, str], ...] = (
    (16, "dio.tx", "DIO TX", "", "u8", "digital"),
    (17, "dio.rx", "DIO RX", "", "u8", "digital"),
    (18, "link.rtt_us", "Link RTT", "us", "u32", "link"),
    (19, "link.frames_ok", "Frames OK", "", "u32", "link"),
    (20, "link.frames_err", "Frames err", "", "u32", "link"),
    (21, "link.ber", "Bit error rate", "", "f32", "link"),
)


# -- pins ------------------------------------------------------------------

def allowed_output_pins() -> tuple[int, ...]:
    return tuple(sorted(_ALLOWED_OUTPUTS))


def is_allowed_output(gpio: int) -> bool:
    return gpio in _ALLOWED_OUTPUTS


def output_rejection_reason(gpio: int, *, manual: bool = False) -> str | None:
    """Why `gpio` may not be driven, or None if it may. The strings are the
    firmware's (`esps_dio_result_str`): "pin_not_allowed" for anything outside
    the SPEC allow-list, checked first so GPIO12 is always reported as that,
    and "owned_by_link" for the link's pins. A rejected set_gpio must never
    fail silently: this is the `reason` of the `dio.gpio_rejected` event."""
    if not is_allowed_output(gpio):
        return "pin_not_allowed"
    if gpio in LINK_INPUT_PINS or (gpio in LINK_OUTPUT_PINS and not manual):
        return "owned_by_link"
    return None


# -- CRC-8/SMBUS and framing -------------------------------------------------

def crc8(data: bytes | bytearray | Iterable[int]) -> int:
    """CRC-8/SMBUS: poly 0x07, init 0x00, no reflection, xorout 0x00."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ CRC8_POLY) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build_frame(payload: bytes) -> bytes:
    """0xAA | LEN | PAYLOAD | CRC8, CRC covering LEN||PAYLOAD (not the sync)."""
    n = len(payload)
    if not 1 <= n <= MAX_PAYLOAD:
        raise ValueError(f"payload length must be 1..{MAX_PAYLOAD}, got {n}")
    body = bytes([n]) + bytes(payload)
    return bytes([SYNC]) + body + bytes([crc8(body)])


def frame_to_bits(frame: bytes) -> list[int]:
    """Bytes in frame order, MSB first."""
    return [(byte >> shift) & 1 for byte in frame for shift in range(7, -1, -1)]


# -- receiver ------------------------------------------------------------------

_HUNT, _LEN, _PAYLOAD, _CRC = range(4)

RxStatus = Literal["ok", "crc_err", "len_err"]


@dataclass(frozen=True)
class RxResult:
    status: RxStatus
    length: int  # 0 for len_err (SPEC-LINK: after LEN_ERR, len is 0)
    payload: bytes


class FrameRx:
    """Bit-fed receiver state machine (SPEC-LINK "Receptor").

    HUNT shifts bits into an 8-bit register until it reads 0xAA, then LEN
    (0 or >32 -> discard, back to HUNT, reported as `len_err`), PAYLOAD, CRC.
    A completed frame is returned as ok/crc_err and the machine goes back to
    HUNT. The shift register is zeroed whenever a byte completes (SPEC-LINK
    "Detalles del receptor"): otherwise a frame glued to the previous one
    false-syncs when that CRC ends in 0b1010, four bits before the real 0xAA.
    """

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self._state = _HUNT
        self._shift = 0
        self._cur = 0
        self._nbits = 0
        self._len = 0
        self._buf = bytearray()

    @property
    def hunting(self) -> bool:
        return self._state == _HUNT

    def feed_bit(self, bit: int) -> RxResult | None:
        b = 1 if bit else 0
        if self._state == _HUNT:
            self._shift = ((self._shift << 1) | b) & 0xFF
            if self._shift == SYNC:
                self._state = _LEN
                self._cur = 0
                self._nbits = 0
            return None

        self._cur = ((self._cur << 1) | b) & 0xFF
        self._nbits += 1
        if self._nbits < 8:
            return None
        byte, self._cur, self._nbits = self._cur, 0, 0
        self._shift = 0

        if self._state == _LEN:
            if byte == 0 or byte > MAX_PAYLOAD:
                self.reset()
                return RxResult("len_err", 0, b"")
            self._len = byte
            self._buf = bytearray()
            self._state = _PAYLOAD
            return None
        if self._state == _PAYLOAD:
            self._buf.append(byte)
            if len(self._buf) == self._len:
                self._state = _CRC
            return None
        # _CRC
        payload = bytes(self._buf)
        ok = crc8(bytes([self._len]) + payload) == byte
        result = RxResult("ok" if ok else "crc_err", self._len, payload)
        self.reset()
        return result

    def feed_bits(self, bits: Iterable[int]) -> list[RxResult]:
        out: list[RxResult] = []
        for bit in bits:
            if not bit and self._state == _HUNT and self._shift == 0:
                continue  # idle line while hunting: provably a no-op, and it is most of the stream
            r = self.feed_bit(bit)
            if r is not None:
                out.append(r)
        return out


# -- test-frame generator ----------------------------------------------------------

SEED = 0xC0FFEE
_MASK32 = 0xFFFFFFFF


def xorshift32(x: int) -> int:
    x &= _MASK32
    x ^= (x << 13) & _MASK32
    x ^= x >> 17
    x ^= (x << 5) & _MASK32
    return x


def testframe_payload(seq: int) -> bytes:
    """Deterministic payload for test frame number `seq` (SPEC-LINK "Payload
    de prueba"). Byte 0 carries seq & 0xFF so a receiver can resync."""
    seq &= _MASK32
    st = (SEED ^ ((seq * 2654435761) & _MASK32)) & _MASK32
    if st == 0:
        st = 1
    st = xorshift32(st)
    length = 1 + (st % MAX_PAYLOAD)
    out = bytearray([seq & 0xFF])
    for _ in range(1, length):
        st = xorshift32(st)
        out.append((st >> 8) & 0xFF)
    return bytes(out)  # the C generator zero-pads out[len..32); payload_bit_errors does the same


def resync_index(expected: int, byte0: int) -> int:
    """New expected index after a CRC-valid frame whose payload[0] is `byte0`:
    k + ((byte0 - k) & 0xFF)."""
    return expected + ((byte0 - expected) & 0xFF)


# -- accounting ----------------------------------------------------------------------

def _popcount(x: int) -> int:
    return x.bit_count()


def payload_bit_errors(received: bytes, expected: bytes) -> int:
    """Differing bits over the `len(received)` bytes that arrived. If the
    received length differs from the expected one (a frame whose LEN was
    corrupted), the expected payload is zero-padded, exactly like the C
    implementation's 32-byte buffer: extra received bytes are compared against
    0, and expected bytes that never arrived are not counted."""
    padded = expected.ljust(len(received), b"\x00")
    return sum(_popcount(r ^ e) for r, e in zip(received, padded))


@dataclass
class LinkStats:
    bits_rx: int = 0  # LEN+PAYLOAD+CRC bits of every completed frame
    frames_ok: int = 0
    frames_crc_err: int = 0
    frames_len_err: int = 0
    bit_errors: int = 0

    @property
    def frames_err(self) -> int:
        return self.frames_crc_err + self.frames_len_err

    @property
    def ber(self) -> float:
        return self.bit_errors / self.bits_rx if self.bits_rx else 0.0


@dataclass(frozen=True)
class FrameReport:
    """What LinkMonitor learned from one frame outcome."""

    status: RxStatus
    length: int
    expected_seq: int  # index the payload was compared against
    missed: int  # frames skipped since the previous valid one (ok frames only)
    bit_errors: int


@dataclass
class LinkMonitor:
    """FrameRx + expected-index tracking + SPEC-LINK accounting: everything a
    receiving node does with the wire, minus the wire itself."""

    rx: FrameRx = field(default_factory=FrameRx)
    stats: LinkStats = field(default_factory=LinkStats)
    expected: int = 0

    def feed_bit(self, bit: int) -> FrameReport | None:
        result = self.rx.feed_bit(bit)
        if result is None:
            return None
        return self._account(result)

    def feed_bits(self, bits: Iterable[int]) -> list[FrameReport]:
        out: list[FrameReport] = []
        for bit in bits:
            r = self.feed_bit(bit)
            if r is not None:
                out.append(r)
        return out

    def _account(self, result: RxResult) -> FrameReport:
        stats = self.stats
        if result.status == "len_err":
            stats.frames_len_err += 1
            return FrameReport("len_err", 0, self.expected, 0, 0)

        missed = 0
        if result.status == "ok":
            # A valid frame is trusted to say which one it is: resync first,
            # then compare against that index.
            resynced = resync_index(self.expected, result.payload[0])
            missed = resynced - self.expected
            self.expected = resynced
            stats.frames_ok += 1
        else:
            # A corrupt frame's byte 0 is not to be trusted: compare against
            # the current index and just advance.
            stats.frames_crc_err += 1

        errors = payload_bit_errors(result.payload, testframe_payload(self.expected))
        # A CRC-valid frame with a wrong payload is a CRC collision: it is
        # counted in bit_errors like any other, so it shows up in the BER.
        stats.bit_errors += errors
        stats.bits_rx += (2 + result.length) * 8
        report = FrameReport(result.status, result.length, self.expected, missed, errors)
        self.expected += 1
        return report


# -- event rate limiting ------------------------------------------------------------
#
# Port of esps_dio_ratelimit_* (firmware/components/esps_dio/src/policy.c): a
# fixed window that admits `max_in_window` events. The window restarts at the
# first call that finds the previous one expired (it does not march on a fixed
# grid, so a long silence cannot bank "catch-up" allowances). Events refused
# are counted and reported as `data.suppressed` on the next one that gets
# through. Time is the node's uint32 millisecond clock; comparisons are done
# on the unsigned difference so the ~49.7-day wrap is harmless.

_MASK32_MS = 0xFFFFFFFF


@dataclass
class RateLimit:
    window_ms: int
    max_in_window: int
    window_start_ms: int = 0
    count: int = 0
    suppressed: int = 0
    started: bool = False

    def allow(self, now_ms: int) -> bool:
        now_ms &= _MASK32_MS
        if not self.started or ((now_ms - self.window_start_ms) & _MASK32_MS) >= self.window_ms:
            self.started = True
            self.window_start_ms = now_ms
            self.count = 0
        if self.count < self.max_in_window:
            self.count += 1
            return True
        self.suppressed = min(self.suppressed + 1, _MASK32_MS)
        return False

    def take_suppressed(self) -> int:
        """Read and clear the suppressed counter. Only meaningful right after
        allow() returned True: that is the event that reports it."""
        n, self.suppressed = self.suppressed, 0
        return n

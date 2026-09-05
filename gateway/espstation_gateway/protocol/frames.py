# Frame body per PROTOCOL.md section 3:
#   ver(1) type(1) node(2) seq(2) len(2) payload(len) crc16(2)
# All multi-byte ints little-endian (the node's native order) -- the gateway
# converts explicitly with `struct`, never by relying on host byte order.
#
# Two encodings ride on top of the same body:
#   - encode_cobs()/StreamingDecoder: serial (COBS + 0x00 delimiter)
#   - length-prefixed framing (u16 BE) for TCP/WS lives in transports/tcp.py,
#     since it needs no COBS step -- it reuses parse()/encode() directly.
from __future__ import annotations

import struct
from dataclasses import dataclass

from .cobs import CobsError, decode as cobs_decode, encode as cobs_encode
from .crc16 import crc16_ccitt_false

HEADER_STRUCT = struct.Struct("<BBHHH")  # ver, type, node, seq, len
HEADER_SIZE = HEADER_STRUCT.size  # 8
CRC_SIZE = 2
MAX_PAYLOAD = 1024
MAX_PAYLOAD_ESPNOW = 240
WIRE_VERSION = 1


class FrameError(ValueError):
    """Base class for frame parse failures."""


class FrameTooShortError(FrameError):
    pass


class FrameBadVersionError(FrameError):
    def __init__(self, got: int) -> None:
        super().__init__(f"unsupported ver byte {got!r} (expected {WIRE_VERSION})")
        self.got = got


class FrameBadLengthError(FrameError):
    def __init__(self, declared: int, available: int) -> None:
        super().__init__(f"len field {declared} does not match available payload {available}")
        self.declared = declared
        self.available = available


class FrameBadCrcError(FrameError):
    def __init__(self, expected: int, got: int) -> None:
        super().__init__(f"CRC mismatch: frame says {expected:#06x}, computed {got:#06x}")
        self.expected = expected
        self.got = got


@dataclass(frozen=True, slots=True)
class Frame:
    """A decoded ENLP frame body (ver is always WIRE_VERSION on construction
    for outbound frames; parse() preserves whatever byte was on the wire so
    callers can detect a version mismatch)."""

    type: int
    node: int
    seq: int
    payload: bytes
    ver: int = WIRE_VERSION

    def encode(self) -> bytes:
        """Build the raw frame body (header + payload + crc16), no framing."""
        return encode(self.type, self.node, self.seq, self.payload, ver=self.ver)

    def encode_cobs(self) -> bytes:
        """Build the serial-ready block: COBS(body) + 0x00 delimiter."""
        return encode_cobs(self.type, self.node, self.seq, self.payload, ver=self.ver)


def encode(type_: int, node: int, seq: int, payload: bytes, *, ver: int = WIRE_VERSION) -> bytes:
    """Build a raw ENLP frame body (no serial/TCP framing)."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload of {len(payload)} bytes exceeds MAX_PAYLOAD={MAX_PAYLOAD}")
    header = HEADER_STRUCT.pack(ver, type_, node & 0xFFFF, seq & 0xFFFF, len(payload))
    body = header + payload
    crc = crc16_ccitt_false(body)
    return body + struct.pack("<H", crc)


def encode_cobs(type_: int, node: int, seq: int, payload: bytes, *, ver: int = WIRE_VERSION) -> bytes:
    """Build the serial wire block: COBS-encoded body + 0x00 delimiter."""
    body = encode(type_, node, seq, payload, ver=ver)
    return cobs_encode(body) + b"\x00"


def parse(body: bytes) -> Frame:
    """Parse a raw (already de-COBS'd / de-length-prefixed) frame body.

    Raises FrameTooShortError, FrameBadVersionError, FrameBadLengthError or
    FrameBadCrcError -- distinct exceptions so callers (esp. the streaming
    decoder) can decide what's a resync-worthy garbage byte vs. a frame that
    parsed structurally but failed integrity.
    """
    if len(body) < HEADER_SIZE + CRC_SIZE:
        raise FrameTooShortError(f"body of {len(body)} bytes shorter than minimum {HEADER_SIZE + CRC_SIZE}")

    ver, type_, node, seq, length = HEADER_STRUCT.unpack_from(body, 0)
    if ver != WIRE_VERSION:
        raise FrameBadVersionError(ver)

    available = len(body) - HEADER_SIZE - CRC_SIZE
    if length != available:
        raise FrameBadLengthError(length, available)

    payload = body[HEADER_SIZE:HEADER_SIZE + length]
    (crc_on_wire,) = struct.unpack_from("<H", body, HEADER_SIZE + length)
    crc_computed = crc16_ccitt_false(body[:HEADER_SIZE + length])
    if crc_on_wire != crc_computed:
        raise FrameBadCrcError(crc_on_wire, crc_computed)

    return Frame(type=type_, node=node, seq=seq, payload=payload, ver=ver)


class StreamingDecoder:
    """Incremental COBS/0x00 frame decoder for the serial transport.

    PROTOCOL.md section 2.1 is explicit that bytes between delimiters that
    fail to COBS-decode or fail CRC are *not* an error -- they are the ESP32
    boot-ROM banner and any pre-link printf output, and must reach the UI as
    raw console text rather than being dropped. So this decoder never raises:
    feed() returns a list of events, each either ("frame", Frame) or
    ("raw", bytes), preserving arrival order.

    It is deliberately stateful and chunk-agnostic: pyserial hands back
    whatever the OS buffer happened to contain, which may split a frame
    across reads or pack several frames (plus banner noise) into one read.
    """

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> list[tuple[str, object]]:
        events: list[tuple[str, object]] = []
        self._buf.extend(chunk)
        while True:
            delim = self._buf.find(0x00)
            if delim == -1:
                break
            block = bytes(self._buf[:delim])
            del self._buf[: delim + 1]
            if not block:
                # A bare 0x00 with nothing before it (e.g. two delimiters in
                # a row). Not an error, just nothing to decode.
                continue
            events.append(self._decode_block(block))
        return events

    def flush(self) -> list[tuple[str, object]]:
        """Call on transport close: any undelimited tail is raw console
        output (no frame can be complete without its trailing 0x00)."""
        if not self._buf:
            return []
        tail = bytes(self._buf)
        self._buf.clear()
        return [("raw", tail)]

    @staticmethod
    def _decode_block(block: bytes) -> tuple[str, object]:
        try:
            body = cobs_decode(block)
        except CobsError:
            return ("raw", block)
        try:
            frame = parse(body)
        except FrameError:
            # Structurally COBS-valid but not a valid frame (e.g. plain text
            # that happened to contain no zero bytes over the wire). Surface
            # the original encoded bytes, not the decoded ones, since this
            # is meant to be human console output.
            return ("raw", block)
        return ("frame", frame)

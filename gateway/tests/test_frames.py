import pytest

from espstation_gateway.protocol import frames


def test_encode_parse_roundtrip():
    body = frames.encode(0x01, 4711, 42, b'{"hello":true}')
    frame = frames.parse(body)
    assert frame.type == 0x01
    assert frame.node == 4711
    assert frame.seq == 42
    assert frame.payload == b'{"hello":true}'
    assert frame.ver == frames.WIRE_VERSION


def test_encode_parse_empty_payload():
    body = frames.encode(0x03, 1, 0, b"")
    frame = frames.parse(body)
    assert frame.payload == b""


def test_too_short_raises():
    with pytest.raises(frames.FrameTooShortError):
        frames.parse(b"\x01\x02\x03")


def test_bad_version_raises():
    body = bytearray(frames.encode(0x01, 1, 1, b"x"))
    # Corrupt the ver byte but leave len/crc alone -- must still be caught
    # as a version mismatch, not a CRC failure (CRC is recomputed over the
    # actual bytes so it will differ, but ver is checked first).
    body[0] = 0x02
    with pytest.raises(frames.FrameBadVersionError):
        frames.parse(bytes(body))


def test_bad_length_raises():
    body = bytearray(frames.encode(0x01, 1, 1, b"hello"))
    # Claim a longer payload than actually present.
    body[6] = 0xFF
    with pytest.raises((frames.FrameBadLengthError,)):
        frames.parse(bytes(body))


def test_bad_crc_raises():
    body = bytearray(frames.encode(0x01, 1, 1, b"hello"))
    body[-1] ^= 0xFF
    with pytest.raises(frames.FrameBadCrcError):
        frames.parse(bytes(body))


def test_payload_too_large_rejected_on_encode():
    with pytest.raises(ValueError):
        frames.encode(0x10, 1, 1, bytes(frames.MAX_PAYLOAD + 1))


def test_frame_dataclass_encode_matches_module_function():
    f = frames.Frame(type=0x20, node=99, seq=5, payload=b"abc")
    assert f.encode() == frames.encode(0x20, 99, 5, b"abc")
    assert f.encode_cobs() == frames.encode_cobs(0x20, 99, 5, b"abc")


class TestStreamingDecoder:
    def test_single_frame_one_chunk(self):
        dec = frames.StreamingDecoder()
        wire = frames.encode_cobs(0x01, 1, 1, b"hi")
        events = dec.feed(wire)
        assert len(events) == 1
        kind, frame = events[0]
        assert kind == "frame"
        assert frame.payload == b"hi"

    def test_frame_split_across_chunks(self):
        dec = frames.StreamingDecoder()
        wire = frames.encode_cobs(0x01, 1, 1, b"split-me")
        mid = len(wire) // 2
        assert dec.feed(wire[:mid]) == []
        events = dec.feed(wire[mid:])
        assert len(events) == 1
        assert events[0][0] == "frame"
        assert events[0][1].payload == b"split-me"

    def test_multiple_frames_one_chunk(self):
        dec = frames.StreamingDecoder()
        wire = frames.encode_cobs(0x01, 1, 1, b"one") + frames.encode_cobs(0x01, 1, 2, b"two")
        events = dec.feed(wire)
        assert [e[1].payload for e in events] == [b"one", b"two"]

    def test_boot_rom_banner_surfaces_as_raw(self):
        # PROTOCOL.md section 2.1: text with no frame structure at all
        # (e.g. the ESP32 boot ROM banner) must reach the UI as raw console
        # output, not be dropped or raise.
        dec = frames.StreamingDecoder()
        banner = b"ESP-ROM:esp32-arduino\r\nBuild:date\r\n"
        events = dec.feed(banner + b"\x00")
        assert len(events) == 1
        assert events[0][0] == "raw"
        assert events[0][1] == banner

    def test_interleaved_garbage_and_frames(self):
        dec = frames.StreamingDecoder()
        garbage = b"boot noise here"
        wire = frames.encode_cobs(0x01, 1, 1, b"payload-a")
        stream = garbage + b"\x00" + wire + garbage + b"\x00"
        events = dec.feed(stream)
        kinds = [k for k, _ in events]
        assert kinds == ["raw", "frame", "raw"]
        assert events[1][1].payload == b"payload-a"

    def test_corrupted_frame_crc_surfaces_as_raw_not_exception(self):
        dec = frames.StreamingDecoder()
        body = bytearray(frames.encode(0x01, 1, 1, b"hello"))
        body[-1] ^= 0xFF  # corrupt CRC
        wire = __import__("espstation_gateway.protocol.cobs", fromlist=["encode"]).encode(bytes(body)) + b"\x00"
        events = dec.feed(wire)
        assert len(events) == 1
        assert events[0][0] == "raw"

    def test_bare_delimiter_produces_nothing(self):
        dec = frames.StreamingDecoder()
        assert dec.feed(b"\x00\x00\x00") == []

    def test_flush_returns_undelimited_tail_as_raw(self):
        dec = frames.StreamingDecoder()
        dec.feed(b"partial-frame-no-delimiter")
        events = dec.flush()
        assert events == [("raw", b"partial-frame-no-delimiter")]

    def test_flush_empty_buffer_returns_nothing(self):
        dec = frames.StreamingDecoder()
        assert dec.flush() == []

    def test_byte_by_byte_feed(self):
        dec = frames.StreamingDecoder()
        wire = frames.encode_cobs(0x01, 7, 3, b"trickle")
        events = []
        for i in range(len(wire)):
            events.extend(dec.feed(wire[i:i + 1]))
        assert len(events) == 1
        assert events[0][1].payload == b"trickle"

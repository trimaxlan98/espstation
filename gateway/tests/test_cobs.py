import pytest

from espstation_gateway.protocol import cobs


def roundtrip(data: bytes) -> None:
    encoded = cobs.encode(data)
    assert b"\x00" not in encoded
    assert cobs.decode(encoded) == data


def test_empty():
    roundtrip(b"")


def test_no_zeros():
    roundtrip(b"hello world")


def test_all_zeros():
    roundtrip(bytes(10))


def test_single_zero():
    roundtrip(b"\x00")


def test_leading_and_trailing_zero():
    roundtrip(b"\x00abc\x00")


def test_254_byte_block_no_zero():
    # Exactly the length-code boundary: a 254-byte zero-free run fits in one
    # code-255 block with no implicit trailing zero.
    roundtrip(bytes(range(1, 255)))  # 254 nonzero bytes


def test_255_byte_block_no_zero():
    # One byte over the boundary: must split into a 254-byte block plus a
    # 1-byte block.
    data = bytes((i % 255) + 1 for i in range(255))
    assert 0 not in data
    roundtrip(data)


def test_many_interleaved_zeros():
    data = bytes([1, 2, 0, 0, 3, 0, 4, 5, 6, 0])
    roundtrip(data)


def test_large_random_with_zeros():
    import random
    rng = random.Random(1234)
    data = bytes(rng.randint(0, 255) for _ in range(2000))
    roundtrip(data)


def test_decode_rejects_empty_block():
    with pytest.raises(cobs.CobsError):
        cobs.decode(b"")


def test_decode_rejects_embedded_zero_as_length_code():
    with pytest.raises(cobs.CobsError):
        cobs.decode(b"\x00")


def test_decode_rejects_overrun_length_code():
    # Length code claims more bytes than are actually present.
    with pytest.raises(cobs.CobsError):
        cobs.decode(b"\x05ab")

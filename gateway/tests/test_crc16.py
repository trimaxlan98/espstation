from espstation_gateway.protocol.crc16 import crc16_ccitt_false


def test_check_string_vector():
    # The canonical CRC-16/CCITT-FALSE check value, also asserted at import
    # time in crc16.py itself.
    assert crc16_ccitt_false(b"123456789") == 0x29B1


def test_empty_input_is_init_value():
    assert crc16_ccitt_false(b"") == 0xFFFF


def test_different_inputs_differ():
    assert crc16_ccitt_false(b"hello") != crc16_ccitt_false(b"hellp")


def test_seeded_crc_matches_concatenation():
    # crc(a+b) == crc(b, seed=crc(a)) -- used by callers who checksum a
    # header and payload without concatenating them first.
    a, b = b"header--", b"payload data here"
    whole = crc16_ccitt_false(a + b)
    seeded = crc16_ccitt_false(b, crc16_ccitt_false(a))
    assert whole == seeded


def test_all_zeros_block():
    # Not a degenerate case for a poly-based CRC, but worth pinning: all
    # zero bytes still produce a real, deterministic checksum.
    assert crc16_ccitt_false(bytes(32)) == crc16_ccitt_false(bytes(32))
    assert crc16_ccitt_false(bytes(32)) != 0

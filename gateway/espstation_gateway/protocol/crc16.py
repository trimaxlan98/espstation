# CRC-16/CCITT-FALSE, per PROTOCOL.md section 3 and espstation.protocol.yaml
# `crc:` block: poly 0x1021, init 0xFFFF, no input/output reflection, no
# final xor. This is the "false" CCITT variant (distinct from CRC-16/XMODEM,
# which shares the poly but not the init value) -- the test vector below is
# the standard way to tell them apart.
from __future__ import annotations

_POLY = 0x1021
_INIT = 0xFFFF
_MASK = 0xFFFF

# Precomputed byte-wise table (Sarwate's method) so hot paths (framing every
# outbound message) don't recompute 8 shifts per byte.
def _build_table() -> tuple[int, ...]:
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ _POLY) & _MASK
            else:
                crc = (crc << 1) & _MASK
        table.append(crc)
    return tuple(table)


_TABLE = _build_table()


def crc16_ccitt_false(data: bytes, crc: int = _INIT) -> int:
    """Compute CRC-16/CCITT-FALSE over `data`.

    `crc` may be seeded to continue a running checksum across chunks (used
    when framing header+payload without concatenating them first).
    """
    for byte in data:
        crc = ((crc << 8) & _MASK) ^ _TABLE[((crc >> 8) ^ byte) & 0xFF]
    return crc & _MASK


# Frozen self-check: PROTOCOL.md doesn't spell out a test vector, but this is
# the canonical CRC-16/CCITT-FALSE check string used by every reference
# implementation of this variant. Importing this module re-validates it so a
# future edit to the table/poly can't silently break wire compatibility with
# the firmware.
_CHECK_STRING = b"123456789"
_CHECK_VALUE = 0x29B1
assert crc16_ccitt_false(_CHECK_STRING) == _CHECK_VALUE, "CRC-16/CCITT-FALSE table is broken"

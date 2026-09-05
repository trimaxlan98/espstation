# Consistent Overhead Byte Stuffing (COBS), per PROTOCOL.md section 2.1.
# Used only on the serial transport: the encoded body contains no 0x00 byte,
# so a single 0x00 delimiter is unambiguous even mid-stream, letting a
# receiver that joins in the middle resynchronise at the next delimiter.
from __future__ import annotations


class CobsError(ValueError):
    """Malformed COBS-encoded input (bad length code or truncated block)."""


def encode(data: bytes) -> bytes:
    """COBS-encode `data`. Output never contains 0x00; caller appends the
    delimiter separately (see frames.encode_cobs)."""
    if not data:
        # The empty message still needs a valid COBS body: a single length
        # byte of 1 (meaning "no data, no following zero-free run").
        return b"\x01"

    out = bytearray()
    idx = 0
    n = len(data)
    while True:
        # Find the next zero byte (or end of buffer), capping the run at 254
        # data bytes because the length code itself is 1..255.
        block_start = idx
        zero_pos = data.find(b"\x00", idx)
        end = zero_pos if zero_pos != -1 else n
        if end - block_start > 254:
            end = block_start + 254

        code = (end - block_start) + 1
        out.append(code)
        out.extend(data[block_start:end])
        idx = end

        if idx < n and data[idx] == 0:
            idx += 1
            if idx == n:
                # Trailing zero byte: emit a final empty block so the
                # decoder knows a zero terminated the data, not the stream.
                out.append(1)
                break
            continue
        if idx >= n:
            break
    return bytes(out)


def decode(data: bytes) -> bytes:
    """Decode a COBS block (no trailing delimiter). Raises CobsError on any
    structural violation -- callers must not trust corrupted frames."""
    if not data:
        raise CobsError("empty COBS block")

    out = bytearray()
    idx = 0
    n = len(data)
    while idx < n:
        code = data[idx]
        if code == 0:
            raise CobsError("zero byte inside COBS block")
        idx += 1
        run_end = idx + code - 1
        if run_end > n:
            raise CobsError("COBS length code overruns block")
        out.extend(data[idx:run_end])
        idx = run_end
        if code < 255 and idx < n:
            out.append(0)
    return bytes(out)

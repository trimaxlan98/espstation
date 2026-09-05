/* COBS encode/decode. See esps_cobs.h for the framing rationale.
 *
 * Every write to dst is bounds-checked before it happens (not after), so a
 * hostile or corrupt input can only ever return ESPS_COBS_ERR_CAPACITY /
 * ESPS_COBS_ERR_MALFORMED, never overrun the caller's buffer — this file has
 * no ESP-IDF underneath it to catch that with an MPU fault, and on the wire
 * side this is exactly the boundary PROTOCOL.md S2.1 expects to fail safely
 * (malformed bytes become raw console output upstream, not a crash here).
 */
#include "esps_cobs.h"

esps_cobs_err_t esps_cobs_encode(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap, size_t *out_len) {
    if (dst_cap < 1) {
        return ESPS_COBS_ERR_CAPACITY;
    }

    size_t read_index = 0;
    size_t write_index = 1;
    size_t code_index = 0;
    uint8_t code = 1;

    while (read_index < src_len) {
        uint8_t byte = src[read_index];
        if (byte == 0) {
            dst[code_index] = code;
            code = 1;
            code_index = write_index;
            if (code_index >= dst_cap) {
                return ESPS_COBS_ERR_CAPACITY;
            }
            write_index++;
            read_index++;
        } else {
            if (write_index >= dst_cap) {
                return ESPS_COBS_ERR_CAPACITY;
            }
            dst[write_index++] = byte;
            code++;
            read_index++;
            if (code == 0xFF) {
                dst[code_index] = code;
                code = 1;
                code_index = write_index;
                if (code_index >= dst_cap) {
                    return ESPS_COBS_ERR_CAPACITY;
                }
                write_index++;
            }
        }
    }

    /* code_index was validated < dst_cap at the point it was set (or is the
     * initial 0, validated by the dst_cap < 1 check above). */
    dst[code_index] = code;
    *out_len = write_index;
    return ESPS_COBS_OK;
}

esps_cobs_err_t esps_cobs_decode(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap, size_t *out_len) {
    size_t read_index = 0;
    size_t write_index = 0;

    while (read_index < src_len) {
        uint8_t code = src[read_index];
        if (code == 0) {
            /* 0x00 never appears inside a COBS-encoded block; its presence
             * here means this isn't valid COBS (e.g. arbitrary text that
             * happened to land between two delimiters). */
            return ESPS_COBS_ERR_MALFORMED;
        }
        size_t block_len = (size_t)(code - 1);
        read_index++;

        if (read_index + block_len > src_len) {
            return ESPS_COBS_ERR_MALFORMED;
        }
        if (write_index + block_len > dst_cap) {
            return ESPS_COBS_ERR_CAPACITY;
        }
        for (size_t i = 0; i < block_len; i++) {
            dst[write_index + i] = src[read_index + i];
        }
        write_index += block_len;
        read_index += block_len;

        /* A code < 0xFF marks a block that was terminated by a real zero
         * byte in the original data — restore it, unless this is the final
         * block of the frame (no more bytes follow), whose implicit zero is
         * the delimiter itself, never part of the payload. */
        if (code != 0xFF && read_index < src_len) {
            if (write_index >= dst_cap) {
                return ESPS_COBS_ERR_CAPACITY;
            }
            dst[write_index++] = 0;
        }
    }

    *out_len = write_index;
    return ESPS_COBS_OK;
}

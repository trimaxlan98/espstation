/* Consistent Overhead Byte Stuffing (COBS) — removes 0x00 from an arbitrary
 * byte string so 0x00 can be used as an unambiguous serial frame delimiter
 * (PROTOCOL.md S2.1). Overhead is at most 1 byte per 254, which is why the
 * gateway can afford to resynchronise on any 0x00 without a length prefix.
 *
 * Pure C11, no ESP-IDF dependency (host-testable, see test/host/).
 * Every function takes an explicit destination capacity and returns an error
 * rather than trusting the caller sized the buffer correctly — malformed
 * input (a code byte that claims more bytes than remain) must fail cleanly,
 * never read or write past the given buffer.
 */
#ifndef ESPS_COBS_H
#define ESPS_COBS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPS_COBS_OK = 0,
    ESPS_COBS_ERR_CAPACITY,  /* destination buffer too small */
    ESPS_COBS_ERR_MALFORMED, /* a code byte would read past the source */
} esps_cobs_err_t;

/* Worst case: one overhead byte per 254 source bytes, plus the leading code
 * byte of the frame. Always safe to over-allocate by this amount; it is not
 * the exact size for every input. */
#define ESPS_COBS_MAX_ENCODED(n) ((n) + (n) / 254u + 1u)

/* Encodes src[0..src_len) into dst. Does NOT append the trailing 0x00
 * delimiter — that is a framing concern layered on top (see esps_enlp.h),
 * because a raw COBS block is also useful un-delimited (e.g. concatenated
 * fields). On success *out_len holds the encoded length and dst contains no
 * 0x00 bytes. */
esps_cobs_err_t esps_cobs_encode(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap, size_t *out_len);

/* Decodes a COBS block (no delimiter expected in src) into dst. Rejects
 * src containing a 0x00 (that would mean the delimiter was included in the
 * block by mistake) and any code byte whose implied block runs past src_len. */
esps_cobs_err_t esps_cobs_decode(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_COBS_H */

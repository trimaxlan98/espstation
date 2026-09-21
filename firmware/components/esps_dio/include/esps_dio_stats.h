/* Link accounting for the two-wire digital link (SPEC-LINK.md "Contabilidad").
 * Pure counters over completed frames; the ESP-IDF layer publishes them as
 * the NDB channels link.frames_ok / link.frames_err / link.ber.
 */
#ifndef ESPS_DIO_STATS_H
#define ESPS_DIO_STATS_H

#include "esps_dio_frame.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All counters are uint32 and wrap silently; at the link's frame rates that
 * is far beyond any experiment's duration. Zero-fill (or `= {0}`) to reset. */
typedef struct esps_dio_link_stats {
    uint32_t bits_rx;         /* LEN+PAYLOAD+CRC bits of completed frames (ok or crc_err) */
    uint32_t bit_errors;      /* differing payload bits vs. the expected payload          */
    uint32_t frames_ok;
    uint32_t frames_crc_err;
    uint32_t frames_len_err;
} esps_dio_link_stats_t;

/* Accounts one receiver result. `got` is the received payload (rx->payload),
 * `expected` the payload the sender should have sent, `len` the received
 * length. Both buffers are compared over `len` bytes, so `expected` must hold
 * at least `len` bytes (esps_dio_testframe_payload() zero-fills its 32-byte
 * buffer past the frame length for exactly this reason: a frame received with
 * the wrong length is compared, deterministically, against that padding).
 *
 *  - NONE: ignored.
 *  - LEN_ERR: frames_len_err++ only; got/expected/len are not read.
 *  - OK / CRC_ERR: frames_ok / frames_crc_err++, bits_rx += 8*(len+2), and
 *    bit_errors += popcount(got ^ expected). A frame whose CRC is correct but
 *    whose payload differs from the expected one still adds to bit_errors —
 *    that is a CRC collision and it must be visible.
 *
 * Defensive: a NULL stats, or (for OK/CRC_ERR) a len outside 1..32, is a
 * no-op; NULL got/expected count the frame but add no bit errors. */
void esps_dio_stats_account(esps_dio_link_stats_t *stats, esps_dio_rx_result_t result,
                            const uint8_t *got, const uint8_t *expected, size_t len);

/* bit_errors / bits_rx, or 0.0f when bits_rx == 0 (or stats is NULL). */
float esps_dio_stats_ber(const esps_dio_link_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_DIO_STATS_H */

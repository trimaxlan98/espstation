#include "esps_dio_stats.h"

/* Portable popcount: __builtin_popcount is fine on gcc but this file must not
 * depend on the toolchain, and 8 bits is a handful of operations. */
static uint32_t popcount8(uint8_t v) {
    v = (uint8_t)(v - ((v >> 1) & 0x55u));
    v = (uint8_t)((v & 0x33u) + ((v >> 2) & 0x33u));
    return (uint32_t)((v + (v >> 4)) & 0x0Fu);
}

void esps_dio_stats_account(esps_dio_link_stats_t *stats, esps_dio_rx_result_t result,
                            const uint8_t *got, const uint8_t *expected, size_t len) {
    if (stats == NULL) {
        return;
    }
    switch (result) {
    case ESPS_DIO_RX_FRAME_LEN_ERR:
        stats->frames_len_err++;
        return;
    case ESPS_DIO_RX_FRAME_OK:
    case ESPS_DIO_RX_FRAME_CRC_ERR:
        break;
    default:
        return;
    }
    if (len < 1u || len > ESPS_DIO_MAX_PAYLOAD) {
        return;
    }
    if (result == ESPS_DIO_RX_FRAME_OK) {
        stats->frames_ok++;
    } else {
        stats->frames_crc_err++;
    }
    stats->bits_rx += (uint32_t)(8u * (len + 2u));
    if (got != NULL && expected != NULL) {
        for (size_t i = 0; i < len; i++) {
            stats->bit_errors += popcount8((uint8_t)(got[i] ^ expected[i]));
        }
    }
}

float esps_dio_stats_ber(const esps_dio_link_stats_t *stats) {
    if (stats == NULL || stats->bits_rx == 0u) {
        return 0.0f;
    }
    return (float)stats->bit_errors / (float)stats->bits_rx;
}

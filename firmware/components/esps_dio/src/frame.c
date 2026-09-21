#include "esps_dio_frame.h"

#include "esps_dio_crc8.h"

#include <string.h>

bool esps_dio_frame_build(const uint8_t *payload, size_t len,
                          uint8_t *out, size_t out_cap, size_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (payload == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    if (len < 1u || len > ESPS_DIO_MAX_PAYLOAD || out_cap < len + 3u) {
        return false;
    }
    out[0] = (uint8_t)ESPS_DIO_SYNC;
    out[1] = (uint8_t)len;
    memcpy(&out[2], payload, len);
    out[2 + len] = esps_dio_crc8(&out[1], len + 1u, ESPS_DIO_CRC8_INIT);
    *out_len = len + 3u;
    return true;
}

void esps_dio_rx_init(esps_dio_rx_t *rx) {
    if (rx == NULL) {
        return;
    }
    memset(rx, 0, sizeof(*rx));
    rx->state = ESPS_DIO_RXS_HUNT;
}

esps_dio_rx_result_t esps_dio_rx_push_bit(esps_dio_rx_t *rx, uint8_t bit) {
    if (rx == NULL) {
        return ESPS_DIO_RX_NONE;
    }
    const uint8_t b = (bit != 0u) ? 1u : 0u;

    if (rx->state == ESPS_DIO_RXS_HUNT) {
        rx->acc = (uint8_t)((rx->acc << 1) | b);
        if (rx->acc == ESPS_DIO_SYNC) {
            rx->state = ESPS_DIO_RXS_LEN;
            rx->acc = 0;
            rx->nbits = 0;
        }
        return ESPS_DIO_RX_NONE;
    }

    rx->acc = (uint8_t)((rx->acc << 1) | b);
    rx->nbits++;
    if (rx->nbits < 8u) {
        return ESPS_DIO_RX_NONE;
    }
    const uint8_t byte = rx->acc;
    /* Clearing here, before any state change, is what guarantees HUNT always
     * starts with an empty shift register: otherwise the last bits of a
     * finished frame (or of a rejected LEN) could count toward the next 0xAA
     * and false-sync early on a back-to-back frame. */
    rx->acc = 0;
    rx->nbits = 0;

    switch (rx->state) {
    case ESPS_DIO_RXS_LEN:
        if (byte < 1u || byte > ESPS_DIO_MAX_PAYLOAD) {
            rx->len = 0;
            rx->state = ESPS_DIO_RXS_HUNT;
            return ESPS_DIO_RX_FRAME_LEN_ERR;
        }
        rx->len = byte;
        rx->idx = 0;
        /* The CRC covers LEN || PAYLOAD, so it is folded in byte by byte to
         * keep the per-bit ISR cost constant. */
        rx->crc = esps_dio_crc8(&byte, 1, ESPS_DIO_CRC8_INIT);
        rx->state = ESPS_DIO_RXS_PAYLOAD;
        return ESPS_DIO_RX_NONE;

    case ESPS_DIO_RXS_PAYLOAD:
        rx->payload[rx->idx] = byte;
        rx->idx++;
        rx->crc = esps_dio_crc8(&byte, 1, rx->crc);
        if (rx->idx >= rx->len) {
            rx->state = ESPS_DIO_RXS_CRC;
        }
        return ESPS_DIO_RX_NONE;

    case ESPS_DIO_RXS_CRC: {
        const bool ok = (byte == rx->crc);
        rx->state = ESPS_DIO_RXS_HUNT;
        return ok ? ESPS_DIO_RX_FRAME_OK : ESPS_DIO_RX_FRAME_CRC_ERR;
    }

    default:
        /* Unreachable for a struct only this file writes; recover rather than
         * stay wedged if the caller's memory was corrupted. */
        rx->state = ESPS_DIO_RXS_HUNT;
        return ESPS_DIO_RX_NONE;
    }
}

void esps_dio_txbits_init(esps_dio_txbits_t *tx, const uint8_t *bytes, size_t len) {
    if (tx == NULL) {
        return;
    }
    tx->bytes = bytes;
    tx->len = (bytes != NULL) ? len : 0;
    tx->idx = 0;
    tx->shift = 0;
}

bool esps_dio_txbits_next(esps_dio_txbits_t *tx, uint8_t *bit) {
    if (tx == NULL || bit == NULL || tx->bytes == NULL || tx->idx >= tx->len) {
        return false;
    }
    *bit = (uint8_t)((tx->bytes[tx->idx] >> (7u - tx->shift)) & 1u);
    tx->shift++;
    if (tx->shift == 8u) {
        tx->shift = 0;
        tx->idx++;
    }
    return true;
}

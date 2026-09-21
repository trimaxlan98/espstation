/* Shared helpers for the esps_dio host tests: hex decoding for the golden
 * vectors and a bit-by-bit feed that goes through the same txbits iterator
 * the firmware uses, so the tests exercise the real bit order. */
#ifndef ESPS_TEST_DIO_HELPERS_H
#define ESPS_TEST_DIO_HELPERS_H

#include "esps_dio_frame.h"

#include <stddef.h>
#include <stdint.h>

static inline int dio_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decodes lowercase hex into out; returns the byte count (0 on bad input). */
static inline size_t dio_unhex(const char *hex, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (; hex[0] != '\0' && hex[1] != '\0'; hex += 2) {
        int hi = dio_hexval(hex[0]);
        int lo = dio_hexval(hex[1]);
        if (hi < 0 || lo < 0 || n >= cap) return 0;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

/* Feeds `n` bytes into rx MSB first and returns the first non-NONE result
 * seen (NONE if the frame never completed). Stops at that result, so a
 * following frame is left unread — dio_feed_all() below keeps going. */
static inline esps_dio_rx_result_t dio_feed(esps_dio_rx_t *rx, const uint8_t *bytes, size_t n) {
    esps_dio_txbits_t tx;
    esps_dio_txbits_init(&tx, bytes, n);
    uint8_t bit;
    while (esps_dio_txbits_next(&tx, &bit)) {
        esps_dio_rx_result_t r = esps_dio_rx_push_bit(rx, bit);
        if (r != ESPS_DIO_RX_NONE) return r;
    }
    return ESPS_DIO_RX_NONE;
}

/* Feeds every bit and appends each non-NONE result to results[] (up to cap).
 * Returns how many non-NONE results occurred in total. */
static inline size_t dio_feed_all(esps_dio_rx_t *rx, const uint8_t *bytes, size_t n,
                                  esps_dio_rx_result_t *results, size_t cap) {
    esps_dio_txbits_t tx;
    esps_dio_txbits_init(&tx, bytes, n);
    uint8_t bit;
    size_t count = 0;
    while (esps_dio_txbits_next(&tx, &bit)) {
        esps_dio_rx_result_t r = esps_dio_rx_push_bit(rx, bit);
        if (r != ESPS_DIO_RX_NONE) {
            if (count < cap) results[count] = r;
            count++;
        }
    }
    return count;
}

#endif /* ESPS_TEST_DIO_HELPERS_H */

/* Frame builder, bit-serial receiver and bit-order iterator for the two-wire
 * digital link (bench/practicas/enlace-digital/SPEC-LINK.md):
 *
 *     0xAA | LEN (1 B, 1..32) | PAYLOAD (LEN B) | CRC8(LEN || PAYLOAD)
 *
 * MSB first. Everything here is pure logic over caller-owned buffers and
 * structs: no allocation, no globals, no ESP-IDF. esps_dio_rx_push_bit() is
 * written to be called from an ISR (constant work per bit, no blocking, no
 * libc); the ESP-IDF layer decides where it lives (IRAM etc.).
 */
#ifndef ESPS_DIO_FRAME_H
#define ESPS_DIO_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPS_DIO_SYNC        0xAAu
#define ESPS_DIO_MAX_PAYLOAD 32u
/* Sync + LEN + max payload + CRC: the largest encoded frame. */
#define ESPS_DIO_FRAME_MAX   (ESPS_DIO_MAX_PAYLOAD + 3u)

/* Encodes payload[0..len) into out as SYNC|LEN|PAYLOAD|CRC and sets
 * *out_len = len + 3. Returns false — writing nothing to out and setting
 * *out_len to 0 when it is non-NULL — if any pointer is NULL, len is outside
 * 1..32, or out_cap < len + 3. */
bool esps_dio_frame_build(const uint8_t *payload, size_t len,
                          uint8_t *out, size_t out_cap, size_t *out_len);

/* ---- Receiver -------------------------------------------------------- */

typedef enum {
    ESPS_DIO_RX_NONE = 0,       /* bit consumed, no frame boundary reached   */
    ESPS_DIO_RX_FRAME_OK,       /* frame complete, CRC matches               */
    ESPS_DIO_RX_FRAME_CRC_ERR,  /* frame complete, CRC mismatch              */
    ESPS_DIO_RX_FRAME_LEN_ERR,  /* LEN was 0 or > 32; frame discarded        */
} esps_dio_rx_result_t;

typedef enum {
    ESPS_DIO_RXS_HUNT = 0,      /* shifting bits until 0xAA                  */
    ESPS_DIO_RXS_LEN,
    ESPS_DIO_RXS_PAYLOAD,
    ESPS_DIO_RXS_CRC,
} esps_dio_rx_state_t;

/* Receiver state. The caller owns it (typically one per link, static in the
 * ESP-IDF layer). Fields the caller may read are marked; the rest is private.
 * The zero-filled struct is a valid HUNT state, but use esps_dio_rx_init(). */
typedef struct esps_dio_rx {
    uint8_t payload[ESPS_DIO_MAX_PAYLOAD]; /* last completed frame's payload  */
    uint8_t len;                           /* its length; 0 after a LEN_ERR   */
    esps_dio_rx_state_t state;
    uint8_t acc;    /* HUNT: 8-bit shift register; other states: byte in flight */
    uint8_t nbits;  /* bits accumulated in acc (not used in HUNT)               */
    uint8_t idx;    /* payload bytes stored so far                              */
    uint8_t crc;    /* running CRC over LEN || PAYLOAD                          */
} esps_dio_rx_t;

void esps_dio_rx_init(esps_dio_rx_t *rx);

/* Feeds one bit (0 or non-zero = 1), in wire order. Returns FRAME_OK or
 * FRAME_CRC_ERR when the CRC byte completes — rx->len and rx->payload then
 * hold the frame and stay valid until the next call — or FRAME_LEN_ERR as soon
 * as an invalid LEN is read (rx->len is 0). After any frame boundary the
 * receiver is back in HUNT with its shift register cleared, so bits of the
 * finished frame can never seed the next sync. A NULL rx returns NONE. */
esps_dio_rx_result_t esps_dio_rx_push_bit(esps_dio_rx_t *rx, uint8_t bit);

/* ---- Transmitter bit order ------------------------------------------- */

/* Walks a byte buffer MSB first so the ESP-IDF layer never re-implements the
 * bit order. Holds a pointer to the caller's bytes; they must outlive it. */
typedef struct esps_dio_txbits {
    const uint8_t *bytes;
    size_t len;
    size_t idx;    /* current byte */
    uint8_t shift; /* bits already emitted from bytes[idx] */
} esps_dio_txbits_t;

/* A NULL bytes pointer yields an empty sequence. */
void esps_dio_txbits_init(esps_dio_txbits_t *tx, const uint8_t *bytes, size_t len);

/* Stores the next bit (0 or 1) in *bit and returns true; returns false once
 * all len*8 bits were produced (or if tx/bit is NULL). */
bool esps_dio_txbits_next(esps_dio_txbits_t *tx, uint8_t *bit);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_DIO_FRAME_H */

/* UART implementation of esps_link_if_t — the S0/S1 transport. UART0 is
 * deliberately reused rather than adding a second UART: it is the same wire
 * as the boot ROM / USB-serial bridge on every dev board this targets, which
 * is exactly why PROTOCOL.md S2.1 designed COBS resync around sharing it
 * (boot text arrives raw, everything after link-up arrives framed).
 */
#ifndef ESPS_LINK_UART_H
#define ESPS_LINK_UART_H

#include "esps_link.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One outbound frame's worth of storage in the TX queue: the full COBS-encoded
 * wire form (header+payload+crc, then COBS overhead, then the delimiter), not
 * just the payload. HELLO with this firmware's 3-channel system NDB already
 * runs ~625 B on the wire, so 900 B is the floor, not a round number picked
 * for looks; a HELLO with a much larger NDB (many sensor channels) could
 * still exceed it — splitting oversized control messages is TODO(S6)
 * alongside BULK_* framing, not solved here by growing this indefinitely. */
#define ESPS_LINK_UART_MAX_FRAME 900u

typedef struct {
    int uart_num;
    int baud_rate;
    int tx_pin; /* -1 = UART_PIN_NO_CHANGE, i.e. use the default IO_MUX pin */
    int rx_pin; /* -1 = UART_PIN_NO_CHANGE */
    size_t tx_queue_depth; /* frames, not bytes — bounded per PROTOCOL.md S5.1 */
    size_t rx_buf_size;    /* bytes, the UART driver's internal RX ring buffer */
} esps_link_uart_config_t;

#define ESPS_LINK_UART_CONFIG_DEFAULT()                                                          \
    (esps_link_uart_config_t) {                                                                  \
        .uart_num = 0, .baud_rate = 115200, .tx_pin = -1, .rx_pin = -1, .tx_queue_depth = 16,     \
        .rx_buf_size = 2048,                                                                      \
    }

/* Fills `out` with a UART-backed link. Call out->open(...) to bring it up. */
void esps_link_uart_init(esps_link_if_t *out, const esps_link_uart_config_t *config);

/* Frames dropped because the TX queue was full — exposed so main.c can fold
 * it into the HEARTBEAT buffered-pending flag rather than it being an
 * invisible counter nobody sees until the link looks mysteriously lossy. */
uint32_t esps_link_uart_dropped_count(const esps_link_if_t *link);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_LINK_UART_H */

/* TCP/WebSocket transport — TODO(S6). PROTOCOL.md S2.2 (u16 big-endian
 * length prefix, WS as binary frames carrying the same prefixed body) is
 * already specified; this header exists now so esps_link consumers (main.c,
 * a future multi-transport manager) can be written against the final shape
 * before the implementation lands, instead of that shape being guessed at
 * in S6 under pressure to match whatever main.c happened to assume.
 */
#ifndef ESPS_LINK_TCP_H
#define ESPS_LINK_TCP_H

#include "esps_link.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t port;
    size_t tx_queue_depth;
} esps_link_tcp_config_t;

/* TODO(S6): implement. Fills `out` with a TCP-backed link whose open()
 * starts a listen/accept (or connect, depending on role) task and whose
 * send()/poll() honour the same non-blocking, bounded-queue contract as
 * esps_link_uart. Currently always returns a link whose open() fails
 * immediately, so callers can wire this in ahead of time without it doing
 * anything unsafe. */
void esps_link_tcp_init(esps_link_if_t *out, const esps_link_tcp_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_LINK_TCP_H */

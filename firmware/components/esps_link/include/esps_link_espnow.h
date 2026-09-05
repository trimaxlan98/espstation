/* ESP-NOW transport — TODO(S6). PROTOCOL.md S2.3/S3: raw frame body placed
 * directly in the ESP-NOW payload, MAX_PAYLOAD 240 (not 1024) because the
 * 250 B radio payload has to hold the 10 B header+CRC too. That budget is
 * an esps_link_espnow concern (it must reject/refuse to send a frame that
 * doesn't fit), not something esps_proto enforces generically.
 */
#ifndef ESPS_LINK_ESPNOW_H
#define ESPS_LINK_ESPNOW_H

#include "esps_link.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPS_LINK_ESPNOW_MAX_PAYLOAD 240u

typedef struct {
    uint8_t channel;
    size_t tx_queue_depth;
} esps_link_espnow_config_t;

/* TODO(S6): implement. Will own peer registration for esps_net's peer
 * table — open() brings up the ESP-NOW driver and this node's own peer
 * entry; adding remote peers is a separate esps_net-level API, not part of
 * this transport's open(). Currently always returns a link whose open()
 * fails immediately (see esps_link_tcp.c for why that's the honest stub
 * behaviour rather than a fake success path). */
void esps_link_espnow_init(esps_link_if_t *out, const esps_link_espnow_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_LINK_ESPNOW_H */

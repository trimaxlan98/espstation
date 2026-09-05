/* Node state machine (docs/ARCHITECTURE.md: boot -> idle -> running ->
 * degraded -> safe) and the HEARTBEAT payload builder (PROTOCOL.md S4.3).
 * State transitions themselves are driven by esps_experiment (not built
 * this sprint) and main.c; this module just holds the current value and
 * knows how to render a heartbeat from it plus live ESP-IDF vitals.
 */
#ifndef ESPS_HEALTH_H
#define ESPS_HEALTH_H

#include "esps_enlp.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPS_NODE_STATE_BOOT = 0,
    ESPS_NODE_STATE_IDLE = 1,
    ESPS_NODE_STATE_RUNNING = 2,
    ESPS_NODE_STATE_DEGRADED = 3,
    ESPS_NODE_STATE_SAFE = 4,
} esps_node_state_t;

/* Call once, early in boot (after esp_reset_reason() is meaningful, i.e.
 * any time after the bootloader handed off — so effectively first thing in
 * app_main). Latches brownout/watchdog flags for the lifetime of this boot;
 * PROTOCOL.md's HEARTBEAT.flags are "since boot", not "currently active",
 * so once latched they stay set until the next reboot. */
void esps_health_init(void);

void esps_health_set_state(esps_node_state_t state);
esps_node_state_t esps_health_get_state(void);

/* Set by the link layer when it notices the connection drop/return; not
 * latched — this reflects current status, per PROTOCOL.md S4.3's flag being
 * about the frame it rides in ("bit1 link-was-lost" is examined by the
 * station on receipt of the next heartbeat after a loss, not a sticky
 * lifetime flag like brownout/watchdog). */
void esps_health_set_link_lost(bool lost);

/* Set by the store layer when data is buffered pending delivery. */
void esps_health_set_buffered_pending(bool pending);

/* Set by whichever radio link is active; 0 (no radio link) is the default
 * and matches PROTOCOL.md S4.3's "0 if no radio link". */
void esps_health_set_rssi(int8_t rssi);

/* Fills uptime_ms/heap_free/heap_min from live ESP-IDF state, state/flags
 * from the above, rssi from the last esps_health_set_rssi(). */
void esps_health_build_heartbeat(esps_heartbeat_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_HEALTH_H */

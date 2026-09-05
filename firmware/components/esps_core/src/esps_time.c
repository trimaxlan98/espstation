/* See esps_time.h. PROTOCOL.md S4.11 is explicit that "node time is never
 * rewritten": the node's monotonic clock is the ground truth for ordering
 * samples that may already be sitting in the store (S5) waiting to be sent,
 * and correcting it after the fact would silently reorder or misdate
 * already-buffered data. So this file only ever *reports* node time; the
 * offset/rtt math lives entirely on the station.
 */
#include "esps_time.h"
#include "esps_enlp.h"

#include "esp_timer.h"

uint32_t esps_time_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool esps_time_build_reply(uint64_t t1_host_us, uint32_t t2_node_ms,
                            uint8_t *buf, size_t cap, size_t *out_len) {
    esps_time_sync_t ts = {
        .t1_host_us = t1_host_us,
        .t2_node_ms = t2_node_ms,
        .t3_node_ms = esps_time_now_ms(),
    };
    return esps_enlp_pack_time_sync(&ts, buf, cap, out_len);
}

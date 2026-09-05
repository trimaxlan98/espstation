/* Monotonic ms clock and the TIME_SYNC (0x70) responder (PROTOCOL.md S4.11).
 * The node's clock is never adjusted from the station's — see the header
 * comment in esps_time.c for why that invariant is load-bearing, not just
 * caution.
 */
#ifndef ESPS_TIME_H
#define ESPS_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic milliseconds since boot. The one clock source every outgoing
 * frame's timestamp field is drawn from (HEARTBEAT.uptime_ms,
 * TELEMETRY.base_ts_ms, LOG.ts_ms, TIME_SYNC.t2/t3) so they stay comparable
 * against each other without a conversion step on the node. */
uint32_t esps_time_now_ms(void);

/* Builds the packed TIME_SYNC reply payload. `t2_node_ms` must be captured
 * by the caller at the moment the station's TIME_SYNC frame was parsed
 * (before any queuing delay) — passing it in rather than deriving it here
 * is what keeps the round-trip estimate honest. t3 (send time) is captured
 * internally, right before packing, which is as close to "on the wire" as
 * this layer can get without instrumenting the link's TX task. */
bool esps_time_build_reply(uint64_t t1_host_us, uint32_t t2_node_ms,
                            uint8_t *buf, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_TIME_H */

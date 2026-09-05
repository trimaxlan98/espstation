/* `seq` in the ENLP header is per-sender, not per-message-type (PROTOCOL.md
 * S3: "gaps are how the station detects loss") — every frame a node emits,
 * whether HEARTBEAT, TELEMETRY, LOG or CMD_ACK, draws from one shared
 * counter, or the station would see phantom gaps every time two message
 * types interleave. This is the single place that counter lives, so the
 * heartbeat task, telemetry task, log hook and CMD dispatcher all agree.
 */
#ifndef ESPS_FRAME_H
#define ESPS_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thread-safe (used from multiple FreeRTOS tasks and the log hook, which
 * can run on any task). Wraps at 65536 per PROTOCOL.md S3. */
uint16_t esps_frame_next_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_FRAME_H */

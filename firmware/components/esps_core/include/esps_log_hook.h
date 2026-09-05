/* Installs an esp_log_set_vprintf() hook that turns every ESP-IDF log line
 * into a structured LOG (0x20) frame (PROTOCOL.md S2.1/S4.5) instead of raw
 * text on the UART — raw text on that wire is reserved for boot-ROM output
 * that predates this hook being installed.
 */
#ifndef ESPS_LOG_HOOK_H
#define ESPS_LOG_HOOK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Delivers one already-framed (COBS+delimiter) LOG frame. Must be
 * non-blocking and safe to call from any task context (the hook itself
 * never blocks, so its sink can't either) — return false if it could not be
 * queued; esps_log_hook counts that as a drop and does not retry. */
typedef bool (*esps_log_sink_t)(const uint8_t *frame, size_t len, void *ctx);

/* Installs the hook. Call once, after esps_node_id_init() (the hook needs a
 * node id and seq counter to frame each line) and before anything else logs
 * — logging that happens earlier goes out as plain text via the default
 * vprintf, which is fine (it is boot-time output, S2.1's raw-console case). */
void esps_log_hook_init(esps_log_sink_t sink, void *ctx);

/* Lines dropped because the sink was full (`sink` returned false) or the
 * hook re-entered itself. Exposed so a HEARTBEAT or EVENT can report it —
 * this codebase does not hide dropped diagnostics as an interface hides
 * bugs from you. */
uint32_t esps_log_hook_dropped_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_LOG_HOOK_H */

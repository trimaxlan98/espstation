/* The ESP-IDF half of esps_dio: pins, ISRs, the phase task, and the NDB /
 * telemetry / event surface main.c wires to the link.
 *
 * Everything that can be decided without hardware lives in esps_dio_policy.h
 * and the other pure headers; this one owns only what genuinely needs the
 * driver: GPIO configuration, interrupt handlers, bit-bang timing and a
 * FreeRTOS task. It deliberately declares no ESP-IDF types, so main.c can
 * include it without inheriting anything.
 *
 * ROLE SELECTION IS A BUILD VARIANT, NOT A RUNTIME SETTING.
 * ESPS_DIO_ROLE is 0 (disabled), 1 (A: handshake initiator + N3 transmitter)
 * or 2 (B: handshake responder + N3 receiver), set by the platformio.ini
 * environments esp32dev / esp32dev_dio_a / esp32dev_dio_b.
 *
 * TODO(S3): this configuration belongs in the experiment spec delivered by
 * EXP_SET and persisted to NVS (docs/EXPERIMENTS.md) — role, bit rate, burst
 * size and phase periods are all experiment parameters, not build constants.
 * They are build constants today only because the experiment runtime does not
 * exist yet; nothing here is meant to survive S3 unchanged.
 */
#ifndef ESPS_DIO_H
#define ESPS_DIO_H

#include "esps_dio_pins.h"   /* ESPS_DIO_CH_* channel ids, ESPS_DIO_PIN_*   */
#include "esps_dio_policy.h" /* esps_dio_result_t                           */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ESPS_DIO_ROLE
#define ESPS_DIO_ROLE 0
#endif

#define ESPS_DIO_ROLE_DISABLED 0
#define ESPS_DIO_ROLE_A        1
#define ESPS_DIO_ROLE_B        2

/* --- Events ------------------------------------------------------------- */

/* PROTOCOL.md S4.6's severity vocabulary, as an enum so this component never
 * has to know how main.c spells it on the wire. */
typedef enum {
    ESPS_DIO_SEV_DEBUG = 0,
    ESPS_DIO_SEV_INFO,
    ESPS_DIO_SEV_WARNING,
    ESPS_DIO_SEV_ERROR,
} esps_dio_severity_t;

/* Event codes, exactly as SPEC-LINK.md names them. */
#define ESPS_DIO_EV_EDGE          "dio.edge"
#define ESPS_DIO_EV_FRAME_OK      "link.frame_ok"
#define ESPS_DIO_EV_CRC_ERR       "link.crc_err"
#define ESPS_DIO_EV_LINK_LOST     "link.lost"
#define ESPS_DIO_EV_LINK_UP       "link.up"
#define ESPS_DIO_EV_GPIO          "dio.gpio"
#define ESPS_DIO_EV_GPIO_REJECTED "dio.gpio_rejected"

/* One event, in the least opinionated shape that still carries everything the
 * spec asks for: a code, a severity, up to two named numbers and an optional
 * reason string. Building the JSON is main.c's job — this component must not
 * depend on cJSON or on the link, both because it has no business knowing how
 * the node talks to the station and because a cJSON allocation is not
 * something to do on the path that the phase task runs. */
typedef struct esps_dio_event {
    const char *code; /* one of the ESPS_DIO_EV_* literals; never NULL      */
    esps_dio_severity_t severity;
    const char *a_key; /* NULL when unused                                  */
    int32_t a_val;
    const char *b_key; /* NULL when unused                                  */
    int32_t b_val;
    const char *reason; /* dio.gpio_rejected's `reason`; NULL otherwise     */
    /* Events dropped by the rate limiter since the last one that got
     * through. SPEC-LINK.md requires it as `data.suppressed` on the limited
     * events (dio.edge, link.frame_ok, link.crc_err and link.lost-by-frames)
     * so the station can tell a sample of a stream from the whole stream.
     * The sink emits the field only when this is non-zero. */
    uint32_t suppressed;
} esps_dio_event_t;

/* Called from the phase task, never from an ISR and never from a critical
 * section, so the sink may allocate, log and send a frame. It must not block
 * indefinitely: it runs on the task that also has to meet the link's phase
 * deadlines. */
typedef void (*esps_dio_event_sink_fn)(const esps_dio_event_t *ev, void *ctx);

/* Install before esps_dio_start(); a NULL sink simply drops events. */
void esps_dio_set_event_sink(esps_dio_event_sink_fn fn, void *ctx);

/* --- NDB ----------------------------------------------------------------- */

/* One NDB channel declaration (PROTOCOL.md S4.1). Strings are static literals
 * owned by this component; the caller copies them into its HELLO JSON. */
typedef struct esps_dio_ndb_entry {
    uint8_t id;
    const char *key;
    const char *name;
    const char *unit; /* "" for a dimensionless channel */
    const char *type; /* NDB semantic type: "u8", "u32", "f32" */
    uint16_t rate_hz;
    const char *group;
} esps_dio_ndb_entry_t;

/* How many channels esps_dio_ndb() returns. Exposed as a macro so a caller
 * can size its own channel array at compile time instead of discovering the
 * overflow at runtime. Kept honest by a _Static_assert against the table. */
#define ESPS_DIO_NDB_COUNT 6u

/* The six channels of SPEC-LINK.md, ids 16..21. Both roles declare all six;
 * see esps_dio_get_sample() for which ones each role actually samples. Sets
 * *out_count and returns the table (never NULL). */
const esps_dio_ndb_entry_t *esps_dio_ndb(size_t *out_count);

/* --- Telemetry ------------------------------------------------------------ */

typedef struct esps_dio_sample {
    uint8_t tx_level;  /* real pad level of TX_DATA, read back from the GPIO */
    uint8_t rx_level;  /* real pad level of RX_DATA                          */
    uint32_t rtt_us;   /* valid only when rtt_valid                          */
    uint32_t frames_ok;
    uint32_t frames_err; /* frames_crc_err + frames_len_err (SPEC-LINK.md)   */
    float ber;
    /* False when the most recent handshake timed out, and always false on
     * role B, which has no handshake to time. SPEC-LINK.md: link.rtt_us is
     * not published on a timeout — the caller skips the sample rather than
     * sending a stale or zero value that would plot as a real measurement. */
    bool rtt_valid;
} esps_dio_sample_t;

/* Snapshot for the telemetry task. Safe to call from any task and before
 * esps_dio_start() (everything reads 0 then).
 *
 * Not atomic as a set: the counters are plain 32-bit loads, each individually
 * torn-free, but a burst finishing between two of them can leave frames_ok
 * one ahead of ber. At a 1 Hz telemetry rate that is a one-frame skew in a
 * ratio, which is not worth a lock on the ISR's path to remove. */
void esps_dio_get_sample(esps_dio_sample_t *out);

/* --- Lifecycle ------------------------------------------------------------ */

/* Starts the link: validates the pin assignment, creates the phase task and
 * lets it configure the GPIOs and install the interrupt handlers.
 *
 * Autonomous by construction (D-1): call it from app_main without waiting for
 * a link, a HELLO_ACK or a station. It returns false only when the build's own
 * pin assignment is invalid or the task cannot be created — never because
 * nothing is plugged in. A false return is not fatal to the node: everything
 * else keeps running, which is the point.
 *
 * With ESPS_DIO_ROLE == 0 this component contributes no code at all; main.c
 * does not reference it and the base firmware is unchanged. */
bool esps_dio_start(void);

/* --- set_gpio (M2) --------------------------------------------------------- */

/* Drives one operator-selected GPIO, with the full SPEC-LINK.md precondition:
 * the output allow-list, the four pins the link owns (14/25/26/27, rejected
 * with reason "owned_by_link" outside ESPS_DIO_MODE_MANUAL) and the level
 * range. A rejection emits dio.gpio_rejected (severity warning, carrying
 * gpio/level/reason) and changes nothing; a success configures the pin as an
 * output, writes the level and emits dio.gpio.
 *
 * TODO(S3): invoke from the experiment trigger executor (action set_gpio).
 * Nothing reaches this function today — there is no CMD op for it and adding
 * one would be a protocol change. It is implemented, validated and tested so
 * that S3 wires an executor to it rather than writing it then.
 *
 * Returns ESPS_DIO_ERR_NOT_READY before esps_dio_start() has configured the
 * driver, since writing a pin the GPIO driver has not been told about would
 * silently do nothing. */
esps_dio_result_t esps_dio_set_gpio(int gpio, int level);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_DIO_H */

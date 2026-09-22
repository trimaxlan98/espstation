/* The ESP-IDF half of esps_morse: pins, the two edge ISRs, the 1 ms task, and
 * the NDB / telemetry / event surface main.c wires to the link.
 *
 * Everything that can be decided without hardware lives in the pure headers
 * (esps_morse_table.h, esps_morse_decode.h, esps_morse_key.h) and is gated by
 * test/host/. This one owns only what genuinely needs the driver: GPIO
 * configuration, interrupt handlers and a FreeRTOS task. It deliberately
 * declares no ESP-IDF types, so main.c can include it without inheriting
 * anything.
 *
 * SYMMETRY, NOT ROLES. Unlike esps_dio, a Morse transceiver has no A and no B:
 * both boards run the same binary and the cable is crossed
 * (SPEC-DUPLEX.md). ESPS_MORSE_ENABLED is therefore a plain on/off, set by
 * the platformio environment esp32dev_morse, not a role selector.
 *
 * TODO(S3): the thresholds belong in the experiment spec delivered by EXP_SET
 * and persisted to NVS (docs/EXPERIMENTS.md), not in build-time defaults that
 * an operator can only change by reflashing. They are defaults today only
 * because the experiment runtime does not exist yet.
 */
#ifndef ESPS_MORSE_H
#define ESPS_MORSE_H

#include "esps_morse_decode.h"
#include "esps_morse_pins.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ESPS_MORSE_ENABLED
#define ESPS_MORSE_ENABLED 0
#endif

/* --- Events ------------------------------------------------------------- */

/* PROTOCOL.md S4.6's severity vocabulary, as an enum so this component never
 * has to know how main.c spells it on the wire. */
typedef enum {
    ESPS_MORSE_SEV_DEBUG = 0,
    ESPS_MORSE_SEV_INFO,
    ESPS_MORSE_SEV_WARNING,
    ESPS_MORSE_SEV_ERROR,
} esps_morse_severity_t;

/* One event, in the least opinionated shape that still carries what
 * SPEC-DUPLEX.md asks for. Building the JSON is main.c's job -- this
 * component must not depend on cJSON, both because it has no business
 * knowing how the node talks to the station and because an allocation is not
 * something to do on the task that also has to meet a 1 ms period.
 *
 * Named *station*_event to keep it distinct from esps_morse_event_t, the
 * decoder's own event in esps_morse_decode.h. They are different things: one
 * is a decision ("this pulse was a dash"), the other is a message to the
 * station, with a severity and a direction. */
typedef struct esps_morse_station_event {
    const char *code;   /* one of the ESPS_MORSE_EV_* literals; never NULL   */
    esps_morse_severity_t severity;
    /* "RX" (the other operator's hand) or "TX" (the local echo). A static
     * literal, never NULL: every Morse event belongs to one direction. */
    const char *dir;
    char symbol;        /* morse.symbol: '.' or '-'; otherwise '\0'          */
    char letter;        /* morse.letter: the character; otherwise '\0'       */
    const char *text;   /* morse.letter / morse.unknown: the code that closed;
                         * points into `code_buf` below, never NULL for those */
    char code_buf[ESPS_MORSE_MAX_SYMBOL + 2];
    uint32_t ms;        /* the duration that caused it                       */
    /* Events dropped by the rate limiter since the last one that got
     * through, so the station can tell a sample of a stream from the whole
     * stream. The sink emits the field only when this is non-zero. */
    uint32_t suppressed;
} esps_morse_station_event_t;

/* Called from the Morse task, never from an ISR, so the sink may allocate,
 * log and send a frame. It must not block indefinitely: it runs on the task
 * that also has to keep polling the key every millisecond. */
typedef void (*esps_morse_event_sink_fn)(const esps_morse_station_event_t *ev, void *ctx);

/* Install before esps_morse_start(); a NULL sink simply drops events. */
void esps_morse_set_event_sink(esps_morse_event_sink_fn fn, void *ctx);

/* --- NDB ----------------------------------------------------------------- */

/* One NDB channel declaration (PROTOCOL.md S4.1). Strings are static literals
 * owned by this component; the caller copies them into its HELLO JSON. */
typedef struct esps_morse_ndb_entry {
    uint8_t id;
    const char *key;
    const char *name;
    const char *unit; /* "" for a dimensionless channel */
    const char *type; /* NDB semantic type: "u8", "u32" */
    uint16_t rate_hz;
    const char *group;
} esps_morse_ndb_entry_t;

/* How many channels esps_morse_ndb() returns. A macro so a caller can size
 * its own array at compile time instead of discovering the overflow at
 * runtime. Kept honest by a _Static_assert against the table. */
#define ESPS_MORSE_NDB_COUNT 8u

/* The eight channels of SPEC-DUPLEX.md, ids 22..29. Sets *out_count and
 * returns the table (never NULL). */
const esps_morse_ndb_entry_t *esps_morse_ndb(size_t *out_count);

/* --- Telemetry ------------------------------------------------------------ */

typedef struct esps_morse_sample {
    uint8_t tx_level;   /* real pad level of TX_DATA, read back from the GPIO */
    uint8_t rx_level;   /* real pad level of RX_DATA                          */
    uint32_t pulse_ms;  /* last pulse the RX decoder measured                 */
    uint32_t gap_ms;    /* last silence it measured                           */
    uint32_t symbols;   /* RX dots + dashes                                   */
    uint32_t letters;   /* RX letters                                         */
    uint32_t unknown;   /* RX codes not in the table, or overflowed           */
    uint32_t bounces;   /* key reading changes that never became edges        */
} esps_morse_sample_t;

/* Snapshot for the telemetry task. Safe to call from any task and before
 * esps_morse_start() (everything reads 0 then).
 *
 * Not atomic as a set: each counter is an individually torn-free 32-bit load,
 * but a letter closing between two of them can leave `letters` one ahead of
 * `symbols`. At a 1 Hz telemetry rate that is a one-event skew, not worth a
 * lock on a path the 1 ms task runs. */
void esps_morse_get_sample(esps_morse_sample_t *out);

/* --- Thresholds ----------------------------------------------------------- */

/* Replaces one decoder's thresholds. `rx` selects which: true for the
 * incoming hand, false for the local echo. They are separate because the
 * threshold describes the hand that keys, and that hand is at the OTHER end
 * for RX and at this one for TX (SPEC-DUPLEX.md).
 *
 * Returns false and changes nothing when the values are incoherent. Nothing
 * calls this today: it exists so S3 wires an EXP_SET to it rather than
 * writing it then. */
bool esps_morse_set_thresholds(bool rx, const esps_morse_thresholds_t *th);

/* Copies one decoder's current thresholds out. false for NULL args. */
bool esps_morse_get_thresholds(bool rx, esps_morse_thresholds_t *out);

/* --- Lifecycle ------------------------------------------------------------ */

/* Starts the transceiver: configures the pins, installs both edge interrupt
 * handlers and creates the task.
 *
 * Autonomous by construction (D-1): call it from app_main without waiting for
 * a link, a HELLO_ACK or a station. Two operators can key at each other with
 * no laptop attached at all; the station only watches. It returns false only
 * when the driver refuses the pins or the task cannot be created -- never
 * because nothing is plugged in.
 *
 * With ESPS_MORSE_ENABLED == 0 this component contributes no code at all. */
bool esps_morse_start(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_MORSE_H */

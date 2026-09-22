/* The pulse/silence state machine of the Morse link (SPEC-DUPLEX.md
 * "Los dos decodificadores").
 *
 * One instance per direction. A transceiver runs two: RX, fed from the edge
 * interrupt on the incoming wire, is the other operator's hand; TX, fed from
 * the debounced key, is the local echo. They must be the same code, because
 * the whole practice rests on being able to compare what one end thinks it
 * sent against what the other decoded -- if the two halves decided things
 * differently, the comparison would mean nothing.
 *
 * Pure C11 over caller-owned state: no allocation, no globals, no logging, no
 * ESP-IDF. esps_morse_edge() is written to be callable from a task that a
 * GPIO ISR hands timestamps to; it does constant work and never blocks.
 *
 * Time is the node's microsecond clock. Every comparison uses unsigned
 * subtraction, so the ~71.6 minute uint32 wrap of micros() is handled rather
 * than avoided -- no bench session reaches it, which is exactly why it would
 * never be found by testing on hardware [D-10].
 */
#ifndef ESPS_MORSE_DECODE_H
#define ESPS_MORSE_DECODE_H

#include "esps_morse_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Thresholds --------------------------------------------------------- */

/* Start values, NOT calibrated ones. The bench measured punto_raya_ms near
 * 300 with a wide empty band on both hands tested, and found NO valid
 * letter_ms for the operator tested -- the gaps inside a letter and the gaps
 * between letters overlapped. See docs/PRACTICA-MORSE.md, "Resultados". */
#define ESPS_MORSE_DEFAULT_DOT_DASH_MS 300u
#define ESPS_MORSE_DEFAULT_LETTER_MS   700u
#define ESPS_MORSE_DEFAULT_WORD_MS    1800u
#define ESPS_MORSE_DEFAULT_DEBOUNCE_MS  15u

typedef struct {
    uint32_t dot_dash_ms; /* < this is a dot, >= is a dash   */
    uint32_t letter_ms;   /* silence that closes a letter    */
    uint32_t word_ms;     /* silence that closes a word      */
    uint32_t debounce_ms; /* shorter pulses are noise        */
} esps_morse_thresholds_t;

/* Fills t with the defaults above. NULL is a no-op. */
void esps_morse_thresholds_init(esps_morse_thresholds_t *t);

/* The same coherence rules the bench sketch enforces on p/l/w/d: each of the
 * first three in 1..60000, debounce in 0..200, and letter_ms strictly below
 * word_ms (otherwise a word could never be reported). false for NULL. */
bool esps_morse_thresholds_valid(const esps_morse_thresholds_t *t);

/* --- Events ------------------------------------------------------------- */

typedef enum {
    ESPS_MORSE_EVENT_SYMBOL = 0, /* a dot or a dash was decided          */
    ESPS_MORSE_EVENT_FILTERED,   /* a pulse shorter than debounce_ms     */
    ESPS_MORSE_EVENT_LETTER,     /* a code closed and is in the table    */
    ESPS_MORSE_EVENT_UNKNOWN,    /* a code closed and is not, or overflowed */
    ESPS_MORSE_EVENT_WORD,       /* a word gap closed                    */
} esps_morse_event_kind_t;

typedef struct {
    esps_morse_event_kind_t kind;
    char symbol;   /* SYMBOL: '.' or '-'; otherwise '\0'                  */
    char letter;   /* LETTER: the character; otherwise '\0'               */
    /* LETTER/UNKNOWN: the code that closed, NUL-terminated, with a
     * trailing '+' when more than ESPS_MORSE_MAX_SYMBOL pulses arrived. */
    char code[ESPS_MORSE_MAX_SYMBOL + 2];
    uint32_t ms;   /* the duration that caused this: pulse for SYMBOL and
                    * FILTERED, gap for LETTER, UNKNOWN and WORD          */
} esps_morse_event_t;

/* --- Decoder ------------------------------------------------------------ */

typedef struct {
    esps_morse_thresholds_t th;
    /* state */
    uint8_t level;             /* last level processed                     */
    bool in_pulse;             /* a rise happened and its fall has not     */
    bool have_fall;            /* t_fall_us is meaningful                  */
    bool measuring;            /* a letter or a word is still pending      */
    bool overflowed;           /* more than MAX_SYMBOL pulses this letter  */
    bool letter_since_word;    /* a letter closed since the last word gap  */
    uint32_t t_rise_us;
    uint32_t t_fall_us;
    char symbol[ESPS_MORSE_MAX_SYMBOL + 1];
    uint8_t n_symbol;
    /* counters, the same ones the bench sketch prints for `r` */
    uint32_t filtered;
    uint32_t dots;
    uint32_t dashes;
    uint32_t letters;
    uint32_t unknown;
    uint32_t repeated;         /* edges whose level equalled the last one  */
    uint32_t last_pulse_ms;
    uint32_t last_gap_ms;
} esps_morse_dec_t;

/* Zero-fills d and installs `th` (or the defaults when th is NULL or
 * invalid; the return value says which). A zero-filled struct is already a
 * valid idle state, but call this so the thresholds are never zero. */
bool esps_morse_dec_init(esps_morse_dec_t *d, const esps_morse_thresholds_t *th);

/* Replaces the thresholds if they are coherent. Returns false and changes
 * nothing otherwise, so a bad command from the station cannot leave a
 * decoder in a state where letters never close. */
bool esps_morse_dec_set_thresholds(esps_morse_dec_t *d, const esps_morse_thresholds_t *th);

/* Feeds one edge, already timestamped. `level` is 0 or non-zero.
 *
 * Writes at most one event into out[0..max) and returns how many it wrote
 * (0 or 1). A rise never produces an event; a fall produces SYMBOL or
 * FILTERED. An edge whose level equals the last one is counted in
 * `repeated` and ignored -- that is what an ISR reading the pin after the
 * line has already gone back looks like.
 *
 * Passing out == NULL or max == 0 still advances the state machine and
 * returns 0: a caller that does not care about events is allowed. */
size_t esps_morse_edge(esps_morse_dec_t *d, uint8_t level, uint32_t t_us,
                       esps_morse_event_t *out, size_t max);

/* Call as often as the main loop runs. Closes letters and words.
 *
 * Writes up to TWO events (a LETTER or UNKNOWN, then a WORD) and returns how
 * many it wrote. Order matters and is part of the contract: the letter always
 * precedes the word gap that also closed.
 *
 * **`max` must be at least 2.** The state machine advances whether or not the
 * caller has room, so a gap long enough to close both a letter and a word
 * with max == 1 reports the letter and loses the word -- the word will not be
 * repeated on the next tick, because it has already been consumed. Passing
 * out == NULL or max == 0 is different and supported: it means "advance, I do
 * not want events". */
size_t esps_morse_tick(esps_morse_dec_t *d, uint32_t now_us,
                       esps_morse_event_t *out, size_t max);

/* Zeroes the counters, leaving thresholds and the in-flight symbol alone --
 * the same thing the sketch's `c` command does. */
void esps_morse_dec_reset_counters(esps_morse_dec_t *d);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_MORSE_DECODE_H */

/* The operator's key and its debounce (SPEC-DUPLEX.md "Transmisor").
 *
 * The key is two stripped jumper ends pressed together. That contact bounces
 * for a few milliseconds on both close and open -- the bench measured about
 * three raw reading changes per accepted edge on a good contact and ten on a
 * dirty one -- so a reading change is only accepted once the reading has held
 * for `debounce_ms`.
 *
 * The property that makes this usable for Morse: BOTH edges are delayed by
 * the same amount, so the pulse duration the far end measures is the real
 * one. The bench confirmed it to the millisecond (a 200 ms press decoded as
 * 200 ms with a 15 ms filter).
 *
 * Pure C11 over caller-owned state. Time is the node's millisecond clock and
 * the comparison is wrap-safe [D-10].
 */
#ifndef ESPS_MORSE_KEY_H
#define ESPS_MORSE_KEY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPS_MORSE_KEY_DEBOUNCE_MAX_MS 200u

typedef struct {
    uint32_t debounce_ms;
    uint8_t stable;         /* the level currently on the wire            */
    uint8_t candidate;      /* the last raw reading                       */
    uint32_t t_candidate_ms;/* when the reading first became `candidate`  */
    uint32_t raw_changes;   /* reading changes, before the filter          */
    uint32_t accepted;      /* edges that passed it                        */
} esps_morse_key_t;

/* Zero-fills k and sets the filter. A debounce above the maximum is clamped
 * and reported by the return value; 0 is legal and disables filtering, which
 * is the contrast case the practice uses to show what bounce looks like. */
bool esps_morse_key_init(esps_morse_key_t *k, uint32_t debounce_ms, uint8_t initial_level);

/* One poll of the pin.
 *
 * Returns 0 or 1 when an edge is ACCEPTED -- that is the new level to write
 * to the wire -- and -1 when nothing was accepted this poll. Call it as fast
 * as the loop runs; at human keying speeds it is called thousands of times
 * per millisecond, which is why a change is never accepted on the same poll
 * that first sees it.
 *
 * A pulse shorter than debounce_ms produces no accepted edge at all: the
 * reading reverts before the filter is satisfied, so the far end never sees
 * it. That is a different thing from the decoder's own debounce, which
 * discards a pulse it has already measured. */
int esps_morse_key_sample(esps_morse_key_t *k, uint8_t reading, uint32_t now_ms);

/* Reading changes that never became edges: the morse.bounces channel. */
uint32_t esps_morse_key_bounces(const esps_morse_key_t *k);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_MORSE_KEY_H */

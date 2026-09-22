#include "esps_morse_key.h"

#include <string.h>

bool esps_morse_key_init(esps_morse_key_t *k, uint32_t debounce_ms, uint8_t initial_level) {
    if (k == NULL) {
        return false;
    }
    memset(k, 0, sizeof(*k));
    uint8_t lvl = initial_level ? 1u : 0u;
    /* Start already agreeing with the pin. Booting with the key held down is
     * a real case (an operator resting a finger on it), and the alternative
     * -- assuming 0 -- would fabricate an edge that never happened. */
    k->stable = lvl;
    k->candidate = lvl;
    if (debounce_ms > ESPS_MORSE_KEY_DEBOUNCE_MAX_MS) {
        k->debounce_ms = ESPS_MORSE_KEY_DEBOUNCE_MAX_MS;
        return false;
    }
    k->debounce_ms = debounce_ms;
    return true;
}

int esps_morse_key_sample(esps_morse_key_t *k, uint8_t reading, uint32_t now_ms) {
    if (k == NULL) {
        return -1;
    }
    uint8_t lvl = reading ? 1u : 0u;

    if (lvl != k->candidate) {
        /* A change, real or a bounce: restart the stability clock. */
        k->candidate = lvl;
        k->t_candidate_ms = now_ms;
        k->raw_changes++;
        return -1;
    }

    /* Unsigned subtraction, so this stays correct across the ~49.7 day wrap
     * of the millisecond clock [D-10] -- never `now_ms - k->t_candidate_ms
     * >= x` written as a comparison of two absolute stamps. */
    if (k->candidate != k->stable &&
        (uint32_t)(now_ms - k->t_candidate_ms) >= k->debounce_ms) {
        k->stable = k->candidate;
        k->accepted++;
        return (int)k->stable;
    }
    return -1;
}

uint32_t esps_morse_key_bounces(const esps_morse_key_t *k) {
    if (k == NULL) {
        return 0;
    }
    /* Defensive: accepted can never exceed raw_changes, but a caller reading
     * a partially-initialised struct should get 0, not a huge unsigned. */
    if (k->accepted > k->raw_changes) {
        return 0;
    }
    return k->raw_changes - k->accepted;
}

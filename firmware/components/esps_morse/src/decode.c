/* The Morse state machine. Mirrors, deliberately line for line, the Arduino
 * sketch of the bench practice (bench/practicas/morse-duplex/transceptor/)
 * and the gateway's Python mirror (transports/sim/morse_link.py). Three
 * implementations exist because the link is a node-to-node contract, not
 * ENLP; the golden vectors in SPEC-DUPLEX.md are what stop them drifting.
 *
 * The one thing to preserve above all is the ORDER events come out in: a
 * symbol the instant its pulse ends, the letter during the following silence
 * and before the gap that produced it is reported, the word after the letter.
 * Every vector compares that order.
 */
#include "esps_morse_decode.h"

#include <string.h>

/* Milliseconds elapsed between two microsecond stamps, wrap-safe. Unsigned
 * subtraction is defined to wrap, so this stays correct across the ~71.6 min
 * uint32 rollover of the node's clock [D-10]. Truncating division is what
 * makes the +-1 ms agreement measured on the bench reproducible here and in
 * the two mirrors -- do not round it. */
static uint32_t elapsed_ms(uint32_t now_us, uint32_t then_us) {
    return (uint32_t)(now_us - then_us) / 1000u;
}

void esps_morse_thresholds_init(esps_morse_thresholds_t *t) {
    if (t == NULL) {
        return;
    }
    t->dot_dash_ms = ESPS_MORSE_DEFAULT_DOT_DASH_MS;
    t->letter_ms = ESPS_MORSE_DEFAULT_LETTER_MS;
    t->word_ms = ESPS_MORSE_DEFAULT_WORD_MS;
    t->debounce_ms = ESPS_MORSE_DEFAULT_DEBOUNCE_MS;
}

bool esps_morse_thresholds_valid(const esps_morse_thresholds_t *t) {
    if (t == NULL) {
        return false;
    }
    if (t->dot_dash_ms < 1u || t->dot_dash_ms > 60000u) {
        return false;
    }
    if (t->letter_ms < 1u || t->letter_ms > 60000u) {
        return false;
    }
    if (t->word_ms < 1u || t->word_ms > 60000u) {
        return false;
    }
    if (t->debounce_ms > 200u) {
        return false;
    }
    /* A word gap that is not strictly longer than a letter gap would mean a
     * word can never be reported separately from the letter that ends it. */
    return t->letter_ms < t->word_ms;
}

bool esps_morse_dec_init(esps_morse_dec_t *d, const esps_morse_thresholds_t *th) {
    if (d == NULL) {
        return false;
    }
    memset(d, 0, sizeof(*d));
    if (esps_morse_thresholds_valid(th)) {
        d->th = *th;
        return true;
    }
    esps_morse_thresholds_init(&d->th);
    return false;
}

bool esps_morse_dec_set_thresholds(esps_morse_dec_t *d, const esps_morse_thresholds_t *th) {
    if (d == NULL || !esps_morse_thresholds_valid(th)) {
        return false;
    }
    d->th = *th;
    return true;
}

void esps_morse_dec_reset_counters(esps_morse_dec_t *d) {
    if (d == NULL) {
        return;
    }
    d->filtered = 0;
    d->dots = 0;
    d->dashes = 0;
    d->letters = 0;
    d->unknown = 0;
    d->repeated = 0;
    d->last_pulse_ms = 0;
    d->last_gap_ms = 0;
}

/* Writes ev into out[0] when there is room, and reports whether it did. The
 * state machine must advance identically whether or not the caller wanted
 * the event, so every call site uses this rather than branching earlier. */
static size_t emit(esps_morse_event_t *out, size_t max, const esps_morse_event_t *ev) {
    if (out == NULL || max == 0) {
        return 0;
    }
    out[0] = *ev;
    return 1;
}

size_t esps_morse_edge(esps_morse_dec_t *d, uint8_t level, uint32_t t_us,
                       esps_morse_event_t *out, size_t max) {
    if (d == NULL) {
        return 0;
    }
    uint8_t lvl = level ? 1u : 0u;
    if (lvl == d->level) {
        /* The ISR read the pin after the line had already gone back: the
         * edge is real, the level is stale. Counted, never guessed at. */
        d->repeated++;
        return 0;
    }
    d->level = lvl;

    if (lvl != 0u) { /* rising */
        if (d->have_fall) {
            d->last_gap_ms = elapsed_ms(t_us, d->t_fall_us);
        }
        d->t_rise_us = t_us;
        d->in_pulse = true;
        return 0;
    }

    /* falling */
    if (!d->in_pulse) {
        /* Started with the key already closed: there is no rise to measure
         * from, so this fall is not a pulse. */
        return 0;
    }
    d->in_pulse = false;

    uint32_t dur_ms = elapsed_ms(t_us, d->t_rise_us);
    d->last_pulse_ms = dur_ms;

    esps_morse_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.ms = dur_ms;

    if (dur_ms < d->th.debounce_ms) {
        /* Noise. It is not a symbol AND it does not interrupt the silence:
         * t_fall_us is left alone so the gap being measured keeps running
         * through the glitch. */
        d->filtered++;
        ev.kind = ESPS_MORSE_EVENT_FILTERED;
        return emit(out, max, &ev);
    }

    char s = (dur_ms < d->th.dot_dash_ms) ? '.' : '-';
    if (s == '.') {
        d->dots++;
    } else {
        d->dashes++;
    }
    if (d->n_symbol < ESPS_MORSE_MAX_SYMBOL) {
        d->symbol[d->n_symbol++] = s;
    } else {
        d->overflowed = true;
    }
    d->t_fall_us = t_us;
    d->have_fall = true;
    d->measuring = true;

    ev.kind = ESPS_MORSE_EVENT_SYMBOL;
    ev.symbol = s;
    return emit(out, max, &ev);
}

/* Closes the in-flight code into ev. */
static void close_letter(esps_morse_dec_t *d, uint32_t gap_ms, esps_morse_event_t *ev) {
    d->symbol[d->n_symbol] = '\0';

    memset(ev, 0, sizeof(*ev));
    ev->ms = gap_ms;
    memcpy(ev->code, d->symbol, (size_t)d->n_symbol + 1u);
    if (d->overflowed) {
        /* The '+' is part of the reported code: eight dots and a jammed key
         * must not look like a legitimate eight-dot code. */
        ev->code[d->n_symbol] = '+';
        ev->code[d->n_symbol + 1u] = '\0';
    }

    char c = d->overflowed ? '\0' : esps_morse_decode(d->symbol);
    if (c != '\0') {
        d->letters++;
        ev->kind = ESPS_MORSE_EVENT_LETTER;
        ev->letter = c;
    } else {
        d->unknown++;
        ev->kind = ESPS_MORSE_EVENT_UNKNOWN;
    }

    d->n_symbol = 0;
    d->overflowed = false;
    d->letter_since_word = true;
}

size_t esps_morse_tick(esps_morse_dec_t *d, uint32_t now_us,
                       esps_morse_event_t *out, size_t max) {
    if (d == NULL || !d->measuring || d->level != 0u) {
        return 0;
    }
    uint32_t gap_ms = elapsed_ms(now_us, d->t_fall_us);
    size_t n = 0;

    if (d->n_symbol > 0u && gap_ms >= d->th.letter_ms) {
        esps_morse_event_t ev;
        close_letter(d, gap_ms, &ev);
        n += emit(out ? out + n : NULL, (max > n) ? (max - n) : 0u, &ev);
    }

    if (d->letter_since_word && gap_ms >= d->th.word_ms) {
        esps_morse_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = ESPS_MORSE_EVENT_WORD;
        ev.ms = gap_ms;
        /* The flag is what makes this fire once per gap instead of on every
         * loop iteration for as long as the silence lasts. */
        d->letter_since_word = false;
        n += emit(out ? out + n : NULL, (max > n) ? (max - n) : 0u, &ev);
    }

    if (d->n_symbol == 0u && !d->letter_since_word) {
        /* Nothing left to wait for. Stop measuring so the next tick does not
         * keep subtracting from a t_fall_us that is drifting towards the
         * uint32 wrap with no event ever due. */
        d->measuring = false;
    }
    return n;
}

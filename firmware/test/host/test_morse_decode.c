/* The Morse pulse/silence state machine (SPEC-DUPLEX.md).
 *
 * These are the golden vectors. The same cases exist in the bench practice's
 * host tests for the real .ino and in gateway/tests/test_morse_link.py for
 * the Python mirror; three implementations of one contract can only be kept
 * honest by comparing all three against the same fixed answers.
 *
 * Nothing here measures timing. It measures DECISIONS at given timestamps,
 * which is the part that must be identical everywhere.
 */
#include "esps_morse_decode.h"
#include "harness.h"

#include <string.h>

#define MAXEV 4

/* A tiny driver: feed edges, ticking every millisecond between them exactly
 * as a main loop would, and collect every event in order. */
typedef struct {
    esps_morse_dec_t d;
    esps_morse_event_t ev[64];
    size_t n;
} drv_t;

static void drv_collect(drv_t *dr, size_t got, const esps_morse_event_t *src) {
    for (size_t i = 0; i < got && dr->n < (sizeof(dr->ev) / sizeof(dr->ev[0])); i++) {
        dr->ev[dr->n++] = src[i];
    }
}

static void drv_tick_to(drv_t *dr, uint32_t from_us, uint32_t to_us) {
    esps_morse_event_t tmp[MAXEV];
    for (uint32_t t = from_us + 1000u; t <= to_us; t += 1000u) {
        drv_collect(dr, esps_morse_tick(&dr->d, t, tmp, MAXEV), tmp);
    }
}

/* One pulse: rises at `at_us`, falls `dur_ms` later. Returns the fall time. */
static uint32_t drv_pulse(drv_t *dr, uint32_t at_us, uint32_t dur_ms) {
    esps_morse_event_t tmp[MAXEV];
    drv_collect(dr, esps_morse_edge(&dr->d, 1, at_us, tmp, MAXEV), tmp);
    uint32_t fall = at_us + dur_ms * 1000u;
    drv_collect(dr, esps_morse_edge(&dr->d, 0, fall, tmp, MAXEV), tmp);
    return fall;
}

/* Keys a pattern like "... --- ..." with the given element timings. */
static uint32_t drv_key(drv_t *dr, const char *pattern, uint32_t t_us,
                        uint32_t dot_ms, uint32_t dash_ms,
                        uint32_t gap_ms, uint32_t letter_gap_ms) {
    for (const char *p = pattern; *p != '\0'; p++) {
        if (*p == ' ') {
            drv_tick_to(dr, t_us, t_us + letter_gap_ms * 1000u);
            t_us += letter_gap_ms * 1000u;
            continue;
        }
        if (p != pattern && *(p - 1) != ' ') {
            drv_tick_to(dr, t_us, t_us + gap_ms * 1000u);
            t_us += gap_ms * 1000u;
        }
        t_us = drv_pulse(dr, t_us, (*p == '-') ? dash_ms : dot_ms);
    }
    return t_us;
}

static void drv_init(drv_t *dr, const esps_morse_thresholds_t *th) {
    memset(dr, 0, sizeof(*dr));
    esps_morse_dec_init(&dr->d, th);
}

/* The decoded text of everything collected: letters, '?' for unknown codes,
 * ' ' for a word gap. Symbols and filtered pulses do not appear. */
static void drv_text(const drv_t *dr, char *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < dr->n && n + 1u < cap; i++) {
        switch (dr->ev[i].kind) {
            case ESPS_MORSE_EVENT_LETTER:  out[n++] = dr->ev[i].letter; break;
            case ESPS_MORSE_EVENT_UNKNOWN: out[n++] = '?'; break;
            case ESPS_MORSE_EVENT_WORD:    out[n++] = ' '; break;
            default: break;
        }
    }
    out[n] = '\0';
}

int test_morse_decode_all(void) {
    int fails = 0;
    esps_morse_event_t ev[MAXEV];
    char text[64];

    /* --- thresholds ---------------------------------------------------- */
    esps_morse_thresholds_t th;
    esps_morse_thresholds_init(&th);
    ESPS_CHECK_EQ(&fails, th.dot_dash_ms, ESPS_MORSE_DEFAULT_DOT_DASH_MS);
    ESPS_CHECK(&fails, esps_morse_thresholds_valid(&th));
    ESPS_CHECK(&fails, !esps_morse_thresholds_valid(NULL));

    th.letter_ms = 2000; th.word_ms = 1800;   /* letter must be below word  */
    ESPS_CHECK(&fails, !esps_morse_thresholds_valid(&th));
    esps_morse_thresholds_init(&th);
    th.debounce_ms = 201;                      /* above the documented max   */
    ESPS_CHECK(&fails, !esps_morse_thresholds_valid(&th));
    esps_morse_thresholds_init(&th);
    th.dot_dash_ms = 0;
    ESPS_CHECK(&fails, !esps_morse_thresholds_valid(&th));

    /* An invalid set must not be installed, and init must fall back to the
     * defaults rather than leaving a decoder with zero thresholds, where no
     * letter would ever close. */
    esps_morse_dec_t d;
    ESPS_CHECK(&fails, !esps_morse_dec_init(&d, &th));
    ESPS_CHECK_EQ(&fails, d.th.dot_dash_ms, ESPS_MORSE_DEFAULT_DOT_DASH_MS);
    ESPS_CHECK(&fails, !esps_morse_dec_set_thresholds(&d, &th));
    ESPS_CHECK_EQ(&fails, d.th.dot_dash_ms, ESPS_MORSE_DEFAULT_DOT_DASH_MS);

    /* --- golden vector: SOS -------------------------------------------- */
    drv_t dr;
    esps_morse_thresholds_init(&th);
    drv_init(&dr, &th);
    uint32_t t = drv_key(&dr, "... --- ...", 1000000u, 100u, 400u, 150u, 900u);
    drv_tick_to(&dr, t, t + 3000000u);

    static const esps_morse_event_kind_t EXPECTED[] = {
        ESPS_MORSE_EVENT_SYMBOL, ESPS_MORSE_EVENT_SYMBOL, ESPS_MORSE_EVENT_SYMBOL,
        ESPS_MORSE_EVENT_LETTER,
        ESPS_MORSE_EVENT_SYMBOL, ESPS_MORSE_EVENT_SYMBOL, ESPS_MORSE_EVENT_SYMBOL,
        ESPS_MORSE_EVENT_LETTER,
        ESPS_MORSE_EVENT_SYMBOL, ESPS_MORSE_EVENT_SYMBOL, ESPS_MORSE_EVENT_SYMBOL,
        ESPS_MORSE_EVENT_LETTER,
        ESPS_MORSE_EVENT_WORD,
    };
    ESPS_CHECK_EQ(&fails, dr.n, sizeof(EXPECTED) / sizeof(EXPECTED[0]));
    for (size_t i = 0; i < dr.n && i < sizeof(EXPECTED) / sizeof(EXPECTED[0]); i++) {
        ESPS_CHECK_EQ(&fails, dr.ev[i].kind, EXPECTED[i]);
    }
    drv_text(&dr, text, sizeof(text));
    ESPS_CHECK(&fails, strcmp(text, "SOS ") == 0);
    ESPS_CHECK_EQ(&fails, dr.d.dots, 6);
    ESPS_CHECK_EQ(&fails, dr.d.dashes, 3);
    ESPS_CHECK_EQ(&fails, dr.d.letters, 3);
    ESPS_CHECK_EQ(&fails, dr.d.unknown, 0);
    ESPS_CHECK_EQ(&fails, dr.ev[0].symbol, '.');
    ESPS_CHECK_EQ(&fails, dr.ev[4].symbol, '-');
    ESPS_CHECK_EQ(&fails, dr.ev[3].letter, 'S');
    ESPS_CHECK(&fails, strcmp(dr.ev[3].code, "...") == 0);

    /* --- the word gap is reported once, not on every tick --------------- */
    drv_init(&dr, &th);
    t = drv_key(&dr, ".", 1000000u, 100u, 400u, 150u, 900u);
    drv_tick_to(&dr, t, t + 10000000u);      /* ten seconds of silence */
    ESPS_CHECK_EQ(&fails, dr.n, 3);          /* symbol, letter E, one word */
    drv_text(&dr, text, sizeof(text));
    ESPS_CHECK(&fails, strcmp(text, "E ") == 0);

    /* --- the dot/dash boundary is inclusive upwards --------------------- */
    drv_init(&dr, &th);
    t = drv_pulse(&dr, 1000000u, 299u);
    drv_tick_to(&dr, t, t + 1000000u);
    drv_text(&dr, text, sizeof(text));
    ESPS_CHECK(&fails, strcmp(text, "E") == 0);   /* 299 is still a dot */

    drv_init(&dr, &th);
    t = drv_pulse(&dr, 1000000u, 300u);
    drv_tick_to(&dr, t, t + 1000000u);
    drv_text(&dr, text, sizeof(text));
    ESPS_CHECK(&fails, strcmp(text, "T") == 0);   /* 300 is a dash */

    /* --- noise below debounce_ms --------------------------------------- */
    drv_init(&dr, &th);
    t = drv_pulse(&dr, 1000000u, 5u);
    drv_tick_to(&dr, t, t + 2000000u);
    ESPS_CHECK_EQ(&fails, dr.n, 1);
    ESPS_CHECK_EQ(&fails, dr.ev[0].kind, ESPS_MORSE_EVENT_FILTERED);
    ESPS_CHECK_EQ(&fails, dr.ev[0].ms, 5);
    ESPS_CHECK_EQ(&fails, dr.d.filtered, 1);
    ESPS_CHECK_EQ(&fails, dr.d.dots, 0);
    ESPS_CHECK_EQ(&fails, dr.d.letters, 0);

    /* A glitch inside a silence must not restart the gap: the letter still
     * closes at the right moment. This is what "it is not a symbol and it
     * does not interrupt the silence" means, and getting it wrong would make
     * a noisy line silently merge letters. */
    drv_init(&dr, &th);
    t = drv_pulse(&dr, 1000000u, 100u);        /* a dot */
    drv_tick_to(&dr, t, t + 300000u);
    (void)drv_pulse(&dr, t + 300000u, 5u);     /* 5 ms of noise mid-gap */
    drv_tick_to(&dr, t + 305000u, t + 2500000u);
    drv_text(&dr, text, sizeof(text));
    ESPS_CHECK(&fails, strcmp(text, "E ") == 0);
    ESPS_CHECK_EQ(&fails, dr.d.filtered, 1);

    /* --- unknown and overflowed codes ----------------------------------- */
    drv_init(&dr, &th);
    t = drv_key(&dr, ".......", 1000000u, 100u, 400u, 150u, 900u);
    drv_tick_to(&dr, t, t + 1000000u);
    ESPS_CHECK_EQ(&fails, dr.ev[dr.n - 1].kind, ESPS_MORSE_EVENT_UNKNOWN);
    ESPS_CHECK(&fails, strcmp(dr.ev[dr.n - 1].code, ".......") == 0);
    ESPS_CHECK_EQ(&fails, dr.d.unknown, 1);

    drv_init(&dr, &th);
    t = drv_key(&dr, ".........", 1000000u, 100u, 400u, 150u, 900u);  /* nine */
    drv_tick_to(&dr, t, t + 1000000u);
    ESPS_CHECK_EQ(&fails, dr.ev[dr.n - 1].kind, ESPS_MORSE_EVENT_UNKNOWN);
    ESPS_CHECK(&fails, strcmp(dr.ev[dr.n - 1].code, "........+") == 0);

    /* --- an edge whose level repeats is counted, not guessed at --------- */
    drv_init(&dr, &th);
    esps_morse_edge(&dr.d, 1, 1000000u, ev, MAXEV);
    ESPS_CHECK_EQ(&fails, esps_morse_edge(&dr.d, 1, 1000100u, ev, MAXEV), 0);
    ESPS_CHECK_EQ(&fails, dr.d.repeated, 1);
    esps_morse_edge(&dr.d, 0, 1100000u, ev, MAXEV);
    ESPS_CHECK_EQ(&fails, dr.d.dots, 1);      /* measured from the FIRST rise */
    ESPS_CHECK_EQ(&fails, dr.d.last_pulse_ms, 100);

    /* --- a fall with no rise before it is not a pulse ------------------- */
    drv_init(&dr, &th);
    ESPS_CHECK_EQ(&fails, esps_morse_edge(&dr.d, 0, 1000000u, ev, MAXEV), 0);
    ESPS_CHECK_EQ(&fails, dr.d.dots, 0);
    ESPS_CHECK_EQ(&fails, dr.d.dashes, 0);
    ESPS_CHECK_EQ(&fails, dr.d.filtered, 0);

    /* --- the microsecond clock wrap [D-10] ------------------------------ */
    /* A pulse that starts just before the uint32 micros() rollover and ends
     * after it must measure 100 ms, not 4294967 s. No bench session is 71
     * minutes long, so this can only ever be caught here. */
    drv_init(&dr, &th);
    uint32_t near_wrap = 0xFFFFFFFFu - 50000u;          /* 50 ms to go */
    esps_morse_edge(&dr.d, 1, near_wrap, ev, MAXEV);
    esps_morse_edge(&dr.d, 0, near_wrap + 100000u, ev, MAXEV);  /* wraps */
    ESPS_CHECK_EQ(&fails, dr.d.last_pulse_ms, 100);
    ESPS_CHECK_EQ(&fails, dr.d.dots, 1);
    /* And the silence after it closes the letter at the right moment. */
    {
        esps_morse_event_t tmp[MAXEV];
        uint32_t fall = near_wrap + 100000u;
        size_t got = 0;
        for (uint32_t k = 1; k <= 900u; k++) {
            got += esps_morse_tick(&dr.d, fall + k * 1000u, tmp, MAXEV);
        }
        ESPS_CHECK(&fails, got >= 1);
    }

    /* --- NULL and zero-capacity calls are defined ----------------------- */
    ESPS_CHECK_EQ(&fails, esps_morse_edge(NULL, 1, 0, ev, MAXEV), 0);
    ESPS_CHECK_EQ(&fails, esps_morse_tick(NULL, 0, ev, MAXEV), 0);
    esps_morse_dec_reset_counters(NULL);
    drv_init(&dr, &th);
    /* out == NULL still advances the machine: the counters move. */
    esps_morse_edge(&dr.d, 1, 1000000u, NULL, 0);
    esps_morse_edge(&dr.d, 0, 1100000u, NULL, 0);
    ESPS_CHECK_EQ(&fails, dr.d.dots, 1);

    /* --- counters reset without disturbing the thresholds --------------- */
    esps_morse_dec_reset_counters(&dr.d);
    ESPS_CHECK_EQ(&fails, dr.d.dots, 0);
    ESPS_CHECK_EQ(&fails, dr.d.th.dot_dash_ms, ESPS_MORSE_DEFAULT_DOT_DASH_MS);

    /* --- the bench's own numbers ---------------------------------------- */
    /* Tanda 2 ran with p=300 l=600 w=1800 d=40 frozen on both boards. These
     * are real pulses and gaps from evidencia/tanda_k40_l600_B.log, and they
     * are here because they are the case no synthetic vector produced: two
     * errors of opposite sign, which is what proves no letter_ms works for
     * that operator (docs/PRACTICA-MORSE.md, "Resultados"). */
    esps_morse_thresholds_t bench = {300u, 600u, 1800u, 40u};
    ESPS_CHECK(&fails, esps_morse_thresholds_valid(&bench));

    /* Three dots separated by gaps of 621 and 627 ms: above l=600, so each
     * closes as its own letter E. The operator meant one S. */
    drv_init(&dr, &bench);
    t = drv_pulse(&dr, 1000000u, 98u);
    drv_tick_to(&dr, t, t + 621000u);
    t = drv_pulse(&dr, t + 621000u, 74u);
    drv_tick_to(&dr, t, t + 627000u);
    t = drv_pulse(&dr, t + 627000u, 77u);
    drv_tick_to(&dr, t, t + 2500000u);
    drv_text(&dr, text, sizeof(text));
    ESPS_CHECK(&fails, strcmp(text, "EEE ") == 0);

    /* And a gap of 548 ms between an S and an O: below l=600, so the two
     * letters merge into one six-symbol code that is in no table. */
    drv_init(&dr, &bench);
    t = drv_key(&dr, "...", 1000000u, 99u, 408u, 202u, 0u);
    drv_tick_to(&dr, t, t + 548000u);
    t = drv_key(&dr, "---", t + 548000u, 99u, 408u, 202u, 0u);
    drv_tick_to(&dr, t, t + 2500000u);
    drv_text(&dr, text, sizeof(text));
    ESPS_CHECK(&fails, strcmp(text, "? ") == 0);
    ESPS_CHECK_EQ(&fails, dr.d.unknown, 1);

    return fails;
}

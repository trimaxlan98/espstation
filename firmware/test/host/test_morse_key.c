/* The key debounce (SPEC-DUPLEX.md "Transmisor").
 *
 * The property everything else rests on: both edges are delayed by the same
 * amount, so the duration that reaches the far end is the real one. If that
 * stopped being true, every pulse measurement in the practice would be off
 * by an unknown amount and nothing would say so.
 */
#include "esps_morse_key.h"
#include "harness.h"

/* Polls the key once per millisecond over a scripted pin, recording when
 * edges were accepted. Returns how many. */
typedef struct {
    uint32_t ms;
    int level;
} edge_t;

static size_t run_key(esps_morse_key_t *k, const uint8_t *pin, uint32_t n_ms,
                      uint32_t t0_ms, edge_t *out, size_t max) {
    size_t n = 0;
    for (uint32_t i = 0; i < n_ms; i++) {
        int r = esps_morse_key_sample(k, pin[i], t0_ms + i);
        if (r >= 0 && n < max) {
            out[n].ms = t0_ms + i;
            out[n].level = r;
            n++;
        }
    }
    return n;
}

int test_morse_key_all(void) {
    int fails = 0;
    esps_morse_key_t k;
    edge_t edges[16];
    static uint8_t pin[600];

    /* --- init ----------------------------------------------------------- */
    ESPS_CHECK(&fails, esps_morse_key_init(&k, 15u, 0u));
    ESPS_CHECK_EQ(&fails, k.debounce_ms, 15);
    ESPS_CHECK_EQ(&fails, k.stable, 0);
    /* Above the maximum is clamped and reported, not silently accepted. */
    ESPS_CHECK(&fails, !esps_morse_key_init(&k, 500u, 0u));
    ESPS_CHECK_EQ(&fails, k.debounce_ms, ESPS_MORSE_KEY_DEBOUNCE_MAX_MS);
    ESPS_CHECK(&fails, !esps_morse_key_init(NULL, 15u, 0u));
    ESPS_CHECK_EQ(&fails, esps_morse_key_sample(NULL, 1u, 0u), -1);
    ESPS_CHECK_EQ(&fails, esps_morse_key_bounces(NULL), 0);

    /* Booting with the key already held must not fabricate an edge. */
    ESPS_CHECK(&fails, esps_morse_key_init(&k, 15u, 1u));
    ESPS_CHECK_EQ(&fails, k.stable, 1);
    for (uint32_t i = 0; i < 100; i++) {
        pin[i] = 1u;
    }
    ESPS_CHECK_EQ(&fails, run_key(&k, pin, 100u, 0u, edges, 16), 0);

    /* --- a bouncy 200 ms press ------------------------------------------ */
    /* Closes at t=0 chattering for 8 ms, held to t=200, opens chattering
     * for 5 ms. Eight raw changes; exactly two should reach the wire. */
    esps_morse_key_init(&k, 15u, 0u);
    for (uint32_t i = 0; i < 600; i++) {
        pin[i] = 0u;
    }
    pin[0] = 1; pin[1] = 0; pin[2] = 1; pin[3] = 0;           /* close bounce */
    for (uint32_t i = 4; i < 200; i++) {
        pin[i] = 1u;                                           /* held */
    }
    pin[200] = 0; pin[201] = 1; pin[202] = 0;                  /* open bounce */
    size_t n = run_key(&k, pin, 400u, 0u, edges, 16);

    ESPS_CHECK_EQ(&fails, n, 2);
    if (n == 2) {
        ESPS_CHECK_EQ(&fails, edges[0].level, 1);
        ESPS_CHECK_EQ(&fails, edges[1].level, 0);
        /* THE property: the rise settles at t=4 and the fall at t=202, and
         * both are delayed by the same 15 ms, so the transmitted duration is
         * 202-4 = 198 ms -- the real one, not 198 plus or minus a filter. */
        ESPS_CHECK_EQ(&fails, edges[0].ms, 4u + 15u);
        ESPS_CHECK_EQ(&fails, edges[1].ms, 202u + 15u);
        ESPS_CHECK_EQ(&fails, edges[1].ms - edges[0].ms, 202u - 4u);
    }
    ESPS_CHECK_EQ(&fails, k.accepted, 2);
    ESPS_CHECK_EQ(&fails, k.raw_changes, 8);
    ESPS_CHECK_EQ(&fails, esps_morse_key_bounces(&k), 6);

    /* --- a pulse shorter than the filter never reaches the wire ---------- */
    esps_morse_key_init(&k, 15u, 0u);
    for (uint32_t i = 0; i < 400; i++) {
        pin[i] = 0u;
    }
    for (uint32_t i = 10; i < 20; i++) {
        pin[i] = 1u;                       /* 10 ms, below the 15 ms filter */
    }
    ESPS_CHECK_EQ(&fails, run_key(&k, pin, 400u, 0u, edges, 16), 0);
    ESPS_CHECK_EQ(&fails, k.accepted, 0);

    /* --- with the filter off, every change gets through ------------------ */
    /* The contrast case the practice uses to show what bounce looks like.
     * Even at 0 the change is accepted on the NEXT poll, never the one that
     * first saw it: on hardware the loop polls far faster than the key
     * moves, so that is immediate there and has to be written out here. */
    esps_morse_key_init(&k, 0u, 0u);
    for (uint32_t i = 0; i < 400; i++) {
        pin[i] = 0u;
    }
    pin[0] = 1; pin[1] = 1; pin[2] = 0; pin[3] = 0;
    for (uint32_t i = 4; i < 200; i++) {
        pin[i] = 1u;
    }
    n = run_key(&k, pin, 300u, 0u, edges, 16);
    ESPS_CHECK_EQ(&fails, n, 4);           /* up, down, up, down */
    ESPS_CHECK_EQ(&fails, esps_morse_key_bounces(&k), 0);

    /* --- the deadline is inclusive, and not a millisecond earlier -------- */
    /* `>=` versus `>` is a one-character difference that no vector above
     * distinguishes, and it is the difference between the 15 ms filter the
     * SPEC documents and a 16 ms one. */
    esps_morse_key_init(&k, 15u, 0u);
    ESPS_CHECK_EQ(&fails, esps_morse_key_sample(&k, 1u, 1000u), -1);  /* the change */
    ESPS_CHECK_EQ(&fails, esps_morse_key_sample(&k, 1u, 1014u), -1);  /* 14 ms: no */
    ESPS_CHECK_EQ(&fails, esps_morse_key_sample(&k, 1u, 1015u), 1);   /* 15 ms: yes */
    /* And once accepted it does not fire again for the same change. */
    ESPS_CHECK_EQ(&fails, esps_morse_key_sample(&k, 1u, 1016u), -1);
    ESPS_CHECK_EQ(&fails, k.accepted, 1);

    /* The documented maximum is a legal value, not one over the edge. */
    ESPS_CHECK(&fails, esps_morse_key_init(&k, ESPS_MORSE_KEY_DEBOUNCE_MAX_MS, 0u));
    ESPS_CHECK_EQ(&fails, k.debounce_ms, ESPS_MORSE_KEY_DEBOUNCE_MAX_MS);

    /* --- a stamp that goes BACKWARDS defeats the filter ------------------ */
    /* Not a defect to fix here: the filter's "has it held long enough?" is an
     * unsigned subtraction, which is what makes it correct across the 49.7
     * day wrap, and the same arithmetic cannot tell a wrap from a clock that
     * went backwards by 1 ms. This vector pins the consequence so that the
     * caller-side guard that exists because of it -- key_clock() in
     * esps_morse.c, which clamps the stamps the ISR rings produce so they
     * never regress -- can never be deleted as "defensive programming nobody
     * asked for". On the node the regression is real: the acceptance poll and
     * the ring drain read the clock at different moments, and the ISR can
     * capture an edge between them. */
    esps_morse_key_init(&k, 15u, 0u);
    ESPS_CHECK_EQ(&fails, esps_morse_key_sample(&k, 1u, 1000u), -1);
    ESPS_CHECK_EQ(&fails, esps_morse_key_sample(&k, 1u, 999u), 1);  /* 1 ms early -> ~49 days */
    ESPS_CHECK_EQ(&fails, k.accepted, 1);

    /* --- the millisecond clock wrap [D-10] ------------------------------ */
    /* A press that starts 20 ms before the uint32 millisecond rollover must
     * still be accepted 15 ms later, on the other side of it. At ~49.7 days
     * of uptime, no bench session finds this. */
    esps_morse_key_init(&k, 15u, 0u);
    uint32_t near_wrap = 0xFFFFFFFFu - 20u;
    for (uint32_t i = 0; i < 400; i++) {
        pin[i] = 1u;
    }
    n = run_key(&k, pin, 60u, near_wrap, edges, 16);
    ESPS_CHECK_EQ(&fails, n, 1);
    if (n == 1) {
        ESPS_CHECK_EQ(&fails, edges[0].level, 1);
        /* accepted 15 ms after the first poll, wrapping through zero */
        ESPS_CHECK_EQ(&fails, edges[0].ms, (uint32_t)(near_wrap + 15u));
    }

    return fails;
}

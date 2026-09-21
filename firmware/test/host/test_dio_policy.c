/* esps_dio_policy: the link's decisions that need no hardware.
 *
 * The millisecond-wrap cases are the reason this file exists. The node's
 * clock is a uint32 of milliseconds (D-10) and wraps after ~49.7 days; a
 * scheduler or rate limiter written with plain `>=` comparisons works
 * perfectly on a bench and then stops working after seven weeks of uptime.
 * That is not reachable by any hardware test, so it is pinned here.
 */
#include "esps_dio_policy.h"
#include "harness.h"

#include <string.h>

/* --- set_gpio validation ------------------------------------------------- */

static int test_gpio_validate(void) {
    int fails = 0;

    /* The allow-list is checked first, so the pins that brick or destabilise a
     * board report "pin_not_allowed" even when something else is also wrong. */
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(12, 1, true), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(12, 1, false), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(12, 7, false), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    for (int gpio = 6; gpio <= 11; gpio++) {
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(gpio, 0, true), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    }
    for (int gpio = 34; gpio <= 39; gpio++) {
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(gpio, 0, true), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    }
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(0, 0, true), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(2, 0, true), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(5, 0, true), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(15, 0, true), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(-1, 0, true), ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(99, 0, true), ESPS_DIO_ERR_PIN_NOT_ALLOWED);

    /* The link's four pins, each against both modes. SPEC-LINK.md "Pines del
     * enlace y modo manual": the INPUTS (14 RX_CLK, 25 RX_DATA) are refused in
     * BOTH modes, because the far end of those wires is the other board's
     * output and this node cannot see that. Only the OUTPUTS (26 TX_DATA,
     * 27 TX_CLK) are released in manual mode.
     *
     * The table is written out pin by pin rather than looped over a "link
     * pins" set, because the whole point of the rule is that the four are no
     * longer interchangeable. */
    struct {
        int gpio;
        esps_dio_result_t linked; /* manual_mode = false */
        esps_dio_result_t manual; /* manual_mode = true  */
    } cases[] = {
        {14, ESPS_DIO_ERR_OWNED_BY_LINK, ESPS_DIO_ERR_OWNED_BY_LINK}, /* RX_CLK  */
        {25, ESPS_DIO_ERR_OWNED_BY_LINK, ESPS_DIO_ERR_OWNED_BY_LINK}, /* RX_DATA */
        {26, ESPS_DIO_ERR_OWNED_BY_LINK, ESPS_DIO_OK},                /* TX_DATA */
        {27, ESPS_DIO_ERR_OWNED_BY_LINK, ESPS_DIO_OK},                /* TX_CLK  */
        {4, ESPS_DIO_OK, ESPS_DIO_OK},                                /* LED     */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(cases[i].gpio, 1, false), cases[i].linked);
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(cases[i].gpio, 0, false), cases[i].linked);
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(cases[i].gpio, 1, true), cases[i].manual);
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(cases[i].gpio, 0, true), cases[i].manual);
    }

    /* The input/output split the rule rests on. */
    ESPS_CHECK(&fails, esps_dio_pin_is_link_input(14));
    ESPS_CHECK(&fails, esps_dio_pin_is_link_input(25));
    ESPS_CHECK(&fails, !esps_dio_pin_is_link_input(26));
    ESPS_CHECK(&fails, !esps_dio_pin_is_link_input(27));
    ESPS_CHECK(&fails, esps_dio_pin_is_link_output(26));
    ESPS_CHECK(&fails, esps_dio_pin_is_link_output(27));
    ESPS_CHECK(&fails, !esps_dio_pin_is_link_output(14));
    ESPS_CHECK(&fails, !esps_dio_pin_is_link_output(25));
    for (int g = 0; g <= 40; g++) {
        const bool owned = (g == 14 || g == 25 || g == 26 || g == 27);
        ESPS_CHECK_EQ(&fails, esps_dio_pin_is_link_owned(g), owned);
        ESPS_CHECK_EQ(&fails, esps_dio_pin_is_link_input(g) || esps_dio_pin_is_link_output(g),
                      owned);
    }

    /* GPIO4 is the witness LED, explicitly NOT link-owned per the spec. */
    ESPS_CHECK(&fails, !esps_dio_pin_is_link_owned(4));

    /* Free allowed pins, both levels, both modes. */
    static const int free_pins[] = {13, 16, 17, 18, 19, 21, 22, 23, 32, 33};
    for (size_t i = 0; i < sizeof(free_pins) / sizeof(free_pins[0]); i++) {
        ESPS_CHECK(&fails, !esps_dio_pin_is_link_owned(free_pins[i]));
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(free_pins[i], 0, false), ESPS_DIO_OK);
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(free_pins[i], 1, false), ESPS_DIO_OK);
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(free_pins[i], 1, true), ESPS_DIO_OK);
    }

    /* Forbidden pins stay forbidden in manual mode too — manual mode releases
     * the link's own outputs, it is not an override of the allow-list. */
    static const int never[] = {12, 8, 34, 99, 0, 2, 5, 15, -1};
    for (size_t i = 0; i < sizeof(never) / sizeof(never[0]); i++) {
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(never[i], 1, false),
                      ESPS_DIO_ERR_PIN_NOT_ALLOWED);
        ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(never[i], 1, true),
                      ESPS_DIO_ERR_PIN_NOT_ALLOWED);
    }

    /* Level range is checked after ownership, so an allowed free pin with a
     * nonsense level is the case that reports bad_level. A link output in
     * manual mode reaches the level check too. */
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(13, 2, false), ESPS_DIO_ERR_BAD_LEVEL);
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(13, -1, false), ESPS_DIO_ERR_BAD_LEVEL);
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(26, 2, true), ESPS_DIO_ERR_BAD_LEVEL);
    /* ...but a link input is refused before the level is even looked at. */
    ESPS_CHECK_EQ(&fails, esps_dio_gpio_validate(25, 2, true), ESPS_DIO_ERR_OWNED_BY_LINK);

    /* The reason strings go into dio.gpio_rejected verbatim; the station and
     * the simulator match on them, so they are contract, not debug text. */
    ESPS_CHECK(&fails, strcmp(esps_dio_result_str(ESPS_DIO_OK), "ok") == 0);
    ESPS_CHECK(&fails,
               strcmp(esps_dio_result_str(ESPS_DIO_ERR_PIN_NOT_ALLOWED), "pin_not_allowed") == 0);
    ESPS_CHECK(&fails,
               strcmp(esps_dio_result_str(ESPS_DIO_ERR_OWNED_BY_LINK), "owned_by_link") == 0);
    ESPS_CHECK(&fails, strcmp(esps_dio_result_str(ESPS_DIO_ERR_BAD_LEVEL), "bad_level") == 0);
    ESPS_CHECK(&fails, strcmp(esps_dio_result_str(ESPS_DIO_ERR_NOT_READY), "not_ready") == 0);
    ESPS_CHECK(&fails, strcmp(esps_dio_result_str(ESPS_DIO_ERR_DRIVER), "driver_error") == 0);
    ESPS_CHECK(&fails, esps_dio_result_str((esps_dio_result_t)99) != NULL);

    return fails;
}

/* --- link up/down hysteresis --------------------------------------------- */

static int test_linkwatch(void) {
    int fails = 0;
    esps_dio_linkwatch_t w;

    /* SPEC-LINK.md: lost after exactly 3 consecutive timeouts, up on the first
     * success since boot or since lost. */
    esps_dio_linkwatch_init(&w, 3, 1);
    ESPS_CHECK(&fails, !esps_dio_linkwatch_is_up(&w)); /* no claim before evidence */

    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, true), ESPS_DIO_LINK_WENT_UP);
    ESPS_CHECK(&fails, esps_dio_linkwatch_is_up(&w));
    /* Further successes are not repeated events. */
    for (int i = 0; i < 10; i++) {
        ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, true), ESPS_DIO_LINK_NO_CHANGE);
    }

    /* Exactly on the third timeout, not the second and not the fourth. */
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK(&fails, esps_dio_linkwatch_is_up(&w));
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_WENT_DOWN);
    ESPS_CHECK(&fails, !esps_dio_linkwatch_is_up(&w));
    for (int i = 0; i < 10; i++) {
        ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_NO_CHANGE);
    }
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, true), ESPS_DIO_LINK_WENT_UP);

    /* A success in the middle of a bad run resets the count: two timeouts, a
     * success, two more timeouts must NOT declare the link lost. */
    esps_dio_linkwatch_init(&w, 3, 1);
    (void)esps_dio_linkwatch_update(&w, true);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, true), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK(&fails, esps_dio_linkwatch_is_up(&w));

    /* A node that boots with the cable already out reports lost, not up. */
    esps_dio_linkwatch_init(&w, 3, 1);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, false), ESPS_DIO_LINK_WENT_DOWN);

    /* Thresholds of 0 are clamped to 1 rather than firing on every call. */
    esps_dio_linkwatch_init(&w, 0, 0);
    ESPS_CHECK_EQ(&fails, w.lost_after, 1);
    ESPS_CHECK_EQ(&fails, w.up_after, 1);

    /* A higher up_after needs that many consecutive successes. */
    esps_dio_linkwatch_init(&w, 2, 3);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, true), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, true), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(&w, true), ESPS_DIO_LINK_WENT_UP);

    ESPS_CHECK_EQ(&fails, esps_dio_linkwatch_update(NULL, true), ESPS_DIO_LINK_NO_CHANGE);
    ESPS_CHECK(&fails, !esps_dio_linkwatch_is_up(NULL));

    return fails;
}

/* --- event rate limiting -------------------------------------------------- */

static int test_ratelimit(void) {
    int fails = 0;
    esps_dio_ratelimit_t rl;

    /* dio.edge: at most 5 per second. */
    esps_dio_ratelimit_init(&rl, 1000, 5);
    for (int i = 0; i < 5; i++) {
        ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 10000));
    }
    for (int i = 0; i < 20; i++) {
        ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 10000));
    }
    ESPS_CHECK_EQ(&fails, esps_dio_ratelimit_take_suppressed(&rl), 20);
    ESPS_CHECK_EQ(&fails, esps_dio_ratelimit_take_suppressed(&rl), 0); /* cleared by the read */

    /* Still inside the window at +999 ms, a new window at +1000 ms. */
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 10999));
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 11000));

    /* max_in_window == 0 silences the event entirely. */
    esps_dio_ratelimit_init(&rl, 1000, 0);
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 0));
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 5000));
    ESPS_CHECK_EQ(&fails, esps_dio_ratelimit_take_suppressed(&rl), 2);

    /* One per burst: the shape used for link.frame_ok. */
    esps_dio_ratelimit_init(&rl, 5000, 1);
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 0));
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 4999));
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 5000));

    /* Millisecond wrap: a window opened just before the uint32 rollover must
     * still be the same window a few hundred ms later, on the far side of it. */
    esps_dio_ratelimit_init(&rl, 1000, 2);
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 0xFFFFFFF0u));
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 0x0000000Au)); /* +26 ms, wrapped */
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 0x00000064u)); /* +116 ms, still full */
    /* 0xFFFFFFF0 + 1000 wraps to 0x000003D8, and the boundary is exact: one
     * millisecond earlier is still the old window. */
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 0x000003D7u));
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 0x000003D8u));

    /* --- reference semantics, for the simulator port -------------------
     * SPEC-LINK.md defers the exact meaning of "N per W ms" to this
     * implementation, so the properties a second implementation has to match
     * are pinned here rather than left to prose.
     *
     * (1) The window RESTARTS at the first call that finds the previous one
     *     expired — it is not a fixed grid anchored at t=0. A long silence
     *     therefore never banks up allowances. */
    esps_dio_ratelimit_init(&rl, 1000, 2);
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 100));   /* window [100,1100) */
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 100));
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 1099)); /* still the same window */
    (void)esps_dio_ratelimit_take_suppressed(&rl);
    /* Nothing at all happens for 10 s. The next event opens a window at ITS
     * timestamp and gets exactly N again — not 20 banked allowances. */
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 11000)); /* window [11000,12000) */
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 11500));
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 11999));

    /* (2) `suppressed` counts only the calls that were refused, accumulates
     *     across windows until read, and is cleared by the read — so the value
     *     attached to an emitted event is "how many were dropped since the
     *     previous emitted one", never a running total. */
    esps_dio_ratelimit_init(&rl, 1000, 1);
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 0));
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 10));
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 20));
    ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 1000)); /* new window */
    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 1010));
    /* 3 refusals across two windows, still unread. */
    ESPS_CHECK_EQ(&fails, esps_dio_ratelimit_take_suppressed(&rl), 3);
    ESPS_CHECK_EQ(&fails, esps_dio_ratelimit_take_suppressed(&rl), 0);

    /* (3) An allowed call never touches the suppressed counter. */
    esps_dio_ratelimit_init(&rl, 1000, 5);
    for (int i = 0; i < 5; i++) {
        ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 0));
    }
    ESPS_CHECK_EQ(&fails, esps_dio_ratelimit_take_suppressed(&rl), 0);

    /* (4) The four budgets the link actually ships with, as the spec table
     *     states them. */
    struct {
        uint32_t window_ms;
        uint16_t max;
    } budgets[] = {{1000, 5}, {5000, 1}, {5000, 2}, {5000, 1}};
    for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
        esps_dio_ratelimit_init(&rl, budgets[i].window_ms, budgets[i].max);
        for (uint16_t k = 0; k < budgets[i].max; k++) {
            ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 500));
        }
        ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 500));
        ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(&rl, 500 + budgets[i].window_ms - 1));
        ESPS_CHECK(&fails, esps_dio_ratelimit_allow(&rl, 500 + budgets[i].window_ms));
    }

    ESPS_CHECK(&fails, !esps_dio_ratelimit_allow(NULL, 0));
    ESPS_CHECK_EQ(&fails, esps_dio_ratelimit_take_suppressed(NULL), 0);

    return fails;
}

/* --- RTT accounting -------------------------------------------------------- */

static int test_rtt(void) {
    int fails = 0;
    esps_dio_rtt_t r;

    esps_dio_rtt_init(&r);
    /* Nothing measured yet: min must read 0, not UINT32_MAX, or the station
     * would plot a 71-minute round trip. */
    ESPS_CHECK_EQ(&fails, esps_dio_rtt_min_us(&r), 0);
    ESPS_CHECK_EQ(&fails, esps_dio_rtt_mean_us(&r), 0);
    ESPS_CHECK(&fails, !r.last_ok);

    esps_dio_rtt_add(&r, 40);
    esps_dio_rtt_add(&r, 20);
    esps_dio_rtt_add(&r, 60);
    ESPS_CHECK_EQ(&fails, r.last_us, 60);
    ESPS_CHECK_EQ(&fails, esps_dio_rtt_min_us(&r), 20);
    ESPS_CHECK_EQ(&fails, r.max_us, 60);
    ESPS_CHECK_EQ(&fails, esps_dio_rtt_mean_us(&r), 40);
    ESPS_CHECK_EQ(&fails, r.samples, 3);
    ESPS_CHECK(&fails, r.last_ok);

    /* A timeout clears last_ok (so link.rtt_us is not published) but leaves
     * last_us alone — a skipped sample must never become a fake 0 µs one. */
    esps_dio_rtt_timeout(&r);
    ESPS_CHECK(&fails, !r.last_ok);
    ESPS_CHECK_EQ(&fails, r.last_us, 60);
    ESPS_CHECK_EQ(&fails, r.timeouts, 1);
    ESPS_CHECK_EQ(&fails, r.samples, 3);

    esps_dio_rtt_add(&r, 100);
    ESPS_CHECK(&fails, r.last_ok);
    ESPS_CHECK_EQ(&fails, r.max_us, 100);

    /* The 64-bit accumulator must not overflow on long runs: 2^32-1 µs each,
     * many times over, is far past what a uint32 sum would survive. */
    esps_dio_rtt_init(&r);
    for (int i = 0; i < 1000; i++) {
        esps_dio_rtt_add(&r, 0xFFFFFFFFu);
    }
    ESPS_CHECK_EQ(&fails, esps_dio_rtt_mean_us(&r), 0xFFFFFFFFu);

    esps_dio_rtt_init(NULL);
    esps_dio_rtt_add(NULL, 1);
    esps_dio_rtt_timeout(NULL);
    ESPS_CHECK_EQ(&fails, esps_dio_rtt_min_us(NULL), 0);
    ESPS_CHECK_EQ(&fails, esps_dio_rtt_mean_us(NULL), 0);

    return fails;
}

/* --- phase scheduling ------------------------------------------------------- */

static int test_sched(void) {
    int fails = 0;
    esps_dio_sched_t s;

    /* The shipping cadence: a handshake every second, a burst every ten. */
    esps_dio_sched_init(&s, 0, 1000, 10000);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 0), ESPS_DIO_PHASE_IDLE);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_delay_ms(&s, 0), 1000);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 999), ESPS_DIO_PHASE_IDLE);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 1000), ESPS_DIO_PHASE_N2);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 1000), ESPS_DIO_PHASE_IDLE); /* consumed */

    int n2 = 0, n3 = 0;
    for (uint32_t t = 1001; t <= 10000; t++) {
        switch (esps_dio_sched_due(&s, t)) {
        case ESPS_DIO_PHASE_N2:
            n2++;
            break;
        case ESPS_DIO_PHASE_N3_BURST:
            n3++;
            break;
        default:
            break;
        }
    }
    /* Handshakes fall due at 2000..10000, but t=10000 is served as the burst
     * (N3 wins the tie), so 8 handshakes and 1 burst. */
    ESPS_CHECK_EQ(&fails, n2, 8);
    ESPS_CHECK_EQ(&fails, n3, 1);

    /* When both are due on the same tick, the burst wins and the handshake is
     * served on the following call. */
    esps_dio_sched_init(&s, 0, 1000, 1000);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 1000), ESPS_DIO_PHASE_N3_BURST);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 1000), ESPS_DIO_PHASE_N2);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 1000), ESPS_DIO_PHASE_IDLE);

    /* Overrun: a phase that ran for 5 s must not queue up 5 handshakes to run
     * back to back. One fires, the rest are dropped and counted. */
    esps_dio_sched_init(&s, 0, 1000, 0);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 6000), ESPS_DIO_PHASE_N2);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 6000), ESPS_DIO_PHASE_IDLE);
    ESPS_CHECK_EQ(&fails, s.overruns, 1);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_delay_ms(&s, 6000), 1000);

    /* A period of 0 disables that phase entirely. */
    esps_dio_sched_init(&s, 0, 0, 0);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 1000000), ESPS_DIO_PHASE_IDLE);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_delay_ms(&s, 0), 0xFFFFFFFFu);

    /* Millisecond wrap. Scheduled 1000 ms before the rollover, the deadline
     * lands at 744 on the far side and must not be treated as already past. */
    esps_dio_sched_init(&s, 0xFFFFFF00u, 1000, 0);
    ESPS_CHECK_EQ(&fails, s.next_n2_ms, 744u);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 0xFFFFFF00u), ESPS_DIO_PHASE_IDLE);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_delay_ms(&s, 0xFFFFFF00u), 1000);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 0xFFFFFFFFu), ESPS_DIO_PHASE_IDLE);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 743u), ESPS_DIO_PHASE_IDLE);
    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(&s, 744u), ESPS_DIO_PHASE_N2);
    ESPS_CHECK_EQ(&fails, s.overruns, 0); /* a wrap is not an overrun */

    /* And it keeps ticking normally across the boundary. */
    for (uint32_t t = 745; t <= 5000; t++) {
        (void)esps_dio_sched_due(&s, t);
    }
    ESPS_CHECK_EQ(&fails, s.overruns, 0);

    ESPS_CHECK_EQ(&fails, esps_dio_sched_due(NULL, 0), ESPS_DIO_PHASE_IDLE);

    return fails;
}

/* --- receiver stall detection ------------------------------------------------ */

static int test_rx_stalled(void) {
    int fails = 0;

    /* An idle line in HUNT is not a stall, however long it stays idle. */
    ESPS_CHECK(&fails, !esps_dio_rx_stalled(ESPS_DIO_RXS_HUNT, 1000000, 200));

    /* Parked mid-frame is: this is a cable pulled out during a burst. */
    ESPS_CHECK(&fails, !esps_dio_rx_stalled(ESPS_DIO_RXS_LEN, 199, 200));
    ESPS_CHECK(&fails, esps_dio_rx_stalled(ESPS_DIO_RXS_LEN, 200, 200));
    ESPS_CHECK(&fails, esps_dio_rx_stalled(ESPS_DIO_RXS_PAYLOAD, 5000, 200));
    ESPS_CHECK(&fails, esps_dio_rx_stalled(ESPS_DIO_RXS_CRC, 201, 200));

    /* 0 disables the check. */
    ESPS_CHECK(&fails, !esps_dio_rx_stalled(ESPS_DIO_RXS_PAYLOAD, 0xFFFFFFFFu, 0));

    return fails;
}

int test_dio_policy_all(void) {
    int fails = 0;
    fails += test_gpio_validate();
    fails += test_linkwatch();
    fails += test_ratelimit();
    fails += test_rtt();
    fails += test_sched();
    fails += test_rx_stalled();
    return fails;
}

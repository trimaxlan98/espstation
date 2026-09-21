#include "esps_dio_policy.h"

#include "esps_dio_pins.h"

/* Every deadline comparison in this file goes through this helper rather than
 * `now >= deadline`. The node's clock is a uint32 of milliseconds and wraps at
 * ~49.7 days (D-10); a plain >= comparison declares every deadline "due"
 * forever on the far side of the wrap, which is a node that stops scheduling
 * after seven weeks of uptime — exactly the kind of failure nobody finds on a
 * bench. The unsigned difference cast to int32 is correct as long as no
 * interval exceeds ~24.8 days, which no link phase does. */
static bool time_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t sat_add_u16(uint16_t v) {
    return (v == 0xFFFFu) ? 0xFFFFu : (uint32_t)(v + 1u);
}

static uint32_t sat_inc_u32(uint32_t v) {
    return (v == 0xFFFFFFFFu) ? v : v + 1u;
}

/* --- Result codes -------------------------------------------------------- */

const char *esps_dio_result_str(esps_dio_result_t result) {
    switch (result) {
    case ESPS_DIO_OK:
        return "ok";
    case ESPS_DIO_ERR_PIN_NOT_ALLOWED:
        return "pin_not_allowed";
    case ESPS_DIO_ERR_OWNED_BY_LINK:
        return "owned_by_link";
    case ESPS_DIO_ERR_BAD_LEVEL:
        return "bad_level";
    case ESPS_DIO_ERR_NOT_READY:
        return "not_ready";
    case ESPS_DIO_ERR_DRIVER:
        return "driver_error";
    default:
        return "unknown";
    }
}

bool esps_dio_pin_is_link_input(int gpio) {
    return gpio == ESPS_DIO_PIN_RX_CLK || gpio == ESPS_DIO_PIN_RX_DATA;
}

bool esps_dio_pin_is_link_output(int gpio) {
    return gpio == ESPS_DIO_PIN_TX_DATA || gpio == ESPS_DIO_PIN_TX_CLK;
}

bool esps_dio_pin_is_link_owned(int gpio) {
    return esps_dio_pin_is_link_input(gpio) || esps_dio_pin_is_link_output(gpio);
}

esps_dio_result_t esps_dio_gpio_validate(int gpio, int level, bool manual_mode) {
    /* Allow-list first: a GPIO12 request must be rejected as "not allowed"
     * whatever else is wrong with it, because that is the rejection an
     * operator needs to see (a high level on MTDI at reset selects 1.8 V
     * flash and the board stops booting). */
    if (!esps_dio_pin_is_allowed_output(gpio)) {
        return ESPS_DIO_ERR_PIN_NOT_ALLOWED;
    }
    /* The link's inputs are refused unconditionally. Manual mode governs what
     * this node does with its own outputs; it says nothing about what the
     * other board is driving into these two pins, and this node has no way to
     * find out. */
    if (esps_dio_pin_is_link_input(gpio)) {
        return ESPS_DIO_ERR_OWNED_BY_LINK;
    }
    /* The link's outputs are ours to release once no phase task drives them. */
    if (!manual_mode && esps_dio_pin_is_link_output(gpio)) {
        return ESPS_DIO_ERR_OWNED_BY_LINK;
    }
    if (level != 0 && level != 1) {
        return ESPS_DIO_ERR_BAD_LEVEL;
    }
    return ESPS_DIO_OK;
}

/* --- Link up/down hysteresis --------------------------------------------- */

void esps_dio_linkwatch_init(esps_dio_linkwatch_t *w, uint16_t lost_after, uint16_t up_after) {
    if (w == NULL) {
        return;
    }
    w->lost_after = (lost_after < 1u) ? 1u : lost_after;
    w->up_after = (up_after < 1u) ? 1u : up_after;
    w->consec_bad = 0;
    w->consec_good = 0;
    w->up = false;
    w->known = false;
}

esps_dio_link_edge_t esps_dio_linkwatch_update(esps_dio_linkwatch_t *w, bool ok) {
    if (w == NULL) {
        return ESPS_DIO_LINK_NO_CHANGE;
    }
    if (ok) {
        w->consec_bad = 0;
        w->consec_good = (uint16_t)sat_add_u16(w->consec_good);
        if ((!w->known || !w->up) && w->consec_good >= w->up_after) {
            w->known = true;
            w->up = true;
            return ESPS_DIO_LINK_WENT_UP;
        }
        return ESPS_DIO_LINK_NO_CHANGE;
    }
    w->consec_good = 0;
    w->consec_bad = (uint16_t)sat_add_u16(w->consec_bad);
    if ((!w->known || w->up) && w->consec_bad >= w->lost_after) {
        w->known = true;
        w->up = false;
        return ESPS_DIO_LINK_WENT_DOWN;
    }
    return ESPS_DIO_LINK_NO_CHANGE;
}

bool esps_dio_linkwatch_is_up(const esps_dio_linkwatch_t *w) {
    return (w != NULL) && w->known && w->up;
}

/* --- Event rate limiting -------------------------------------------------- */

void esps_dio_ratelimit_init(esps_dio_ratelimit_t *rl, uint32_t window_ms, uint16_t max_in_window) {
    if (rl == NULL) {
        return;
    }
    rl->window_ms = window_ms;
    rl->window_start_ms = 0;
    rl->suppressed = 0;
    rl->max_in_window = max_in_window;
    rl->count = 0;
    rl->started = false;
}

bool esps_dio_ratelimit_allow(esps_dio_ratelimit_t *rl, uint32_t now_ms) {
    if (rl == NULL) {
        return false;
    }
    /* A zero-length window is "no limit": the window expires on every call, so
     * the budget refreshes every time. */
    if (!rl->started || (uint32_t)(now_ms - rl->window_start_ms) >= rl->window_ms) {
        rl->started = true;
        rl->window_start_ms = now_ms;
        rl->count = 0;
    }
    if (rl->count < rl->max_in_window) {
        rl->count++;
        return true;
    }
    rl->suppressed = sat_inc_u32(rl->suppressed);
    return false;
}

uint32_t esps_dio_ratelimit_take_suppressed(esps_dio_ratelimit_t *rl) {
    if (rl == NULL) {
        return 0;
    }
    const uint32_t n = rl->suppressed;
    rl->suppressed = 0;
    return n;
}

/* --- Round-trip time accounting ------------------------------------------- */

void esps_dio_rtt_init(esps_dio_rtt_t *r) {
    if (r == NULL) {
        return;
    }
    r->last_us = 0;
    r->min_us = 0xFFFFFFFFu;
    r->max_us = 0;
    r->samples = 0;
    r->timeouts = 0;
    r->sum_us = 0;
    r->last_ok = false;
}

void esps_dio_rtt_add(esps_dio_rtt_t *r, uint32_t rtt_us) {
    if (r == NULL) {
        return;
    }
    r->last_us = rtt_us;
    r->last_ok = true;
    if (rtt_us < r->min_us) {
        r->min_us = rtt_us;
    }
    if (rtt_us > r->max_us) {
        r->max_us = rtt_us;
    }
    r->sum_us += rtt_us;
    r->samples = sat_inc_u32(r->samples);
}

void esps_dio_rtt_timeout(esps_dio_rtt_t *r) {
    if (r == NULL) {
        return;
    }
    /* last_us is deliberately left at its previous value rather than zeroed:
     * SPEC-LINK.md says link.rtt_us is not *published* on a timeout, and
     * last_ok is what tells the caller to skip the sample. Zeroing would turn
     * a skipped sample into a plausible-looking 0 µs round trip if anyone
     * later published it unconditionally. */
    r->last_ok = false;
    r->timeouts = sat_inc_u32(r->timeouts);
}

uint32_t esps_dio_rtt_min_us(const esps_dio_rtt_t *r) {
    if (r == NULL || r->samples == 0) {
        return 0;
    }
    return r->min_us;
}

uint32_t esps_dio_rtt_mean_us(const esps_dio_rtt_t *r) {
    if (r == NULL || r->samples == 0) {
        return 0;
    }
    return (uint32_t)(r->sum_us / r->samples);
}

/* --- Phase scheduling ----------------------------------------------------- */

void esps_dio_sched_init(esps_dio_sched_t *s, uint32_t now_ms, uint32_t n2_period_ms,
                         uint32_t n3_period_ms) {
    if (s == NULL) {
        return;
    }
    s->n2_period_ms = n2_period_ms;
    s->n3_period_ms = n3_period_ms;
    s->next_n2_ms = now_ms + n2_period_ms;
    s->next_n3_ms = now_ms + n3_period_ms;
    s->overruns = 0;
}

/* Advances one deadline by its period, re-anchoring instead of catching up if
 * the new deadline is still in the past. */
static void advance(uint32_t *deadline, uint32_t period, uint32_t now_ms, uint32_t *overruns) {
    *deadline += period;
    if (time_reached(now_ms, *deadline)) {
        *deadline = now_ms + period;
        *overruns = sat_inc_u32(*overruns);
    }
}

esps_dio_phase_t esps_dio_sched_due(esps_dio_sched_t *s, uint32_t now_ms) {
    if (s == NULL) {
        return ESPS_DIO_PHASE_IDLE;
    }
    if (s->n3_period_ms != 0 && time_reached(now_ms, s->next_n3_ms)) {
        advance(&s->next_n3_ms, s->n3_period_ms, now_ms, &s->overruns);
        return ESPS_DIO_PHASE_N3_BURST;
    }
    if (s->n2_period_ms != 0 && time_reached(now_ms, s->next_n2_ms)) {
        advance(&s->next_n2_ms, s->n2_period_ms, now_ms, &s->overruns);
        return ESPS_DIO_PHASE_N2;
    }
    return ESPS_DIO_PHASE_IDLE;
}

uint32_t esps_dio_sched_delay_ms(const esps_dio_sched_t *s, uint32_t now_ms) {
    if (s == NULL) {
        return 0xFFFFFFFFu;
    }
    uint32_t best = 0xFFFFFFFFu;
    if (s->n2_period_ms != 0) {
        best = time_reached(now_ms, s->next_n2_ms) ? 0u : (s->next_n2_ms - now_ms);
    }
    if (s->n3_period_ms != 0) {
        const uint32_t d = time_reached(now_ms, s->next_n3_ms) ? 0u : (s->next_n3_ms - now_ms);
        if (d < best) {
            best = d;
        }
    }
    return best;
}

/* --- Receiver stall detection ---------------------------------------------- */

bool esps_dio_rx_stalled(esps_dio_rx_state_t state, uint32_t ms_since_last_bit, uint32_t idle_ms) {
    if (idle_ms == 0 || state == ESPS_DIO_RXS_HUNT) {
        return false;
    }
    return ms_since_last_bit >= idle_ms;
}

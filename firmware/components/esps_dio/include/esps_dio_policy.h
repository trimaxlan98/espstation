/* Everything the digital link decides that needs no hardware to decide it:
 * link up/down hysteresis, event rate limiting, RTT accounting, the phase
 * cycle's schedule, mid-frame stall detection and the `set_gpio` allow-list
 * check.
 *
 * This file exists because the alternative is burying those rules inside the
 * ESP-IDF layer, where the only way to exercise them is to flash two boards
 * and wait. Every rule here is a pure function over caller-owned state, so
 * test/host/test_dio_policy.c can drive it — including the uint32 millisecond
 * wrap at ~49.7 days (D-10), which no bench session is ever going to reach.
 *
 * Pure C11: only <stdbool.h> <stddef.h> <stdint.h> and the sibling esps_dio
 * headers. No ESP-IDF, no allocation, no logging, no globals.
 *
 * Time is passed in as `now_ms`, the node's monotonic millisecond clock
 * (esps_time_now_ms() on the target). All comparisons use unsigned
 * subtraction, so they stay correct across the wrap; none of them compare
 * two absolute timestamps with < or >.
 */
#ifndef ESPS_DIO_POLICY_H
#define ESPS_DIO_POLICY_H

#include "esps_dio_frame.h" /* esps_dio_rx_state_t */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Result codes, shared with esps_dio_set_gpio() ---------------------- */

typedef enum {
    ESPS_DIO_OK = 0,
    ESPS_DIO_ERR_PIN_NOT_ALLOWED, /* not in SPEC-LINK.md's output allow-list   */
    ESPS_DIO_ERR_OWNED_BY_LINK,   /* the link drives this pin in this build    */
    ESPS_DIO_ERR_BAD_LEVEL,       /* level is neither 0 nor 1                  */
    ESPS_DIO_ERR_NOT_READY,       /* esps_dio_start() has not completed        */
    ESPS_DIO_ERR_DRIVER,          /* the ESP-IDF gpio call itself failed       */
} esps_dio_result_t;

/* Stable, machine-readable strings for the `reason` field of the
 * dio.gpio_rejected event (SPEC-LINK.md "Semántica de eventos y de set_gpio").
 * Never NULL, so a caller can hand the result straight to a JSON builder. */
const char *esps_dio_result_str(esps_dio_result_t result);

/* The link's two INPUTS: 14 (RX_CLK) and 25 (RX_DATA).
 *
 * These are rejected by set_gpio ALWAYS — manual mode included. A node cannot
 * see what is on the far end of the cable, and on the standard wiring the far
 * end is the other board's push-pull output. Driving our input high while that
 * output holds it low is two drivers fighting across a 330 ohm pair; the
 * resistors survive it, but nothing about it is a legitimate operation, and no
 * amount of "the operator asked for it" makes a node able to know it is safe.
 * Manual mode changes what THIS node does with its own outputs, not what is
 * physically attached to its inputs. */
bool esps_dio_pin_is_link_input(int gpio);

/* The link's two OUTPUTS: 26 (TX_DATA) and 27 (TX_CLK). Rejected while the
 * link programs phases, released in manual mode — there the node runs no
 * phases and dio.tx belongs to set_gpio, so there is no second driver on this
 * board to conflict with. */
bool esps_dio_pin_is_link_output(int gpio);

/* Either of the above: 14, 25, 26, 27.
 *
 * GPIO4 (the witness LED) is deliberately NOT in this set: SPEC-LINK.md lists
 * exactly those four, and the LED is written only at phase boundaries, so a
 * manual set_gpio on it simply gets overwritten at the next phase rather than
 * corrupting the link. */
bool esps_dio_pin_is_link_owned(int gpio);

/* The whole `set_gpio` precondition, with no hardware involved: allow-list,
 * link ownership and level range.
 *
 * `manual_mode` is true for a build with ESPS_DIO_MODE_MANUAL, where the node
 * programs no link phases. It releases the link's outputs (26/27) and nothing
 * else — the inputs stay rejected either way, for the reason on
 * esps_dio_pin_is_link_input(). Returns ESPS_DIO_OK when the write may
 * proceed. */
esps_dio_result_t esps_dio_gpio_validate(int gpio, int level, bool manual_mode);

/* --- Link up/down hysteresis -------------------------------------------- */

typedef enum {
    ESPS_DIO_LINK_NO_CHANGE = 0,
    ESPS_DIO_LINK_WENT_UP,   /* emit link.up   */
    ESPS_DIO_LINK_WENT_DOWN, /* emit link.lost */
} esps_dio_link_edge_t;

/* Consecutive-run hysteresis over handshake outcomes. SPEC-LINK.md fixes the
 * thresholds: link.lost after exactly 3 consecutive timeouts, link.up on the
 * first success since boot or since lost — i.e. lost_after=3, up_after=1. The
 * thresholds stay parameters anyway so the host tests can pin the boundary
 * behaviour at other values, and so a noisier cable can be tuned without
 * touching this logic. */
typedef struct esps_dio_linkwatch {
    uint16_t lost_after;   /* consecutive failures before declaring lost     */
    uint16_t up_after;     /* consecutive successes before declaring up      */
    uint16_t consec_bad;   /* saturating                                     */
    uint16_t consec_good;  /* saturating                                     */
    bool up;               /* current belief; meaningless while !known       */
    bool known;            /* false until the first verdict is reached       */
} esps_dio_linkwatch_t;

/* Thresholds below 1 are clamped to 1 — a threshold of 0 would mean "declare
 * the state before observing anything", which has no useful meaning and would
 * fire an event on the first call in both directions. */
void esps_dio_linkwatch_init(esps_dio_linkwatch_t *w, uint16_t lost_after, uint16_t up_after);

/* Feeds one handshake outcome. Returns WENT_UP / WENT_DOWN exactly on the
 * call that crosses a threshold, NO_CHANGE otherwise — so the caller emits
 * one event per transition and never has to de-duplicate. Starting state is
 * "unknown": the node does not claim the link is up at boot, and does not
 * claim it went down until it has actually failed lost_after times. */
esps_dio_link_edge_t esps_dio_linkwatch_update(esps_dio_linkwatch_t *w, bool ok);

/* False before the first verdict. */
bool esps_dio_linkwatch_is_up(const esps_dio_linkwatch_t *w);

/* --- Event rate limiting ------------------------------------------------ */

/* Fixed-window limiter. The window restarts at the first call that finds the
 * previous one expired, rather than marching on a fixed grid: for event
 * suppression that is both simpler and immune to a long gap producing a burst
 * of "catch-up" allowances. */
typedef struct esps_dio_ratelimit {
    uint32_t window_ms;
    uint32_t window_start_ms;
    uint32_t suppressed; /* saturating; cleared by take_suppressed()        */
    uint16_t max_in_window;
    uint16_t count;
    bool started;
} esps_dio_ratelimit_t;

/* max_in_window == 0 is legal and silences the event entirely (everything is
 * counted as suppressed), which is how an operator turns off a chatty event
 * without removing the call site. */
void esps_dio_ratelimit_init(esps_dio_ratelimit_t *rl, uint32_t window_ms, uint16_t max_in_window);

/* True when the event may be emitted now. A false return increments the
 * suppressed counter. */
bool esps_dio_ratelimit_allow(esps_dio_ratelimit_t *rl, uint32_t now_ms);

/* Reads and clears the suppressed counter — the value SPEC-LINK.md puts in
 * `data.suppressed` of the event that does get through, so the station can
 * see that it is looking at a sample of a larger stream rather than at
 * everything that happened. */
uint32_t esps_dio_ratelimit_take_suppressed(esps_dio_ratelimit_t *rl);

/* --- Round-trip time accounting ----------------------------------------- */

typedef struct esps_dio_rtt {
    uint32_t last_us;
    uint32_t min_us; /* UINT32_MAX while samples == 0                       */
    uint32_t max_us;
    uint32_t samples;  /* saturating */
    uint32_t timeouts; /* saturating */
    uint64_t sum_us;
    bool last_ok; /* false after a timeout: link.rtt_us must not be published */
} esps_dio_rtt_t;

void esps_dio_rtt_init(esps_dio_rtt_t *r);
void esps_dio_rtt_add(esps_dio_rtt_t *r, uint32_t rtt_us);
void esps_dio_rtt_timeout(esps_dio_rtt_t *r);

/* 0 when no sample has been taken (rather than UINT32_MAX, which would reach
 * the station as a real-looking 71-minute round trip). */
uint32_t esps_dio_rtt_min_us(const esps_dio_rtt_t *r);
uint32_t esps_dio_rtt_mean_us(const esps_dio_rtt_t *r);

/* --- Phase scheduling ---------------------------------------------------- */

typedef enum {
    ESPS_DIO_PHASE_IDLE = 0,
    ESPS_DIO_PHASE_N2,       /* handshake + RTT measurement */
    ESPS_DIO_PHASE_N3_BURST, /* burst of framed test payloads */
} esps_dio_phase_t;

typedef struct esps_dio_sched {
    uint32_t n2_period_ms; /* 0 disables the handshake phase */
    uint32_t n3_period_ms; /* 0 disables the burst phase     */
    uint32_t next_n2_ms;
    uint32_t next_n3_ms;
    uint32_t overruns; /* deadlines dropped because a phase ran long */
} esps_dio_sched_t;

void esps_dio_sched_init(esps_dio_sched_t *s, uint32_t now_ms, uint32_t n2_period_ms,
                         uint32_t n3_period_ms);

/* Returns the phase to run now and consumes its deadline, or IDLE. N3 wins a
 * tie: the burst is the expensive phase and the one whose cadence carries the
 * measurement, so a handshake waits for it rather than the other way round.
 *
 * A deadline that is already in the past by more than one period is
 * re-anchored to now + period instead of being caught up. Catching up would
 * turn one overrun — a long burst, a station command, a flash write — into a
 * back-to-back storm of phases, which on this link means transmitting with no
 * inter-frame gap. `overruns` counts those, so the condition is visible rather
 * than silently absorbed. */
esps_dio_phase_t esps_dio_sched_due(esps_dio_sched_t *s, uint32_t now_ms);

/* Milliseconds until the earliest enabled deadline, 0 if one is already due,
 * and UINT32_MAX if nothing is scheduled at all. The caller is expected to cap
 * this before sleeping on it — the scheduler does not know about the other
 * work the task has to do between phases. */
uint32_t esps_dio_sched_delay_ms(const esps_dio_sched_t *s, uint32_t now_ms);

/* --- Receiver stall detection -------------------------------------------- */

/* True when the bit-serial receiver has been parked mid-frame with no new bit
 * for idle_ms.
 *
 * This is what a cable pulled out mid-burst looks like from the receiving end:
 * the state machine is sitting in LEN/PAYLOAD/CRC waiting for bits that will
 * never come, and nothing in the frame format times out on its own. Left
 * alone, the first frames of the *next* burst get shifted into the stale
 * frame's payload and are lost. The caller re-inits the receiver when this
 * returns true.
 *
 * idle_ms == 0 disables the check. HUNT is never stalled — sitting in HUNT
 * with no bits is just an idle line. */
bool esps_dio_rx_stalled(esps_dio_rx_state_t state, uint32_t ms_since_last_bit, uint32_t idle_ms);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_DIO_POLICY_H */

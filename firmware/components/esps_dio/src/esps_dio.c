/* ESP-IDF layer of the two-wire digital link. See esps_dio.h for the API and
 * SPEC-LINK.md for the wire contract.
 *
 * ============================================================================
 * WHY THE ISRs ARE IN IRAM, AND WHY THE PURE CODE THEY CALL IS TOO
 * ============================================================================
 * The GPIO ISR service is installed with ESP_INTR_FLAG_IRAM, and linker.lf
 * places esps_dio/src/frame.c and esps_dio/src/crc8.c in IRAM. That costs a
 * few hundred bytes of IRAM, and the alternative was not "save the IRAM" but
 * "silently corrupt the measurement":
 *
 * Without the flag, the ISR is masked whenever the flash cache is disabled.
 * On this node the cache goes down for an NVS write — node.set_label from the
 * station, and the boot-count commit at startup — for a window measured in
 * hundreds of microseconds to milliseconds. At the default 10 kbit/s a bit
 * period is 100 us, so such a window swallows entire runs of clock edges. The
 * receiver's shift register then holds half of one frame and half of the
 * next: frames_err climbs, BER climbs, and nothing anywhere says the cause
 * was this node writing its own flash rather than the cable under test.
 *
 * This component exists to measure a link's error rate. A measurement
 * instrument that injects its own errors, invisibly, is worse than no
 * instrument. So the whole receive path is cache-independent:
 *   - the handlers are IRAM_ATTR;
 *   - esps_dio_rx_push_bit() and esps_dio_crc8() are placed in IRAM by
 *     linker.lf, which keeps the pure C11 half free of ESP-IDF headers (D-4)
 *     — placement is a build-system concern, not a source-code one;
 *   - esp_timer_get_time() is in IRAM (CONFIG_ESP_TIMER_IN_IRAM=y);
 *   - the FreeRTOS *FromISR primitives are in IRAM
 *     (CONFIG_FREERTOS_IN_IRAM=y);
 *   - memcpy/memset resolve to the ESP32 boot ROM
 *     (esp32.rom.libc-funcs.ld), which is always addressable;
 *   - gpio_ll_get_level()/gpio_ll_set_level() are static inline register
 *     accesses. For a pin below 32 — all five of ours are — a write compiles
 *     to one store to GPIO_OUT_W1TS/W1TC, which are write-1-to-set registers:
 *     no read-modify-write, so no lock is needed between the ISR and the task
 *     even though both drive pins.
 *
 * NOT VERIFIED ON HARDWARE. Everything above is a reading of the ESP-IDF
 * sources and this build's sdkconfig, not a measurement. The test that settles
 * it is in the report.
 *
 * ============================================================================
 * WHAT TOUCHES WHAT
 * ============================================================================
 * s_rx (the bit-serial receiver state) is WRITTEN only by the RX_CLK ISR.
 * The task never writes it; when it wants a resync it sets s_rx_resync_req
 * and the ISR performs the reset on its next bit. That is deliberate: the
 * alternative — a critical section around esps_dio_rx_init() in the task —
 * is only correct while the ISR happens to be allocated on the same core, and
 * that is not something the code can guarantee for a future caller who
 * installs the GPIO ISR service first. A one-flag check per bit buys a
 * property that does not depend on core placement at all.
 *
 * The task does READ one field of it, s_rx.state, to spot a receiver parked
 * mid-frame. That read goes through rx_state_read(), which qualifies the
 * access volatile at the point of use — the struct itself stays unqualified
 * because it is the pure M1a type the host tests compile. See the comment
 * there for why the access is also atomic.
 *
 * Every other ISR/task hand-off is a single 32-bit volatile (torn-read-free
 * on this architecture) or a bounded, statically allocated queue whose items
 * the ISR fills completely (see dio_rx_msg_t).
 * Microsecond timestamps are kept as uint32 rather than the int64 that
 * esp_timer_get_time() returns, precisely so that they are single-word: a
 * 64-bit volatile read on a 32-bit core can tear across an ISR. uint32
 * microseconds wrap every ~71 minutes, and every interval measured here is
 * milliseconds at most, so unsigned subtraction is correct across the wrap.
 */
#include "esps_dio.h"

#include "esps_dio_frame.h"
#include "esps_dio_pins.h"
#include "esps_dio_policy.h"
#include "esps_dio_stats.h"
#include "esps_dio_testgen.h"

#if ESPS_DIO_ROLE != ESPS_DIO_ROLE_DISABLED

#include "esps_time.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

#include <string.h>

static const char *TAG = "dio";

/* --- build-time configuration -------------------------------------------- *
 * TODO(S3): every constant below belongs in the experiment spec that EXP_SET
 * delivers and NVS persists (docs/EXPERIMENTS.md). They are -D flags today
 * only because the experiment runtime does not exist yet. */

#ifndef ESPS_DIO_BITRATE
#define ESPS_DIO_BITRATE 10000 /* bit/s on the N3 clock */
#endif
#ifndef ESPS_DIO_BURST_FRAMES
#define ESPS_DIO_BURST_FRAMES 20
#endif
#ifndef ESPS_DIO_BURST_PERIOD_MS
#define ESPS_DIO_BURST_PERIOD_MS 10000
#endif
#ifndef ESPS_DIO_N2_PERIOD_MS
#define ESPS_DIO_N2_PERIOD_MS 1000
#endif
#ifndef ESPS_DIO_N2_TIMEOUT_MS
#define ESPS_DIO_N2_TIMEOUT_MS 50
#endif
/* SPEC-LINK.md fixes this at 3 consecutive timeouts. */
#ifndef ESPS_DIO_LOST_AFTER
#define ESPS_DIO_LOST_AFTER 3
#endif
/* Bytes (ESP-IDF's xTaskCreate takes bytes, not words).
 *
 * MEASURED, not guessed. The deep path is not the bit-bang — it is an EVENT
 * going out: the task calls the sink, which builds cJSON and hands the result
 * to the ENLP framing layer, and both of those are stack hogs.
 *
 * Worst-case chain from dio_task, by summing the `entry a1, N` prologues along
 * the call graph (xtensa-esp32-elf-objdump -d, every edge re-verified):
 *
 *   dio_task 208 -> dio_event_sink 32 -> send_json_frame 32
 *     -> cJSON_PrintUnformatted 32 -> print 80 -> print_value 96
 *     -> sscanf 192 -> __ssvfscanf_r 896 -> sprintf 192 -> _svfprintf_r 800
 *     -> _dtoa_r 160 -> ... -> heap/log/queue tail          = 3760 B (role A)
 *                                                           = 3696 B (role B)
 *
 * cJSON's print_number does a sprintf/sscanf round-trip check on every number,
 * and CONFIG_NEWLIB_NANO_FORMAT is off, so the full 800/896-byte newlib
 * formatters are in play. The sibling branch through the framing layer —
 * send_raw_frame 960 (its 900-byte frame buffer) -> esps_enlp_encode_cobs 1088
 * (its MAX_FRAME scratch) -> 112 = 2160 B — is shallower but worth knowing:
 * any task in this firmware that emits a frame needs ~2.2 KB for framing
 * alone.
 *
 * On top of the task's own worst case:
 *   + ~256 B  Xtensa interrupt entry frame — low/medium-priority ISRs run on
 *             the stack of whatever task they interrupt, not a separate stack
 *   + 288 B   (role A) / 416 B (role B) for this component's own handlers
 *
 *   role A: 3760 + 256 + 288 = 4304 B      role B: 3696 + 256 + 416 = 4368 B
 *
 * 6144 leaves 1776 B (40%) over the governing 4368. 5120 was rejected: it
 * gives only 17%, and the walk leaves ~125 indirect calls on that chain
 * unresolved, so the margin has to absorb real unknowns rather than rounding.
 * CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY is on, so getting this wrong is a
 * panic on the first bench session, not a subtle corruption — and it would
 * panic while emitting the very event that explains why.
 *
 * The task reports its own high-water mark (see report_stack) right after the
 * first event and again at 15 s, so this arithmetic gets replaced by a
 * measurement the first time the firmware runs. */
#ifndef ESPS_DIO_TASK_STACK
#define ESPS_DIO_TASK_STACK 6144
#endif
#ifndef ESPS_DIO_TASK_PRIO
#define ESPS_DIO_TASK_PRIO 11
#endif
#ifndef ESPS_DIO_TASK_CORE
#define ESPS_DIO_TASK_CORE 1
#endif
/* How long the receiver may sit mid-frame with no new bit before it is
 * resynced. Must comfortably exceed one frame's transmission time: 35 bytes
 * at 10 kbit/s is 28 ms, so 250 ms is ~9x margin and still fast enough to be
 * ready before the next burst 10 s later. */
#ifndef ESPS_DIO_RX_IDLE_MS
#define ESPS_DIO_RX_IDLE_MS 250
#endif
/* Housekeeping cadence: how often the task publishes edge events and checks
 * for a stalled receiver when no phase is due. Also caps how long it sleeps,
 * so an event is never more than this late. */
#define ESPS_DIO_HOUSEKEEP_MS 100
/* One-shot stack high-water-mark report, after the first burst has run. */
#define ESPS_DIO_STACK_REPORT_MS (ESPS_DIO_BURST_PERIOD_MS + 5000)

#define ESPS_DIO_BIT_PERIOD_US  (1000000u / (unsigned)ESPS_DIO_BITRATE)
#define ESPS_DIO_HALF_PERIOD_US (ESPS_DIO_BIT_PERIOD_US / 2u)
/* SPEC-LINK.md requires at least 16 clock periods of silence between frames.
 * vTaskDelay(n) guarantees only (n-1) whole ticks, so the nominal figure gets
 * one extra tick on top of the rounding one. */
#define ESPS_DIO_GAP_MS ((16u * ESPS_DIO_BIT_PERIOD_US) / 1000u + 2u)

/* In manual mode the node runs no phases at all: set_gpio owns dio.tx. That
 * releases the link's OUTPUTS (26/27) only — its inputs (14/25) stay refused,
 * because the far end of those wires is another board's output and this node
 * cannot tell. See esps_dio_pin_is_link_input(). */
#ifdef ESPS_DIO_MODE_MANUAL
#define ESPS_DIO_MANUAL_MODE true
#define ESPS_DIO_SCHED_N2_MS 0u
#define ESPS_DIO_SCHED_N3_MS 0u
#else
#define ESPS_DIO_MANUAL_MODE false
#define ESPS_DIO_SCHED_N2_MS ((unsigned)ESPS_DIO_N2_PERIOD_MS)
#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_A
#define ESPS_DIO_SCHED_N3_MS ((unsigned)ESPS_DIO_BURST_PERIOD_MS)
#else
/* Role B neither initiates a handshake nor transmits a burst; its work is
 * entirely interrupt-driven, so it schedules no phases. */
#define ESPS_DIO_SCHED_N3_MS 0u
#endif
#endif

_Static_assert(ESPS_DIO_PIN_TX_DATA != 12 && ESPS_DIO_PIN_RX_DATA != 12 &&
                   ESPS_DIO_PIN_TX_CLK != 12 && ESPS_DIO_PIN_RX_CLK != 12 &&
                   ESPS_DIO_PIN_LED != 12,
               "GPIO12 is MTDI: held high at reset it selects 1.8 V VDD_SDIO and a "
               "3.3 V-flash board stops booting. SPEC-LINK.md forbids it outright.");
_Static_assert(ESPS_DIO_BITRATE >= 100 && ESPS_DIO_BITRATE <= 50000,
               "bit rate outside the range this bit-bang transmitter can hold");
/* Below ~10 us of half period the busy-wait's own esp_timer_get_time() calls
 * and the receiver's ISR entry latency dominate the period. The real ceiling
 * is a bench measurement, not this assert — see the report. */
_Static_assert(ESPS_DIO_HALF_PERIOD_US >= 10, "half period too short to hold accurately");
_Static_assert(ESPS_DIO_ROLE == ESPS_DIO_ROLE_A || ESPS_DIO_ROLE == ESPS_DIO_ROLE_B,
               "ESPS_DIO_ROLE must be 0, 1 or 2");

/* --- state ---------------------------------------------------------------- */

static esps_dio_event_sink_fn s_sink;
static void *s_sink_ctx;
static volatile bool s_ready;
static TaskHandle_t s_task;
static bool s_stack_reported;

/* RX_DATA edge observations, written by an ISR. */
static volatile uint32_t s_edge_count;
static volatile uint8_t s_edge_level;
static volatile uint32_t s_edge_us32;
static uint32_t s_edge_reported;

/* Published through the NDB. Role A leaves the frame counters at 0 (it
 * transmits N3, never receives it) and role B leaves the RTT unsampled (it
 * answers handshakes, never initiates them). Both declare all six channels
 * anyway, so the station's view of a node does not depend on which end of the
 * cable it is — a 0 is an honest "this end received none", and link.rtt_us is
 * simply never sampled rather than reported as a fake 0. */
static esps_dio_link_stats_t s_stats;
static esps_dio_rtt_t s_rtt;
static esps_dio_linkwatch_t s_watch;

/* Event rate limiters. link.up and link.lost-by-handshake are deliberately
 * absent: SPEC-LINK.md exempts them, and they are edge-triggered anyway. */
static esps_dio_ratelimit_t s_rl_edge;
static esps_dio_ratelimit_t s_rl_frame_ok;
static esps_dio_ratelimit_t s_rl_crc_err;
static esps_dio_ratelimit_t s_rl_lost;

#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_A
static uint32_t s_tx_seq;
#endif

#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_B
/* Owned by the RX_CLK ISR. The task only ever reads s_rx.state (a single
 * aligned word) and requests a reset through s_rx_resync_req. */
static esps_dio_rx_t s_rx;

/* The one field of s_rx the task looks at, read volatile at the point of use.
 *
 * esps_dio_rx_t belongs to the pure M1a half: it is compiled by the host tests
 * under plain gcc and its layout is part of a verified contract, so `volatile`
 * must not go into the struct. Qualifying the access instead gives exactly the
 * property needed — the compiler may not cache this load across the polling
 * loop or hoist it out — without changing the type anyone else sees.
 *
 * Atomicity comes separately, from the layout: `state` is an enum (4 bytes) in
 * a 4-aligned static, so the load is a single aligned 32-bit read and cannot
 * tear against the ISR. The value may of course be stale the instant it is
 * read; maybe_resync() only needs "not HUNT for a long time", which a stale
 * read can at worst delay by one housekeeping tick. */
static inline esps_dio_rx_state_t rx_state_read(void) {
    return *(const volatile esps_dio_rx_state_t *)&s_rx.state;
}

static volatile bool s_rx_resync_req;
static volatile uint32_t s_last_bit_us32;
static volatile uint32_t s_rx_dropped;
static uint32_t s_expect; /* next test-frame index the receiver expects */
static uint32_t s_resyncs;

/* payload first so it sits at offset 0 and inherits the struct's alignment —
 * the ISR copies all 32 bytes of it as one constant-size move, and an aligned
 * destination lets the compiler do that in words instead of bytes. */
typedef struct {
    uint8_t payload[ESPS_DIO_MAX_PAYLOAD];
    uint8_t result;
    uint8_t len;
} dio_rx_msg_t;

/* The queue copies sizeof(dio_rx_msg_t) for every item, so any byte the ISR
 * does not write travels to the task as an indeterminate value — a slice of
 * whatever was last on the interrupt stack. The ISR therefore writes the
 * struct in full (all 32 payload bytes, result, len) rather than only the
 * `len` bytes it received, and this assert is what keeps that argument true:
 * padding would be bytes nothing can write. */
_Static_assert(sizeof(dio_rx_msg_t) == ESPS_DIO_MAX_PAYLOAD + 2u,
               "dio_rx_msg_t must have no padding — the ISR defines every byte of it");

/* Statically allocated and bounded: no heap on the ISR's path, and a full
 * queue drops a frame (counted in s_rx_dropped) instead of blocking an ISR.
 * Deep enough to hold a whole default burst plus margin, so even a badly
 * delayed task loses nothing. */
#define ESPS_DIO_RXQ_DEPTH 24
static QueueHandle_t s_rx_q;
static StaticQueue_t s_rx_q_struct;
static uint8_t s_rx_q_storage[ESPS_DIO_RXQ_DEPTH * sizeof(dio_rx_msg_t)];
#endif

/* --- events ---------------------------------------------------------------- */

static void report_stack(const char *when); /* defined with the housekeeping */

/* Never called from an ISR, from a critical section, or with a lock held: the
 * sink builds JSON and hands a frame to the link, both of which allocate.
 * This is also the deepest stack path in the component — see
 * ESPS_DIO_TASK_STACK. */
static void dio_emit(const char *code, esps_dio_severity_t sev, const char *a_key, int32_t a_val,
                     const char *b_key, int32_t b_val, const char *reason, uint32_t suppressed) {
    if (s_sink == NULL) {
        return;
    }
    const esps_dio_event_t ev = {
        .code = code,
        .severity = sev,
        .a_key = a_key,
        .a_val = a_val,
        .b_key = b_key,
        .b_val = b_val,
        .reason = reason,
        .suppressed = suppressed,
    };
    s_sink(&ev, s_sink_ctx);
    /* Right after the first event has come back, not before: this call chain
     * (cJSON + ENLP framing) is the deepest the component has, so the mark is
     * only meaningful once it has actually been walked. */
    report_stack("first event");
}

static void dio_emit_gpio_rejected(int gpio, int level, esps_dio_result_t why) {
    ESP_LOGW(TAG, "set_gpio(%d, %d) rejected: %s", gpio, level, esps_dio_result_str(why));
    dio_emit(ESPS_DIO_EV_GPIO_REJECTED, ESPS_DIO_SEV_WARNING, "gpio", (int32_t)gpio, "level",
             (int32_t)level, esps_dio_result_str(why), 0);
}

/* --- interrupt handlers ----------------------------------------------------
 * Rules observed by every handler below, per the file header: IRAM_ATTR, no
 * allocation, no logging, no blocking, no floating point, and no call to
 * anything that is not provably cache-independent. */

#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_A
/* Any edge on RX_DATA. The timestamp is taken first, before the level read,
 * so the round-trip figure is as close to the electrical edge as software on
 * this chip can get; everything after it is bookkeeping. */
static void IRAM_ATTR isr_rx_data_a(void *arg) {
    (void)arg;
    const uint32_t now = (uint32_t)esp_timer_get_time();
    const int level = gpio_ll_get_level(&GPIO, ESPS_DIO_PIN_RX_DATA);
    s_edge_us32 = now;
    s_edge_level = (uint8_t)level;
    s_edge_count++;
    if (s_task != NULL) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &hp);
        if (hp == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}
#endif

#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_B
/* The N2 responder, and the reason role B needs no phase knowledge at all:
 * it simply copies RX_DATA onto TX_DATA. Two register accesses, so the
 * measured round trip is dominated by interrupt entry rather than by
 * anything this function does.
 *
 * During an N3 burst this also mirrors the transmitter's data bits back down
 * the return line. That is harmless: TX_DATA of B reaches RX_DATA of A, and
 * A masks its own RX_DATA interrupt for the duration of a burst (see
 * run_n3_burst). No phase-signalling wire is needed because of that pair of
 * facts, not because the interaction was overlooked. */
static void IRAM_ATTR isr_rx_data_b(void *arg) {
    (void)arg;
    const int level = gpio_ll_get_level(&GPIO, ESPS_DIO_PIN_RX_DATA);
    gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_TX_DATA, (uint32_t)level);
    /* The witness LED follows the received line, which makes the N1 "watch
     * the cable work" demonstration visible with no extra code. */
    gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_LED, (uint32_t)level);
    s_edge_level = (uint8_t)level;
    s_edge_count++;
}

/* Rising edge of RX_CLK: the transmitter guarantees RX_DATA has been stable
 * for half a bit period, so sampling it here is the contract. */
static void IRAM_ATTR isr_rx_clk_b(void *arg) {
    (void)arg;
    if (s_rx_resync_req) {
        s_rx_resync_req = false;
        esps_dio_rx_init(&s_rx);
    }
    const uint8_t bit = (uint8_t)gpio_ll_get_level(&GPIO, ESPS_DIO_PIN_RX_DATA);
    s_last_bit_us32 = (uint32_t)esp_timer_get_time();

    const esps_dio_rx_result_t r = esps_dio_rx_push_bit(&s_rx, bit);
    if (r == ESPS_DIO_RX_NONE) {
        return;
    }

    dio_rx_msg_t msg;
    msg.result = (uint8_t)r;
    msg.len = s_rx.len;
    /* All 32 bytes, unconditionally, not just the `len` received ones.
     *
     * The queue copies sizeof(msg) whatever we do, so a partial fill would
     * hand the task — and the queue's static storage, which outlives the call
     * — a tail of indeterminate stack bytes. Copying the whole array is also
     * the cheaper of the two: the size is now a compile-time constant, so the
     * compiler emits a straight-line move instead of the variable-length
     * memcpy the old `len`-sized copy produced.
     *
     * Reading all 32 is always safe: esps_dio_rx_init() zeroes the whole
     * receiver before the ISR is ever installed, so s_rx.payload is fully
     * defined from boot; bytes past `len` are stale frame data, which is
     * defined-but-uninteresting, and the task reads only `len` of them. */
    memcpy(msg.payload, s_rx.payload, ESPS_DIO_MAX_PAYLOAD);

    BaseType_t hp = pdFALSE;
    if (xQueueSendFromISR(s_rx_q, &msg, &hp) != pdTRUE) {
        s_rx_dropped++; /* bounded queue: drop and count, never block in an ISR */
    }
    if (hp == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
#endif

/* --- pin setup -------------------------------------------------------------- */

static bool dio_check_pins(void) {
    static const struct {
        int gpio;
        bool output;
        const char *what;
    } pins[] = {
        {ESPS_DIO_PIN_TX_DATA, true, "TX_DATA"}, {ESPS_DIO_PIN_TX_CLK, true, "TX_CLK"},
        {ESPS_DIO_PIN_LED, true, "LED"},         {ESPS_DIO_PIN_RX_DATA, false, "RX_DATA"},
        {ESPS_DIO_PIN_RX_CLK, false, "RX_CLK"},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        const bool allowed = pins[i].output ? esps_dio_pin_is_allowed_output(pins[i].gpio)
                                            : esps_dio_pin_is_allowed_input(pins[i].gpio);
        if (!allowed) {
            /* Log only, deliberately no dio.gpio_rejected event. Two reasons,
             * either of which is sufficient: this runs from esps_dio_start(),
             * which D-1 puts before the station link is even opened, so the
             * sink is still NULL and any event would be dropped on the floor;
             * and SPEC-LINK.md's event table defines dio.gpio_rejected as the
             * answer to a set_gpio request, carrying the `level` that was
             * asked for — there is no requested level here, and inventing a
             * -1 sentinel would put a value in the field that no reader of the
             * table expects. The ESP_LOGE reaches the station as raw console
             * output once it connects (PROTOCOL.md S2.1). */
            ESP_LOGE(TAG, "%s is GPIO%d, which is not in the allow-list — refusing to start",
                     pins[i].what, pins[i].gpio);
            ok = false;
        }
    }
    return ok;
}

static bool dio_setup_pins(void) {
    /* Write the output latch low BEFORE enabling the driver. The pad then
     * goes high-Z -> 0 rather than presenting whatever the latch happened to
     * hold; on the A.27 -> B.14 clock line the difference is a spurious clock
     * edge into the receiver's shift register at every boot. Writing a level
     * to a pin that is still an input is harmless. */
    (void)gpio_set_level(ESPS_DIO_PIN_TX_DATA, 0);
    (void)gpio_set_level(ESPS_DIO_PIN_TX_CLK, 0);
    (void)gpio_set_level(ESPS_DIO_PIN_LED, 0);

    /* GPIO_MODE_INPUT_OUTPUT, not GPIO_MODE_OUTPUT: the NDB publishes dio.tx
     * as the *real* pad level, and plain OUTPUT disables the input buffer so
     * gpio_get_level() would read a constant 0. Keeping the input path alive
     * also means a contention or a shorted line shows up as a mismatch
     * between what we drove and what we read, instead of being invisible. */
    gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(ESPS_DIO_PIN_TX_DATA) | BIT64(ESPS_DIO_PIN_TX_CLK) |
                        BIT64(ESPS_DIO_PIN_LED),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "output pin config failed: %s", esp_err_to_name(err));
        return false;
    }

    /* INPUT_PULLDOWN per SPEC-LINK.md. The pull-down is the whole reason a
     * disconnected cable reads a stable 0 instead of noise — it is the
     * correction to the original practice's bare INPUT, and it is also what
     * keeps a reset transmitter (whose pins go high-Z) from injecting
     * phantom edges into this receiver. */
    gpio_config_t in_cfg = {
        .pin_bit_mask = BIT64(ESPS_DIO_PIN_RX_DATA) | BIT64(ESPS_DIO_PIN_RX_CLK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE, /* armed per pin, per role, below */
    };
    err = gpio_config(&in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input pin config failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool dio_install_isrs(void) {
    /* Installed from the phase task, which is pinned to ESPS_DIO_TASK_CORE, so
     * esp_intr_alloc() binds the interrupt to that core: the per-bit handler
     * ends up on the same core as the task it feeds, and off core 0 where the
     * esp_timer ISR is pinned. It says nothing about where the UART link tasks
     * run — those are unpinned; see the affinity note in esps_dio_start(). */
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Someone else got there first. Ours still work, but they inherit
         * that caller's flags — including, possibly, the absence of
         * ESP_INTR_FLAG_IRAM, which silently reintroduces the flash-cache
         * dropout this component went to some trouble to avoid. Worth a
         * warning rather than a shrug. */
        ESP_LOGW(TAG, "GPIO ISR service already installed elsewhere; handlers inherit its "
                      "flags and core — IRAM-safety of the bit path is no longer guaranteed");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return false;
    }

#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_A
    err = gpio_set_intr_type(ESPS_DIO_PIN_RX_DATA, GPIO_INTR_ANYEDGE);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(ESPS_DIO_PIN_RX_DATA, isr_rx_data_a, NULL);
    }
#else
    err = gpio_set_intr_type(ESPS_DIO_PIN_RX_DATA, GPIO_INTR_ANYEDGE);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(ESPS_DIO_PIN_RX_DATA, isr_rx_data_b, NULL);
    }
    if (err == ESP_OK) {
        err = gpio_set_intr_type(ESPS_DIO_PIN_RX_CLK, GPIO_INTR_POSEDGE);
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(ESPS_DIO_PIN_RX_CLK, isr_rx_clk_b, NULL);
    }
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "interrupt setup failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* --- shared housekeeping ------------------------------------------------------ */

static void publish_edges(uint32_t now_ms) {
    const uint32_t count = s_edge_count;
    if (count == s_edge_reported) {
        return;
    }
    s_edge_reported = count;
    if (!esps_dio_ratelimit_allow(&s_rl_edge, now_ms)) {
        return;
    }
    dio_emit(ESPS_DIO_EV_EDGE, ESPS_DIO_SEV_DEBUG, "level", (int32_t)s_edge_level, "count",
             (int32_t)count, NULL, esps_dio_ratelimit_take_suppressed(&s_rl_edge));
}

/* Reports the phase task's stack high-water mark once, so nobody has to trust
 * the arithmetic at ESPS_DIO_TASK_STACK: the first run prints the real figure.
 *
 * Called from two places on purpose. The 15-second call would be the obvious
 * one, but the deepest path in the whole component is emitting an event, and
 * if that path overflows, the panic takes with it the very number that would
 * have explained the overflow. Reporting immediately after the first event has
 * returned means the measurement survives even if the second one never
 * happens.
 *
 * Always measures s_task explicitly rather than the calling task:
 * esps_dio_set_gpio() can emit an event from any task, and NULL would then
 * report that task's stack instead of the one being sized. */
static void report_stack(const char *when) {
    if (s_stack_reported || s_task == NULL) {
        return;
    }
    s_stack_reported = true;
    /* ESP-IDF's uxTaskGetStackHighWaterMark returns bytes, not words. */
    const unsigned free_b = (unsigned)uxTaskGetStackHighWaterMark(s_task);
    ESP_LOGI(TAG, "phase task stack after %s: %u B never used of %u B allocated (%u B used)",
             when, free_b, (unsigned)ESPS_DIO_TASK_STACK,
             (unsigned)ESPS_DIO_TASK_STACK - free_b);
}

/* --- role A: handshake initiator + N3 transmitter ------------------------------ */

#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_A

/* Waits, within a hard microsecond budget, for the RX_DATA edge that takes
 * the line to `want_level`. Returns false on timeout.
 *
 * Every wait in this file is bounded. A node whose cable is unplugged must
 * keep sampling, keep sending telemetry and keep answering the station
 * (D-1); "blocks until the other board answers" would be a node that stops
 * being a node the moment someone trips over a wire. */
static bool n2_wait_level(uint8_t want_level, uint32_t budget_us, uint32_t *edge_us_out) {
    const uint32_t start = (uint32_t)esp_timer_get_time();
    for (;;) {
        const uint32_t elapsed = (uint32_t)esp_timer_get_time() - start;
        if (elapsed >= budget_us) {
            return false;
        }
        const uint32_t remain_ms = (budget_us - elapsed + 999u) / 1000u;
        TickType_t ticks = pdMS_TO_TICKS(remain_ms);
        if (ticks == 0) {
            ticks = 1;
        }
        if (ulTaskNotifyTake(pdTRUE, ticks) == 0) {
            return false;
        }
        if (s_edge_level == want_level) {
            if (edge_us_out != NULL) {
                *edge_us_out = s_edge_us32;
            }
            return true;
        }
        /* An edge to the other level: noise, or the tail of a previous
         * exchange. Keep waiting, but inside the same budget. */
    }
}

static void run_n2(void) {
    /* Drop anything the ISR queued between phases, so a stale edge cannot be
     * mistaken for this exchange's reply and produce a nonsense RTT. */
    (void)ulTaskNotifyTake(pdTRUE, 0);

    const uint32_t t0 = (uint32_t)esp_timer_get_time();
    gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_TX_DATA, 1);

    uint32_t edge_us = 0;
    const bool ok = n2_wait_level(1, (uint32_t)ESPS_DIO_N2_TIMEOUT_MS * 1000u, &edge_us);

    gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_TX_DATA, 0);
    /* The responder mirrors the falling edge too. Waiting for it — bounded,
     * and with its outcome ignored — leaves both lines settled at 0 before
     * the next phase, instead of starting the next handshake on a line that
     * is still coming down. The exchange was already scored above. */
    (void)n2_wait_level(0, (uint32_t)ESPS_DIO_N2_TIMEOUT_MS * 1000u, NULL);

    if (ok) {
        esps_dio_rtt_add(&s_rtt, edge_us - t0);
    } else {
        esps_dio_rtt_timeout(&s_rtt);
    }

    const esps_dio_link_edge_t edge = esps_dio_linkwatch_update(&s_watch, ok);
    gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_LED,
                      esps_dio_linkwatch_is_up(&s_watch) ? 1u : 0u);

    /* Neither transition is rate-limited: SPEC-LINK.md exempts them, and they
     * are edge-triggered, so there is nothing to limit. */
    if (edge == ESPS_DIO_LINK_WENT_UP) {
        dio_emit(ESPS_DIO_EV_LINK_UP, ESPS_DIO_SEV_INFO, "rtt_us", (int32_t)s_rtt.last_us,
                 "timeouts", (int32_t)s_rtt.timeouts, NULL, 0);
    } else if (edge == ESPS_DIO_LINK_WENT_DOWN) {
        dio_emit(ESPS_DIO_EV_LINK_LOST, ESPS_DIO_SEV_WARNING, "timeouts",
                 (int32_t)s_watch.consec_bad, "samples", (int32_t)s_rtt.samples,
                 "handshake_timeout", 0);
    }
}

/* Busy-waits until *deadline + half_us, then advances *deadline.
 *
 * The re-anchoring branch is the important part. If the task was preempted
 * past the deadline, "catching up" would emit every remaining bit of the
 * frame back to back, at a rate the receiver's per-edge interrupt cannot
 * follow — turning one scheduling hiccup into one corrupted frame, or
 * several. Holding the nominal period instead costs only a slightly longer
 * frame, which the clocked link does not care about at all. */
static inline void dio_spin(uint32_t *deadline, uint32_t half_us) {
    *deadline += half_us;
    uint32_t now = (uint32_t)esp_timer_get_time();
    if ((int32_t)(now - *deadline) >= 0) {
        *deadline = now;
        return;
    }
    while ((int32_t)((uint32_t)esp_timer_get_time() - *deadline) < 0) {
        /* Busy-wait, interrupts enabled. See the comment on tx_frame. */
    }
}

/* WHY THE TASK'S JITTER DOES NOT CORRUPT A FRAME
 *
 * The link is synchronous with an explicit clock (SPEC-LINK.md): the receiver
 * samples RX_DATA on the rising edge of RX_CLK, and has no baud assumption,
 * no inter-bit timeout and no oversampling. DATA is written a half period
 * before CLK rises and held until after CLK falls, so the setup and hold
 * windows *are* the half period.
 *
 * If this task is preempted — by an ISR, by the esp_timer task at priority
 * 22, by anything — the pins simply hold their current levels for longer. A
 * stretched half period is still a valid half period: the data is stable
 * around the edge either way. Jitter costs throughput, never correctness.
 * That is precisely why SPEC-LINK.md chose a clocked link over a bit-banged
 * asynchronous UART, where the same preemption would shift every subsequent
 * sampling point and corrupt the byte.
 *
 * Two things do not follow from that, and are handled rather than assumed:
 *
 *  - Falling behind must not be caught up. See dio_spin().
 *
 *  - Interrupts stay ENABLED for the whole frame. A 20-frame burst runs for
 *    hundreds of milliseconds; masking interrupts across it would blow the
 *    300 ms interrupt watchdog (CONFIG_ESP_INT_WDT_TIMEOUT_MS=300), starve
 *    the UART link and stop the FreeRTOS tick. The only thing masked is this
 *    board's own RX_DATA edge interrupt, for the reason given in
 *    run_n3_burst(), and that masks one GPIO, not the CPU.
 *
 * UNVERIFIED ON HARDWARE: the claim that the resulting edge timing is good
 * enough for the receiver at 10 kbit/s is reasoning, not a measurement. */
static void tx_frame(const uint8_t *bytes, size_t len) {
    esps_dio_txbits_t tx;
    esps_dio_txbits_init(&tx, bytes, len);

    uint32_t deadline = (uint32_t)esp_timer_get_time();
    uint8_t bit;
    while (esps_dio_txbits_next(&tx, &bit)) {
        gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_TX_DATA, bit);
        dio_spin(&deadline, ESPS_DIO_HALF_PERIOD_US);
        gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_TX_CLK, 1);
        dio_spin(&deadline, ESPS_DIO_HALF_PERIOD_US);
        gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_TX_CLK, 0);
    }
    /* SPEC-LINK.md: after the last bit, DATA returns to 0 and the line rests
     * at CLK=0, DATA=0. */
    gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_TX_DATA, 0);
}

static void run_n3_burst(void) {
    /* Board B echoes every RX_DATA transition straight back onto its TX_DATA,
     * which arrives on OUR RX_DATA. During a burst that is one interrupt per
     * data bit on this board — around 10 000/s at the default rate — landing
     * on the very task that is trying to hold a 50 us half period. Nothing in
     * the N3 phase reads RX_DATA, so mask it for the duration.
     *
     * This is the one piece of cross-board interaction the phase design has
     * to handle explicitly, and it is *why* no phase-signalling wire is
     * needed: only the transmitter has to know which phase is running. */
    const bool masked = (gpio_intr_disable(ESPS_DIO_PIN_RX_DATA) == ESP_OK);
    if (!masked) {
        ESP_LOGW(TAG, "could not mask RX_DATA for the burst; transmit jitter will be higher");
    }
    gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_LED, 1);

    for (unsigned i = 0; i < (unsigned)ESPS_DIO_BURST_FRAMES; i++) {
        uint8_t payload[ESPS_DIO_MAX_PAYLOAD];
        const size_t len = esps_dio_testframe_payload(s_tx_seq, payload);
        uint8_t frame[ESPS_DIO_FRAME_MAX];
        size_t frame_len = 0;
        if (esps_dio_frame_build(payload, len, frame, sizeof(frame), &frame_len)) {
            tx_frame(frame, frame_len);
        }
        s_tx_seq++;
        /* The inter-frame gap doubles as the yield that lets the idle task on
         * this core run, which is what feeds the task watchdog. Without it a
         * burst would busy-wait for the whole burst duration. */
        vTaskDelay(pdMS_TO_TICKS(ESPS_DIO_GAP_MS));
    }

    gpio_ll_set_level(&GPIO, ESPS_DIO_PIN_LED, esps_dio_linkwatch_is_up(&s_watch) ? 1u : 0u);
    if (masked) {
        (void)gpio_intr_enable(ESPS_DIO_PIN_RX_DATA);
    }
    /* Edges that piled up around the mask window are not this exchange's. */
    (void)ulTaskNotifyTake(pdTRUE, 0);
}
#endif /* ROLE A */

/* --- role B: handshake responder + N3 receiver --------------------------------- */

#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_B

/* link.crc_err covers both ways a frame can arrive broken; `reason` says
 * which. Both share one rate limiter, because to an operator watching a noisy
 * cable they are the same event — "a frame did not survive" — and splitting
 * the budget would let a burst of one hide the other. */
static void emit_frame_err(uint32_t now_ms, uint8_t len, const char *reason) {
    if (!esps_dio_ratelimit_allow(&s_rl_crc_err, now_ms)) {
        return;
    }
    dio_emit(ESPS_DIO_EV_CRC_ERR, ESPS_DIO_SEV_WARNING, "frames_err",
             (int32_t)(s_stats.frames_crc_err + s_stats.frames_len_err), "len", (int32_t)len,
             reason, esps_dio_ratelimit_take_suppressed(&s_rl_crc_err));
}

static void handle_rx(const dio_rx_msg_t *m, uint32_t now_ms) {
    const esps_dio_rx_result_t r = (esps_dio_rx_result_t)m->result;

    if (r == ESPS_DIO_RX_FRAME_LEN_ERR) {
        /* SPEC-LINK.md: a len_err frame does NOT move the expected index — it
         * never completed, so there is no transmitted frame it corresponds to
         * and nothing to have advanced past. It does count in frames_err and
         * it does emit link.crc_err, with reason "len".
         *
         * `len` is reported as 0 here and that is not a shortcut: the M1a
         * receiver sets rx->len = 0 on LEN_ERR, a behaviour SPEC-LINK.md
         * itself pins ("Tras LEN_ERR, len vale 0"), so the offending LEN byte
         * is already gone by the time this runs. Flagged to the orchestrator
         * rather than changing frame.c under a verified contract. */
        esps_dio_stats_account(&s_stats, r, NULL, NULL, 0);
        emit_frame_err(now_ms, m->len, "len");
        return;
    }

    /* len is 1..32 for any completed frame — the state machine rejects
     * anything else as LEN_ERR before it gets here — but payload[0] is only
     * meaningful if the ISR actually copied it, so the guard is on len, not
     * on faith in an invariant two files away. */
    if (r == ESPS_DIO_RX_FRAME_OK && m->len > 0u) {
        /* Byte 0 of a CRC-valid frame is authoritative for the index
         * (SPEC-LINK.md "Payload de prueba"), so resync before regenerating
         * the expected payload — otherwise every frame after a single loss is
         * compared against the wrong reference and BER reads ~0.5. */
        const uint32_t resynced = esps_dio_resync_index(s_expect, m->payload[0]);
        const uint32_t missed = resynced - s_expect;
        s_expect = resynced;
        if (missed > 0u) {
            /* An OK frame that resynced with a jump means frames went missing
             * on the wire. SPEC-LINK.md calls that link.lost by frames, and
             * unlike link.lost by handshake it IS rate-limited. */
            if (esps_dio_ratelimit_allow(&s_rl_lost, now_ms)) {
                dio_emit(ESPS_DIO_EV_LINK_LOST, ESPS_DIO_SEV_WARNING, "missed", (int32_t)missed,
                         "seq", (int32_t)s_expect, "frames_missed",
                         esps_dio_ratelimit_take_suppressed(&s_rl_lost));
            }
        }
    }

    uint8_t expected[ESPS_DIO_MAX_PAYLOAD];
    (void)esps_dio_testframe_payload(s_expect, expected);
    esps_dio_stats_account(&s_stats, r, m->payload, expected, m->len);
    /* Both OK and CRC_ERR advance: a CRC-bad frame is compared against the
     * current expected index and then the index moves on (SPEC-LINK.md). */
    s_expect++;

    if (r == ESPS_DIO_RX_FRAME_OK) {
        if (esps_dio_ratelimit_allow(&s_rl_frame_ok, now_ms)) {
            dio_emit(ESPS_DIO_EV_FRAME_OK, ESPS_DIO_SEV_INFO, "frames_ok",
                     (int32_t)s_stats.frames_ok, "len", (int32_t)m->len, NULL,
                     esps_dio_ratelimit_take_suppressed(&s_rl_frame_ok));
        }
    } else {
        emit_frame_err(now_ms, m->len, "crc");
    }
}

static void maybe_resync(uint32_t now_ms) {
    const uint32_t idle_ms = ((uint32_t)esp_timer_get_time() - s_last_bit_us32) / 1000u;
    if (!esps_dio_rx_stalled(rx_state_read(), idle_ms, ESPS_DIO_RX_IDLE_MS)) {
        return;
    }
    if (s_rx_resync_req) {
        return; /* already asked; the ISR performs it on its next bit */
    }
    /* This is what a cable pulled out mid-burst looks like from here: the bit
     * state machine is parked in LEN/PAYLOAD/CRC waiting for bits that will
     * never arrive, and the frame format has no timeout of its own. Left
     * alone, the first frames of the NEXT burst get shifted into the stale
     * frame and are lost. The ISR owns s_rx, so this is a request, not a
     * write — see the file header for why that is better than a lock. */
    s_rx_resync_req = true;
    s_resyncs++;
    ESP_LOGD(TAG, "receiver parked mid-frame for %u ms; resync requested (total %u)",
             (unsigned)idle_ms, (unsigned)s_resyncs);
    (void)now_ms;
}
#endif /* ROLE B */

/* --- the phase task ------------------------------------------------------------ */

static void dio_task(void *arg) {
    (void)arg;

    if (!dio_setup_pins() || !dio_install_isrs()) {
        /* The link failing to come up must not take the node down with it:
         * telemetry, heartbeat and the station link all keep running. */
        ESP_LOGE(TAG, "digital link did not start; the rest of the node is unaffected");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_ready = true;

    ESP_LOGI(TAG, "pins tx_data=%d rx_data=%d tx_clk=%d rx_clk=%d led=%d",
             ESPS_DIO_PIN_TX_DATA, ESPS_DIO_PIN_RX_DATA, ESPS_DIO_PIN_TX_CLK,
             ESPS_DIO_PIN_RX_CLK, ESPS_DIO_PIN_LED);
#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_A
    /* Role A drives the cadence, so its log line is the one that states it. */
    ESP_LOGI(TAG, "role A (handshake initiator + N3 transmitter): %u bit/s (%u us/bit), "
                  "handshake every %u ms, burst of %u frames every %u ms, %u ms inter-frame gap",
             (unsigned)ESPS_DIO_BITRATE, (unsigned)ESPS_DIO_BIT_PERIOD_US,
             (unsigned)ESPS_DIO_SCHED_N2_MS, (unsigned)ESPS_DIO_BURST_FRAMES,
             (unsigned)ESPS_DIO_SCHED_N3_MS, (unsigned)ESPS_DIO_GAP_MS);

    /* Only role A schedules anything. Role B's work is entirely
     * interrupt-driven, so giving it a scheduler it never polls would be
     * state that lies about what the task does. */
    esps_dio_sched_t sched;
    esps_dio_sched_init(&sched, esps_time_now_ms(), ESPS_DIO_SCHED_N2_MS, ESPS_DIO_SCHED_N3_MS);
#else
    ESP_LOGI(TAG, "role B (handshake responder + N3 receiver): interrupt-driven, no phases of "
                  "its own; expects a role A peer to drive the cable");
#endif

    uint32_t last_housekeep_ms = esps_time_now_ms();

    for (;;) {
#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_A
        const uint32_t now_ms = esps_time_now_ms();
        switch (esps_dio_sched_due(&sched, now_ms)) {
        case ESPS_DIO_PHASE_N3_BURST:
            run_n3_burst();
            break;
        case ESPS_DIO_PHASE_N2:
            run_n2();
            break;
        default: {
            uint32_t sleep_ms = esps_dio_sched_delay_ms(&sched, now_ms);
            if (sleep_ms > ESPS_DIO_HOUSEKEEP_MS) {
                sleep_ms = ESPS_DIO_HOUSEKEEP_MS;
            }
            vTaskDelay(pdMS_TO_TICKS(sleep_ms == 0 ? 1u : sleep_ms));
            break;
        }
        }
#else
        /* Role B is entirely interrupt-driven; the task exists to do the work
         * the ISR is not allowed to do — regenerate the expected payload,
         * account for it, and emit events. */
        dio_rx_msg_t msg;
        if (xQueueReceive(s_rx_q, &msg, pdMS_TO_TICKS(ESPS_DIO_HOUSEKEEP_MS)) == pdTRUE) {
            handle_rx(&msg, esps_time_now_ms());
        }
#endif

        /* Housekeeping runs on its own cadence rather than once per loop, so a
         * back-to-back run of frames cannot starve it and a long idle cannot
         * delay it. */
        const uint32_t after_ms = esps_time_now_ms();
        if ((uint32_t)(after_ms - last_housekeep_ms) >= ESPS_DIO_HOUSEKEEP_MS) {
            last_housekeep_ms = after_ms;
#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_B
            maybe_resync(after_ms);
#endif
            publish_edges(after_ms);
            /* Backstop for a node that has not emitted a single event yet —
             * a role B board with no peer plugged in, for instance. */
            if (after_ms >= ESPS_DIO_STACK_REPORT_MS) {
                report_stack("15 s idle");
            }
        }
    }
}

/* --- public API ----------------------------------------------------------------- */

void esps_dio_set_event_sink(esps_dio_event_sink_fn fn, void *ctx) {
    s_sink_ctx = ctx;
    s_sink = fn;
}

static const esps_dio_ndb_entry_t s_ndb[] = {
    /* SPEC-LINK.md "Canales NDB", and byte-for-byte the same key/name/unit/
     * type/group strings the gateway simulator declares in
     * transports/sim/dio_link.py — a simulated node and a real one must look
     * identical to the station (D-8). */
    {ESPS_DIO_CH_TX, "dio.tx", "DIO TX", "", "u8", 1, "digital"},
    {ESPS_DIO_CH_RX, "dio.rx", "DIO RX", "", "u8", 1, "digital"},
    {ESPS_DIO_CH_RTT_US, "link.rtt_us", "Link RTT", "us", "u32", 1, "link"},
    {ESPS_DIO_CH_FRAMES_OK, "link.frames_ok", "Frames OK", "", "u32", 1, "link"},
    {ESPS_DIO_CH_FRAMES_ERR, "link.frames_err", "Frames err", "", "u32", 1, "link"},
    {ESPS_DIO_CH_BER, "link.ber", "Bit error rate", "", "f32", 1, "link"},
};

_Static_assert(sizeof(s_ndb) / sizeof(s_ndb[0]) == ESPS_DIO_NDB_COUNT,
               "ESPS_DIO_NDB_COUNT is what callers size their arrays from; keep it "
               "equal to the table or they will silently truncate");

const esps_dio_ndb_entry_t *esps_dio_ndb(size_t *out_count) {
    if (out_count != NULL) {
        *out_count = sizeof(s_ndb) / sizeof(s_ndb[0]);
    }
    return s_ndb;
}

void esps_dio_get_sample(esps_dio_sample_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_ready) {
        return;
    }
    /* The real pad levels, not what we believe we drove — see dio_setup_pins()
     * for why TX_DATA keeps its input buffer enabled. */
    out->tx_level = (uint8_t)gpio_get_level(ESPS_DIO_PIN_TX_DATA);
    out->rx_level = (uint8_t)gpio_get_level(ESPS_DIO_PIN_RX_DATA);
    out->rtt_us = s_rtt.last_us;
    out->rtt_valid = s_rtt.last_ok;
    out->frames_ok = s_stats.frames_ok;
    out->frames_err = s_stats.frames_crc_err + s_stats.frames_len_err;
    out->ber = esps_dio_stats_ber(&s_stats);
}

esps_dio_result_t esps_dio_set_gpio(int gpio, int level) {
    const esps_dio_result_t check = esps_dio_gpio_validate(gpio, level, ESPS_DIO_MANUAL_MODE);
    if (check != ESPS_DIO_OK) {
        dio_emit_gpio_rejected(gpio, level, check);
        return check;
    }
    if (!s_ready) {
        /* Writing a pin the GPIO driver has not been told about would return
         * an error or, worse, quietly do nothing. */
        dio_emit_gpio_rejected(gpio, level, ESPS_DIO_ERR_NOT_READY);
        return ESPS_DIO_ERR_NOT_READY;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(gpio), /* validated to 4..33 above */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK || gpio_set_level((gpio_num_t)gpio, (uint32_t)level) != ESP_OK) {
        dio_emit_gpio_rejected(gpio, level, ESPS_DIO_ERR_DRIVER);
        return ESPS_DIO_ERR_DRIVER;
    }
    dio_emit(ESPS_DIO_EV_GPIO, ESPS_DIO_SEV_INFO, "gpio", (int32_t)gpio, "level", (int32_t)level,
             NULL, 0);
    return ESPS_DIO_OK;
}

bool esps_dio_start(void) {
    if (s_task != NULL) {
        return true; /* idempotent */
    }
    if (!dio_check_pins()) {
        return false;
    }

    esps_dio_rtt_init(&s_rtt);
    esps_dio_linkwatch_init(&s_watch, ESPS_DIO_LOST_AFTER, 1);
    /* SPEC-LINK.md names which events are limited; these are the windows.
     * dio.edge is the only one that can genuinely run away — a bit-rate-speed
     * data line produces thousands of edges a second — so it is capped hard
     * at 5/s and the rest report one sample per burst period. */
    esps_dio_ratelimit_init(&s_rl_edge, 1000, 5);
    esps_dio_ratelimit_init(&s_rl_frame_ok, 5000, 1);
    esps_dio_ratelimit_init(&s_rl_crc_err, 5000, 2);
    esps_dio_ratelimit_init(&s_rl_lost, 5000, 1);

#if ESPS_DIO_ROLE == ESPS_DIO_ROLE_B
    esps_dio_rx_init(&s_rx);
    s_rx_q = xQueueCreateStatic(ESPS_DIO_RXQ_DEPTH, sizeof(dio_rx_msg_t), s_rx_q_storage,
                                &s_rx_q_struct);
    if (s_rx_q == NULL) {
        ESP_LOGE(TAG, "receive queue creation failed");
        return false;
    }
#endif

    /* PRIORITY AND AFFINITY.
     *
     * Priority 11 is one above the UART link's TX/RX tasks (10) and well above
     * the telemetry/heartbeat/hello tasks (5). The bit-bang loop holds a
     * 50 us half period by busy-waiting, so every task that can preempt it
     * widens the clock period; being above the link tasks removes the one
     * preemption source that is both frequent and under our control. What it
     * costs is that a frame can delay a UART TX by up to one frame time
     * (~28 ms worst case at 10 kbit/s). That is affordable here: the TX queue
     * is 16 frames deep, non-blocking, and fed at about 2 frames/s, and D-1
     * says the station is never in a control loop anyway.
     *
     * Pinned to core 1, and it is worth being exact about what that does and
     * does not buy, because the obvious reading of it is wrong.
     *
     * It DOES keep the busy-wait off the same core as esp_timer: both the
     * esp_timer task and the esp_timer ISR are pinned to core 0 in this build
     * (CONFIG_ESP_TIMER_TASK_AFFINITY_CPU0, CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0
     * in the generated sdkconfig), so those genuinely cannot interleave with a
     * half period. It also puts this component's GPIO interrupt on core 1,
     * next to the task it feeds, because dio_install_isrs() runs from here and
     * esp_intr_alloc() binds to the calling core.
     *
     * It does NOT isolate the busy-wait from the station link. esps_link_uart
     * creates esps_link_tx and esps_link_rx with xTaskCreate(), i.e.
     * tskNO_AFFINITY, so the SMP scheduler is free to place either of them on
     * core 1 — where this task, at priority 11 against their 10, will hold the
     * CPU for up to one frame time (~28 ms at 10 kbit/s) and, across a burst
     * with its inter-frame yields, up to ~370 ms. Pinning cannot prevent that;
     * only giving the link tasks an affinity of their own would, and that is
     * esps_link's decision to make, not this component's.
     *
     * Bounded, on the numbers of this firmware: the TX queue is 16 frames
     * deep and the send is non-blocking, while the node emits about 2 frames/s
     * (heartbeat + telemetry) plus log traffic, so a 370 ms stall banks roughly
     * one frame against a depth of 16. The RX side is the tighter one and the
     * one to watch on the bench: the driver's ring is 2048 B and 370 ms at
     * 115200 baud is ~4.2 kB, so a station that streamed continuously through a
     * burst could overrun it. Nothing streams today — the station sends
     * occasional CMDs — but that is a property of current usage, not a
     * guarantee.
     *
     * The stack is not a guess-and-hope figure, and it is not the bit-bang
     * that sizes it: run_n3_burst()'s own locals are ~70 B. The deep path is
     * an EVENT going out — sink -> cJSON -> ENLP framing — measured at 3760 B
     * (role A) / 3696 B (role B) plus the interrupt entry frame and this
     * component's handlers. The full arithmetic is at ESPS_DIO_TASK_STACK, and
     * report_stack() prints the measured high-water mark in a LOG frame right
     * after the first event, so the number gets settled by the hardware rather
     * than argued about. */
    const BaseType_t ok = xTaskCreatePinnedToCore(dio_task, "esps_dio", ESPS_DIO_TASK_STACK, NULL,
                                                  ESPS_DIO_TASK_PRIO, &s_task, ESPS_DIO_TASK_CORE);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "phase task creation failed");
        return false;
    }
    return true;
}

#else /* ESPS_DIO_ROLE == ESPS_DIO_ROLE_DISABLED */

/* With the link disabled this component contributes no code and no data:
 * main.c does not reference it, the firmware behaves exactly as it did before
 * esps_dio existed, and `nm` on the ELF finds no esps_dio symbol. The typedef
 * keeps the translation unit from being empty, which ISO C does not allow. */
typedef int esps_dio_translation_unit_is_not_empty_t;

#endif /* ESPS_DIO_ROLE */

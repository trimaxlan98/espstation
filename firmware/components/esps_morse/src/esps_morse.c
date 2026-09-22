/* The ESP-IDF half of esps_morse.
 *
 * Shape: two edge ISRs that do nothing but timestamp, and one 1 ms task that
 * does all the deciding.
 *
 * WHY THE KEY HAS AN ISR TOO, unlike the Arduino sketch, which polls it in
 * loop(). The sketch's loop runs thousands of times per millisecond, so a
 * bounce can never slip between two reads. A FreeRTOS task cannot poll that
 * fast without burning a core, and a bounce that landed entirely between two
 * polls would be INVISIBLE -- the filter would see a level that looks stable
 * and accept an edge the contact never really made. So every raw change is
 * captured by an interrupt with its own timestamp, and the task replays them
 * in order through the same pure filter the sketch uses. The bounce counter
 * stays truthful and the debounce keeps its meaning.
 *
 * WHY 1 ms AND NOT SLOWER. The accepted edge reaches TX_DATA when the task
 * notices the debounce deadline has passed, so the wire edge is late by up to
 * one task period. The far end measures durations off that wire, and the
 * bench measured the two ends agreeing to +-1 ms; a 10 ms period would turn
 * that into +-10 ms and make the practice's central measurement meaningless.
 * CONFIG_FREERTOS_HZ=1000 is already set in sdkconfig.defaults, so
 * vTaskDelay(1) really is a millisecond here -- if that ever changes, this
 * component's timing claim goes with it.
 *
 * WHY NO linker.lf, unlike esps_dio. Its ISR runs the frame decoder and CRC,
 * so those objects must be in IRAM or a flash write would corrupt the link.
 * These ISRs only read the clock, read a pin and write a ring slot; they call
 * nothing from the pure half.
 *
 * WHAT "IRAM-SAFE" ACTUALLY REQUIRES. The handlers being IRAM_ATTR and the
 * ring living in DRAM is NOT the whole requirement, and believing it was cost
 * this component a crash bug: the handlers must also not CALL anything that
 * lives in flash, because ESP_INTR_FLAG_IRAM leaves the interrupt enabled
 * while the cache is off. The original code called gpio_get_level(), which is
 * a real out-of-line function placed in IRAM only when
 * CONFIG_GPIO_CTRL_FUNC_IN_IRAM=y -- it is not set in this build, and
 * `xtensa-esp32-elf-nm firmware.elf` put it at 0x400d522c, i.e. in the
 * flash-mapped region. An edge arriving during an NVS commit would have
 * panicked the node. It now uses gpio_ll_get_level(), a static inline register
 * read, which is the same idiom esps_dio settled on for the same reason. The
 * two remaining calls are esp_timer_get_time() (IRAM,
 * CONFIG_ESP_TIMER_IN_IRAM=y) and nothing else.
 *
 * NOT VERIFIED ON HARDWARE. The paragraph above is a reading of the linked
 * ELF and this build's sdkconfig, not a measurement. The test that settles it
 * is keying continuously while the station writes a label (which commits to
 * NVS) and seeing the node survive.
 */
#include "esps_morse.h"

#include "esps_morse_key.h"
#include "esps_morse_table.h"
#include "esps_time.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

#include <string.h>

static const char *TAG = "morse";

/* Bytes (ESP-IDF's xTaskCreate takes bytes, not words).
 *
 * MEASURED, not guessed. It used to say 4096 because "the sink's JSON
 * building is the deepest thing reachable from this stack", and that was
 * simply wrong: the deepest branch is the FRAMING one, and it is more than
 * twice as deep as the printing one.
 *
 * `entry a1, N` prologues read off this build with
 * xtensa-esp32-elf-objdump -d firmware.elf:
 *
 *   common head (service_key/service_rx are inlined into the task)
 *     morse_task 128 -> key_step 32 -> publish_all 96 (publish/emit inlined)
 *       -> morse_event_sink 48 -> send_json_frame (inlined)      = 304 B
 *
 *   framing branch
 *     ... -> send_raw_frame 960        (its 900-byte frame buffer)
 *       -> esps_enlp_encode_cobs 1088  (its MAX_FRAME scratch)
 *       -> esps_enlp_encode 48                             = 2400 B
 *
 *   printing branch (governs, summed the conservative way esps_dio does)
 *     ... -> cJSON_PrintUnformatted 32 -> print_value 96
 *       -> sprintf 192 -> _svfprintf_r 800 -> _dtoa_r 160  = 2672 B
 *     cJSON's print_number only takes the sprintf/sscanf round-trip for
 *     non-integral doubles, and every number this component puts in an event
 *     (ts_ms, byte, ms, suppressed) is an integer -- so the 896-byte
 *     __ssvfscanf_r frame is currently unreachable. "Currently" is the
 *     problem: one float added to the event JSON would pull it back in, and
 *     the margin has to survive that.
 *
 * On top of the task's own worst case:
 *   + ~256 B  Xtensa interrupt entry frame -- low/medium-priority ISRs run on
 *             the stack of whatever task they interrupt, not a separate stack
 *   +  ~96 B  this component's own handlers (entry a1, 32 each, plus
 *             esp_timer_get_time)
 *
 *   2672 + 256 + 96 = 3024 B governing.
 *
 * 4096 left 1072 B, 26%. esps_dio faced the same call graph and required 40%,
 * explicitly rejecting 17% because the walk leaves the indirect calls
 * (g_link.send, the UART driver tail) unresolved and the margin has to absorb
 * real unknowns. 6144 gives 3120 B, 51%, and costs 2 KB of DRAM on a node
 * with ~150 KB free. CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY is on, so
 * getting this wrong is a panic while emitting the very event that would
 * explain it.
 *
 * report_stack() below prints the real high-water mark, so this arithmetic is
 * replaced by a measurement the first time the firmware runs. */
#define MORSE_TASK_STACK 6144
/* Below the link tasks on purpose: missing a millisecond of key polling
 * degrades a measurement, while starving the UART loses frames. */
#define MORSE_TASK_PRIO 9
#define MORSE_TASK_CORE 1
#define MORSE_TASK_PERIOD_MS 1u

/* Same depth as the bench sketch's buffer, and for the same reason: it is far
 * more than a human hand can fill between two drains, so an overflow means
 * something is wrong with the line, not with the operator. */
#define EDGE_RING 64u
#define KEY_RING 32u

/* A shorted or ringing line can produce symbols far faster than a hand. The
 * decoder's own debounce drops pulses below debounce_ms, but a clean 60 ms
 * square wave would still be ~16 legitimate symbols a second, forever. This
 * limit keeps the station's event stream readable; the counters and the
 * telemetry channels stay exact regardless, so nothing is lost, only sampled.
 * Letters and words are not limited: they are inherently slow. */
#define SYMBOL_WINDOW_MS 1000u
#define SYMBOL_MAX_IN_WINDOW 20u

/* esps_morse_tick() writes at most two events, esps_morse_edge() at most one. */
#define MAX_EV 2

typedef struct {
    uint32_t t_us;
    uint8_t level;
} edge_t;

/* --- shared with the ISRs ------------------------------------------------ */

static volatile edge_t s_rx_ring[EDGE_RING];
static volatile uint32_t s_rx_head; /* written only by the ISR  */
static volatile uint32_t s_rx_tail; /* written only by the task */
static volatile uint32_t s_rx_dropped;

static volatile edge_t s_key_ring[KEY_RING];
static volatile uint32_t s_key_head;
static volatile uint32_t s_key_tail;
static volatile uint32_t s_key_dropped;

/* --- task-only state ----------------------------------------------------- */

static esps_morse_dec_t s_rx;  /* the other operator's hand */
static esps_morse_dec_t s_tx;  /* the local echo            */
static esps_morse_key_t s_key;
static volatile bool s_ready;
static TaskHandle_t s_task;
static esps_morse_event_sink_fn s_sink;
static void *s_sink_ctx;

static uint32_t s_rl_window_start_ms;
static uint32_t s_rl_in_window;
static uint32_t s_rl_suppressed;

static uint32_t s_reported_rx_dropped;
static uint32_t s_reported_key_dropped;
static uint32_t s_drop_log_ms;
static bool s_stack_reported;

/* The last millisecond stamp handed to the key filter. See key_clock(). */
static uint32_t s_key_last_ms;

/* Thresholds staged by esps_morse_set_thresholds() for the task to install.
 * [0] is RX, [1] is TX. */
static esps_morse_thresholds_t s_pending_th[2];
static volatile bool s_pending[2];

/* --- the NDB table ------------------------------------------------------- */

/* rate_hz is what the node ACTUALLY sends, not what would be nice.
 *
 * main.c's telemetry_task publishes every channel once a second, so every row
 * here says 1 -- including the two level channels. Declaring those at 20 Hz
 * (which is what the digital link's table does) would be a lie the station
 * cannot detect: it would chart one sample a second as if it were twenty.
 *
 * And a level sampled once a second MISSES most keying: a dot lasts ~100 ms.
 * That is why the events are the real record of what was keyed -- morse.symbol
 * and morse.letter fire on the edge that caused them -- and morse.tx/morse.rx
 * are only a coarse "is the line busy right now" indicator. Measured on the
 * bench: a whole SOS went by with morse.tx never once sampled high. */
static const esps_morse_ndb_entry_t NDB[] = {
    {ESPS_MORSE_CH_TX,       "morse.tx",       "Morse TX",      "",   "u8",   1, "morse"},
    {ESPS_MORSE_CH_RX,       "morse.rx",       "Morse RX",      "",   "u8",   1, "morse"},
    {ESPS_MORSE_CH_PULSE_MS, "morse.pulse_ms", "Last pulse",    "ms", "u32",  1, "morse"},
    {ESPS_MORSE_CH_GAP_MS,   "morse.gap_ms",   "Last gap",      "ms", "u32",  1, "morse"},
    {ESPS_MORSE_CH_SYMBOLS,  "morse.symbols",  "Symbols",       "",   "u32",  1, "morse"},
    {ESPS_MORSE_CH_LETTERS,  "morse.letters",  "Letters",       "",   "u32",  1, "morse"},
    {ESPS_MORSE_CH_UNKNOWN,  "morse.unknown",  "Unknown codes", "",   "u32",  1, "morse"},
    {ESPS_MORSE_CH_BOUNCES,  "morse.bounces",  "Key bounces",   "",   "u32",  1, "morse"},
};
_Static_assert(sizeof(NDB) / sizeof(NDB[0]) == ESPS_MORSE_NDB_COUNT,
               "ESPS_MORSE_NDB_COUNT must match the table");

const esps_morse_ndb_entry_t *esps_morse_ndb(size_t *out_count) {
    if (out_count != NULL) {
        *out_count = ESPS_MORSE_NDB_COUNT;
    }
    return NDB;
}

/* --- ISRs ---------------------------------------------------------------- */

/* Rules: IRAM_ATTR, no libc, no logging, no allocation, constant work. Read
 * the clock FIRST so the stamp is as close to the edge as the hardware
 * allows; reading the pin first would charge the read latency to the wrong
 * side of the edge. */
static void IRAM_ATTR isr_push(volatile edge_t *ring, uint32_t depth,
                               volatile uint32_t *head, volatile uint32_t *tail,
                               volatile uint32_t *dropped, int pin) {
    const uint32_t now = (uint32_t)esp_timer_get_time();
    /* gpio_ll_get_level, not gpio_get_level: the latter is an out-of-line
     * function in flash in this build, and calling it from a handler
     * registered with ESP_INTR_FLAG_IRAM panics the moment the cache is off.
     * See the file header. */
    const uint8_t level = (uint8_t)gpio_ll_get_level(&GPIO, (gpio_num_t)pin);
    const uint32_t h = *head;
    const uint32_t next = (h + 1u) % depth;
    if (next == *tail) {
        (*dropped)++;   /* full: count it, never overwrite unread data */
        return;
    }
    ring[h].t_us = now;
    ring[h].level = level;
    *head = next;
}

static void IRAM_ATTR isr_rx(void *arg) {
    (void)arg;
    isr_push(s_rx_ring, EDGE_RING, &s_rx_head, &s_rx_tail, &s_rx_dropped,
             ESPS_MORSE_PIN_RX_DATA);
}

static void IRAM_ATTR isr_key(void *arg) {
    (void)arg;
    isr_push(s_key_ring, KEY_RING, &s_key_head, &s_key_tail, &s_key_dropped,
             ESPS_MORSE_PIN_KEY);
}

/* --- the two clocks ------------------------------------------------------- */

/* This component uses TWO time bases and they are not interchangeable:
 *
 *   - 32-bit MICROSECONDS, (uint32_t)esp_timer_get_time(), for the decoders.
 *     Wraps every 71.6 min, at a power of two, so the decoder's unsigned
 *     subtraction is exact across the wrap [D-10].
 *   - 32-bit MILLISECONDS, esps_time_now_ms(), for the key filter and the
 *     rate limiter. Wraps every 49.7 days, also at a power of two.
 *
 * The bug this replaces mixed them: the key filter was fed `t_us / 1000u` for
 * a captured edge and esps_time_now_ms() for the acceptance poll. Those two
 * agree only while the microsecond counter has not wrapped -- for the first
 * 71.6 minutes of uptime -- and afterwards one restarts near zero while the
 * other keeps climbing. From then on every `(uint32_t)(now_ms - t_candidate)`
 * is an enormous number, always >= debounce_ms, and the key debounce is
 * silently off for the rest of the run, with the bounce counter reporting
 * zero because every raw change became an accepted edge.
 *
 * So: one esp_timer read reported in both units, and a captured stamp is
 * converted to milliseconds by its AGE, never by division. */
static uint32_t morse_now(uint32_t *out_us) {
    const int64_t t = esp_timer_get_time();
    *out_us = (uint32_t)t;
    return (uint32_t)(t / 1000); /* identical to esps_time_now_ms() */
}

/* Millisecond stamp, in the node's ms base, of an edge the ISR captured at
 * `t_us`, relative to the reference pair (`ref_us`, `ref_ms`) read from one
 * esp_timer call.
 *
 * An edge captured AFTER the reference read is not hypothetical: the ISR can
 * fire while this task is draining the ring. Its age would underflow to
 * ~4.29e9 us, and the filter would read that as "held long enough". Such a
 * stamp is clamped to the reference, i.e. treated as "now" -- never as the
 * future. */
static uint32_t stamp_ms(uint32_t t_us, uint32_t ref_us, uint32_t ref_ms) {
    const uint32_t age_us = ref_us - t_us;
    if (age_us >= 0x80000000u) {
        return ref_ms;
    }
    return ref_ms - age_us / 1000u;
}

/* The key filter asks "has the reading held for debounce_ms?" with an
 * unsigned subtraction, so a stamp that goes BACKWARDS reads as ~49 days and
 * accepts on the spot -- which is exactly the bounce the filter exists to
 * reject. Two things can make a stamp go backwards even after stamp_ms():
 * a ring entry captured before the previous iteration's acceptance poll, and
 * the drain and the poll reading the clock at different moments. The guard
 * belongs here and not in the pure half, which cannot know about either.
 *
 * Signed comparison, so it stays correct across the 49.7 day wrap: successive
 * stamps are milliseconds apart, never 24.8 days apart. */
static uint32_t key_clock(uint32_t at_ms) {
    if ((int32_t)(at_ms - s_key_last_ms) < 0) {
        return s_key_last_ms;
    }
    s_key_last_ms = at_ms;
    return at_ms;
}

/* --- events -------------------------------------------------------------- */

/* Reports the task's stack high-water mark once, so nobody has to trust the
 * arithmetic at MORSE_TASK_STACK. Called right after the first event has
 * RETURNED, because emitting an event is the deepest path in the component
 * and a panic there would take with it the number that explains the panic. */
static void report_stack(const char *when) {
    if (s_stack_reported || s_task == NULL) {
        return;
    }
    s_stack_reported = true;
    /* ESP-IDF's uxTaskGetStackHighWaterMark returns bytes, not words. */
    const unsigned free_b = (unsigned)uxTaskGetStackHighWaterMark(s_task);
    ESP_LOGI(TAG, "morse task stack after %s: %u B never used of %u B allocated (%u B used)",
             when, free_b, (unsigned)MORSE_TASK_STACK,
             (unsigned)MORSE_TASK_STACK - free_b);
}

void esps_morse_set_event_sink(esps_morse_event_sink_fn fn, void *ctx) {
    s_sink = fn;
    s_sink_ctx = ctx;
}

/* Window-based limiter, like the digital link's: simpler than a token bucket,
 * and the only property that matters here is a bound per second. */
static bool rl_admit(uint32_t now_ms) {
    if ((uint32_t)(now_ms - s_rl_window_start_ms) >= SYMBOL_WINDOW_MS) {
        s_rl_window_start_ms = now_ms;
        s_rl_in_window = 0;
    }
    if (s_rl_in_window >= SYMBOL_MAX_IN_WINDOW) {
        s_rl_suppressed++;
        return false;
    }
    s_rl_in_window++;
    return true;
}

static void emit(const esps_morse_station_event_t *ev) {
    if (s_sink != NULL) {
        s_sink(ev, s_sink_ctx);
        report_stack("the first event");
    }
}

/* Translates one decoder decision into a station event and sends it.
 * `dir` is a static literal: "RX" or "TX". */
static void publish(const esps_morse_event_t *e, const char *dir, uint32_t now_ms) {
    esps_morse_station_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.dir = dir;
    ev.ms = e->ms;

    switch (e->kind) {
        case ESPS_MORSE_EVENT_SYMBOL:
            if (!rl_admit(now_ms)) {
                return;
            }
            ev.code = ESPS_MORSE_EV_SYMBOL;
            ev.severity = ESPS_MORSE_SEV_DEBUG;
            ev.symbol = e->symbol;
            break;
        case ESPS_MORSE_EVENT_FILTERED:
            if (!rl_admit(now_ms)) {
                return;
            }
            ev.code = ESPS_MORSE_EV_FILTERED;
            /* A filtered pulse is noise on a line that should be quiet, so it
             * is a warning and not debug: it is the first thing to look at
             * when letters start coming out wrong. */
            ev.severity = ESPS_MORSE_SEV_WARNING;
            break;
        case ESPS_MORSE_EVENT_LETTER:
            ev.code = ESPS_MORSE_EV_LETTER;
            ev.severity = ESPS_MORSE_SEV_INFO;
            ev.letter = e->letter;
            memcpy(ev.code_buf, e->code, sizeof(ev.code_buf));
            ev.code_buf[sizeof(ev.code_buf) - 1] = '\0';
            ev.text = ev.code_buf;
            break;
        case ESPS_MORSE_EVENT_UNKNOWN:
            ev.code = ESPS_MORSE_EV_UNKNOWN;
            ev.severity = ESPS_MORSE_SEV_WARNING;
            memcpy(ev.code_buf, e->code, sizeof(ev.code_buf));
            ev.code_buf[sizeof(ev.code_buf) - 1] = '\0';
            ev.text = ev.code_buf;
            break;
        case ESPS_MORSE_EVENT_WORD:
            ev.code = ESPS_MORSE_EV_WORD;
            ev.severity = ESPS_MORSE_SEV_DEBUG;
            break;
        default:
            return;
    }

    /* Report the backlog on the first event that gets through after a burst,
     * then clear it: the station can tell a sample from the whole stream. */
    ev.suppressed = s_rl_suppressed;
    s_rl_suppressed = 0;
    emit(&ev);
}

static void publish_all(const esps_morse_event_t *evs, size_t n, const char *dir,
                        uint32_t now_ms) {
    for (size_t i = 0; i < n; i++) {
        publish(&evs[i], dir, now_ms);
    }
}

/* --- the task ------------------------------------------------------------ */

/* One step of the key filter, with the wire write an acceptance implies.
 *
 * The accepted edge is stamped with the time of the WIRE WRITE, not with the
 * time of the contact change that caused it. That is deliberate and it is a
 * fix: the far end measures durations off the wire, and the local TX echo is
 * only worth having if it describes the same signal the far end sees. The old
 * code stamped a drain-path acceptance with the contact-change time and a
 * poll-path acceptance with the poll time, so the echo's pulse durations and
 * the far end's could differ by up to debounce_ms with nothing saying so --
 * and the duration comparison between the two boards is the whole point of
 * the practice (SPEC-DUPLEX.md, "Lo que si se puede medir"). */
static void key_step(uint8_t reading, uint32_t at_ms, esps_morse_event_t *evs) {
    const int accepted = esps_morse_key_sample(&s_key, reading, key_clock(at_ms));
    if (accepted < 0) {
        return;
    }
    gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_TX_DATA, accepted);
    gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_LED_TX, accepted);
    uint32_t wire_us = 0;
    const uint32_t wire_ms = morse_now(&wire_us);
    const size_t n = esps_morse_edge(&s_tx, (uint8_t)accepted, wire_us, evs, MAX_EV);
    publish_all(evs, n, "TX", wire_ms);
}

/* Replays every raw key change the ISR captured, in order, through the pure
 * filter, then polls once with the current level so an edge whose debounce
 * deadline has passed is accepted this millisecond. Writes the wire the
 * moment one is.
 *
 * Reads its own clock rather than taking the caller's: the reference has to
 * be no older than the ring entries it is used to date, and the acceptance
 * poll has to be no older than the drain that preceded it. */
static void service_key(void) {
    esps_morse_event_t evs[MAX_EV];
    uint32_t ref_us = 0;
    const uint32_t ref_ms = morse_now(&ref_us);

    while (s_key_tail != s_key_head) {
        const uint32_t t = s_key_tail;
        const uint8_t level = s_key_ring[t].level;
        const uint32_t t_us = s_key_ring[t].t_us;
        s_key_tail = (t + 1u) % KEY_RING;

        /* The captured change gets its OWN timestamp: dating it "now" would
         * restart the stability clock late and let a bounce look stable. */
        const uint32_t at_ms = stamp_ms(t_us, ref_us, ref_ms);

        /* TWO steps, and the order matters.
         *
         * First advance the filter's clock to this change's own time WITHOUT
         * changing the reading -- feeding the current candidate back is a
         * no-op for change detection and evaluates the pending deadline. Then
         * feed the change itself.
         *
         * Without the first step, a press that began AND ended between two
         * task periods was thrown away: the second ring entry hit the "the
         * reading changed" branch and returned before the first entry's
         * deadline had ever been looked at, so a perfectly legal 20 ms press
         * never reached the wire and nothing counted it. It needs the task to
         * miss >= debounce_ms, which is a contention symptom rather than a
         * normal-path one -- and a link that silently drops symbols under
         * load is exactly the failure a bench session blames on the operator. */
        key_step(s_key.candidate, at_ms, evs);
        key_step(level, at_ms, evs);
    }

    /* The acceptance poll, on a reading of the live pin and a clock read
     * taken AFTER the drain so the stamps never go backwards. Nothing
     * necessarily changed on the pin, so this only asks "has the candidate
     * held long enough yet?". */
    uint32_t now_us = 0;
    const uint32_t now_ms = morse_now(&now_us);
    key_step((uint8_t)gpio_get_level((gpio_num_t)ESPS_MORSE_PIN_KEY), now_ms, evs);
}

static void service_rx(uint32_t now_ms) {
    esps_morse_event_t evs[MAX_EV];
    while (s_rx_tail != s_rx_head) {
        const uint32_t t = s_rx_tail;
        const uint8_t level = s_rx_ring[t].level;
        const uint32_t t_us = s_rx_ring[t].t_us;
        s_rx_tail = (t + 1u) % EDGE_RING;

        gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_LED_RX, level);
        const size_t n = esps_morse_edge(&s_rx, level, t_us, evs, MAX_EV);
        publish_all(evs, n, "RX", now_ms);
    }
}

/* A dropped edge is a measurement that silently never happened, which is the
 * one failure mode this practice must never hide: the station's cross-check
 * would show a mismatch with no explanation.
 *
 * Throttled to once a second. The unthrottled version logged on every task
 * period in which a drop had happened, i.e. up to 1000 lines a second on a
 * line oscillating fast enough to keep the ring full -- and ESP_LOGW here is
 * not cheap: the log hook reframes each line into an ENLP LOG frame and hands
 * it to the UART, which BLOCKS once its TX buffer fills. A blocking call at
 * 1 kHz on a priority-9 task is how a diagnostic turns into the fault it was
 * reporting. One line per second still names every dropped edge, because the
 * count is a difference and not a rate. */
#define DROP_LOG_PERIOD_MS 1000u

static void report_drops(uint32_t now_ms) {
    const uint32_t rx = s_rx_dropped;
    const uint32_t key = s_key_dropped;
    if (rx == s_reported_rx_dropped && key == s_reported_key_dropped) {
        return;
    }
    if ((uint32_t)(now_ms - s_drop_log_ms) < DROP_LOG_PERIOD_MS) {
        return;
    }
    s_drop_log_ms = now_ms;
    if (rx != s_reported_rx_dropped) {
        ESP_LOGW(TAG, "RX ring overflow: %lu edges dropped",
                 (unsigned long)(rx - s_reported_rx_dropped));
        s_reported_rx_dropped = rx;
    }
    if (key != s_reported_key_dropped) {
        ESP_LOGW(TAG, "key ring overflow: %lu changes dropped",
                 (unsigned long)(key - s_reported_key_dropped));
        s_reported_key_dropped = key;
    }
}

/* Installs whatever esps_morse_set_thresholds() staged. Runs on the task and
 * between decoder calls, so a decoder can never observe half of a set. */
static void apply_pending_thresholds(void) {
    for (int i = 0; i < 2; i++) {
        if (!s_pending[i]) {
            continue;
        }
        s_pending[i] = false;
        (void)esps_morse_dec_set_thresholds(i == 0 ? &s_rx : &s_tx, &s_pending_th[i]);
    }
}

static void morse_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "transceiver up: key=%d tx=%d rx=%d (p=%lu l=%lu w=%lu d=%lu)",
             ESPS_MORSE_PIN_KEY, ESPS_MORSE_PIN_TX_DATA, ESPS_MORSE_PIN_RX_DATA,
             (unsigned long)s_rx.th.dot_dash_ms, (unsigned long)s_rx.th.letter_ms,
             (unsigned long)s_rx.th.word_ms, (unsigned long)s_rx.th.debounce_ms);

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        uint32_t now_us = 0;
        uint32_t now_ms = morse_now(&now_us);
        esps_morse_event_t evs[MAX_EV];

        apply_pending_thresholds();
        service_rx(now_ms);
        service_key();

        /* THE CLOCK IS RE-READ HERE, and that is a fix, not tidiness.
         *
         * Servicing above feeds the decoders edges stamped by the ISR, and an
         * ISR can fire after the read at the top of this loop -- the window is
         * the whole of service_rx() plus the drain loop's own re-check. Such
         * an edge lands in the decoder with t_fall_us AHEAD of `now_us`, and
         * esps_morse_tick()'s wrap-safe unsigned subtraction cannot tell "30
         * microseconds in the future" from "71.6 minutes ago": it computes a
         * gap of ~4294967 ms, which clears letter_ms and word_ms at once, and
         * tears the symbol that just arrived off into its own letter followed
         * by a word gap. On a hand keying ~10 symbols a second that is a
         * mangled letter every few minutes, arriving as a perfectly
         * well-formed event with nothing marking it as wrong.
         *
         * A read taken after servicing is no earlier than any stamp the
         * decoders now hold, which is exactly the contract tick() needs. */
        now_ms = morse_now(&now_us);

        /* Ticks close letters and words. Both decoders every period: a
         * silence is only a silence once enough of it has passed, and
         * nothing else will notice. */
        size_t n = esps_morse_tick(&s_rx, now_us, evs, MAX_EV);
        publish_all(evs, n, "RX", now_ms);
        n = esps_morse_tick(&s_tx, now_us, evs, MAX_EV);
        publish_all(evs, n, "TX", now_ms);

        report_drops(now_ms);
        if (now_ms >= 15000u) {
            report_stack("15 s");
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(MORSE_TASK_PERIOD_MS));
    }
}

/* --- thresholds and telemetry --------------------------------------------- */

/* Staged rather than written straight into the decoder.
 *
 * esps_morse_dec_set_thresholds() copies a 16-byte struct. Called from
 * another task -- and the whole reason this function exists is for S3 to wire
 * an EXP_SET to it, which arrives on the link's RX task -- that copy races
 * the 1 ms task reading th.debounce_ms and th.dot_dash_ms inside
 * esps_morse_edge(). The decoder could then run on a new dot_dash_ms with an
 * old letter_ms: a combination no caller ever asked for and
 * esps_morse_thresholds_valid() never saw. Validating here and letting the
 * task install the set between decoder calls makes the tear impossible
 * without putting a lock on a 1 ms path.
 *
 * The coherence rules are still the decoder's, so a bad EXP_SET can never
 * leave a decoder in a state where letters never close.
 *
 * Only one set per direction can be in flight; a second before the task has
 * run replaces the first. At a 1 ms period that is a millisecond-wide window
 * and a station sending two EXP_SETs inside it has no defined ordering
 * anyway. */
bool esps_morse_set_thresholds(bool rx, const esps_morse_thresholds_t *th) {
    if (!esps_morse_thresholds_valid(th)) {
        return false;
    }
    const int i = rx ? 0 : 1;
    s_pending[i] = false; /* stop the task consuming a half-written copy */
    s_pending_th[i] = *th;
    s_pending[i] = true;
    if (!s_ready) {
        /* No task to install it: do it here, where there is no race. */
        apply_pending_thresholds();
    }
    return true;
}

bool esps_morse_get_thresholds(bool rx, esps_morse_thresholds_t *out) {
    if (out == NULL) {
        return false;
    }
    const int i = rx ? 0 : 1;
    /* Report what set() last accepted, not what is installed, so a get
     * immediately after a set does not hand back the old values during the
     * millisecond before the task picks the new ones up. */
    if (s_pending[i]) {
        *out = s_pending_th[i];
        return true;
    }
    *out = rx ? s_rx.th : s_tx.th;
    return true;
}

void esps_morse_get_sample(esps_morse_sample_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_ready) {
        /* Before start(), every channel reads 0 rather than whatever the
         * pads happen to be: the pins are not configured yet, so a "level"
         * read from them would be noise presented as a measurement. */
        return;
    }
    /* Read the pads back rather than trusting a shadow variable: if anything
     * else reconfigured a pin, the channel should show what is really there
     * and not what this component believes it wrote. */
    out->tx_level = (uint8_t)gpio_get_level((gpio_num_t)ESPS_MORSE_PIN_TX_DATA);
    out->rx_level = (uint8_t)gpio_get_level((gpio_num_t)ESPS_MORSE_PIN_RX_DATA);
    out->pulse_ms = s_rx.last_pulse_ms;
    out->gap_ms = s_rx.last_gap_ms;
    out->symbols = s_rx.dots + s_rx.dashes;
    out->letters = s_rx.letters;
    out->unknown = s_rx.unknown;
    out->bounces = esps_morse_key_bounces(&s_key);
}

/* --- configuration -------------------------------------------------------- */

static bool configure_pins(void) {
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << ESPS_MORSE_PIN_TX_DATA) |
                        (1ULL << ESPS_MORSE_PIN_LED_TX) |
                        (1ULL << ESPS_MORSE_PIN_LED_RX),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(outputs) failed: %s", esp_err_to_name(err));
        return false;
    }
    gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_TX_DATA, 0);
    gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_LED_TX, 0);
    gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_LED_RX, 0);

    /* Pull-downs on both inputs: an unplugged cable and an open key both read
     * a stable 0 rather than floating and inventing edges. */
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << ESPS_MORSE_PIN_RX_DATA) |
                        (1ULL << ESPS_MORSE_PIN_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    err = gpio_config(&in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(inputs) failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool install_isrs(void) {
    /* ESP_INTR_FLAG_IRAM so the handlers keep running while a flash write is
     * in progress (NVS at boot, node.set_label): an edge lost to a flash
     * erase would be invisible in the data and blamed on the operator.
     * ESP_ERR_INVALID_STATE means another component already installed the
     * service, which is fine -- it is per-chip, not per-pin. */
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return false;
    }
    err = gpio_isr_handler_add((gpio_num_t)ESPS_MORSE_PIN_RX_DATA, isr_rx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isr_handler_add(rx) failed: %s", esp_err_to_name(err));
        return false;
    }
    err = gpio_isr_handler_add((gpio_num_t)ESPS_MORSE_PIN_KEY, isr_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "isr_handler_add(key) failed: %s", esp_err_to_name(err));
        gpio_isr_handler_remove((gpio_num_t)ESPS_MORSE_PIN_RX_DATA);
        return false;
    }
    return true;
}

/* Leaving the handlers installed with no task to drain what they push is not
 * harmless: the rings fill once and then every edge is counted as dropped
 * forever, and nobody is running to say so. main.c logs "the node continues
 * without it" and means it. */
static void remove_isrs(void) {
    gpio_isr_handler_remove((gpio_num_t)ESPS_MORSE_PIN_RX_DATA);
    gpio_isr_handler_remove((gpio_num_t)ESPS_MORSE_PIN_KEY);
}

bool esps_morse_start(void) {
    if (s_ready) {
        return true;
    }

    esps_morse_thresholds_t th;
    esps_morse_thresholds_init(&th);
    esps_morse_dec_init(&s_rx, &th);
    esps_morse_dec_init(&s_tx, &th);

    if (!configure_pins()) {
        return false;
    }

    /* Seed the filter and both decoders with the levels that are actually on
     * the pads right now. Assuming 0 would fabricate an edge if the operator
     * booted with a finger on the key or the far end already keying. */
    const uint8_t key_level = (uint8_t)gpio_get_level((gpio_num_t)ESPS_MORSE_PIN_KEY);
    const uint8_t rx_level = (uint8_t)gpio_get_level((gpio_num_t)ESPS_MORSE_PIN_RX_DATA);
    esps_morse_key_init(&s_key, ESPS_MORSE_DEFAULT_DEBOUNCE_MS, key_level);
    s_rx.level = rx_level;
    s_tx.level = key_level;
    gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_TX_DATA, key_level);
    gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_LED_TX, key_level);
    gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_LED_RX, rx_level);

    if (!install_isrs()) {
        return false;
    }

    s_rl_window_start_ms = esps_time_now_ms();
    s_drop_log_ms = s_rl_window_start_ms;
    /* The key filter's stamps must never go backwards, and the first one it
     * ever sees has to start the sequence somewhere. */
    s_key_last_ms = s_rl_window_start_ms;
    s_ready = true;

    if (xTaskCreatePinnedToCore(morse_task, "morse", MORSE_TASK_STACK, NULL,
                                MORSE_TASK_PRIO, &s_task, MORSE_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        s_task = NULL;
        s_ready = false;
        remove_isrs();
        return false;
    }
    return true;
}

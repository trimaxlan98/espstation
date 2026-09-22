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
 * nothing from the pure half. The handlers themselves are IRAM_ATTR and the
 * ring lives in DRAM, which is the whole requirement.
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

#include <string.h>

static const char *TAG = "morse";

/* Bytes (ESP-IDF's xTaskCreate takes bytes, not words). The task runs two
 * decoders, a filter and the event sink; 4 KiB leaves room for the sink's
 * JSON building, which is the deepest thing reachable from this stack. */
#define MORSE_TASK_STACK 4096
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
    const uint8_t level = (uint8_t)gpio_get_level((gpio_num_t)pin);
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

/* --- events -------------------------------------------------------------- */

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

/* Replays every raw key change the ISR captured, in order, through the pure
 * filter, then polls once with the current level so an edge whose debounce
 * deadline has passed is accepted this millisecond. Writes the wire the
 * moment one is. */
static void service_key(uint32_t now_ms, uint32_t now_us) {
    esps_morse_event_t evs[MAX_EV];

    while (s_key_tail != s_key_head) {
        const uint32_t t = s_key_tail;
        const uint8_t level = s_key_ring[t].level;
        const uint32_t t_us = s_key_ring[t].t_us;
        s_key_tail = (t + 1u) % KEY_RING;

        /* The captured change gets its OWN timestamp: feeding it `now_ms`
         * would restart the stability clock a millisecond late and let a
         * bounce look stable. */
        const int accepted = esps_morse_key_sample(&s_key, level, t_us / 1000u);
        if (accepted >= 0) {
            gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_TX_DATA, accepted);
            gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_LED_TX, accepted);
            const size_t n = esps_morse_edge(&s_tx, (uint8_t)accepted, t_us, evs, MAX_EV);
            publish_all(evs, n, "TX", now_ms);
        }
    }

    /* The acceptance poll. Nothing changed on the pin, so this only asks
     * "has the candidate held long enough yet?". */
    const uint8_t level = (uint8_t)gpio_get_level((gpio_num_t)ESPS_MORSE_PIN_KEY);
    const int accepted = esps_morse_key_sample(&s_key, level, now_ms);
    if (accepted >= 0) {
        gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_TX_DATA, accepted);
        gpio_set_level((gpio_num_t)ESPS_MORSE_PIN_LED_TX, accepted);
        const size_t n = esps_morse_edge(&s_tx, (uint8_t)accepted, now_us, evs, MAX_EV);
        publish_all(evs, n, "TX", now_ms);
    }
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
 * would show a mismatch with no explanation. */
static void report_drops(void) {
    const uint32_t rx = s_rx_dropped;
    const uint32_t key = s_key_dropped;
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

static void morse_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "transceiver up: key=%d tx=%d rx=%d (p=%lu l=%lu w=%lu d=%lu)",
             ESPS_MORSE_PIN_KEY, ESPS_MORSE_PIN_TX_DATA, ESPS_MORSE_PIN_RX_DATA,
             (unsigned long)s_rx.th.dot_dash_ms, (unsigned long)s_rx.th.letter_ms,
             (unsigned long)s_rx.th.word_ms, (unsigned long)s_rx.th.debounce_ms);

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        const uint32_t now_us = (uint32_t)esp_timer_get_time();
        const uint32_t now_ms = esps_time_now_ms();
        esps_morse_event_t evs[MAX_EV];

        service_rx(now_ms);
        service_key(now_ms, now_us);

        /* Ticks close letters and words. Both decoders every period: a
         * silence is only a silence once enough of it has passed, and
         * nothing else will notice. */
        publish_all(evs, esps_morse_tick(&s_rx, now_us, evs, MAX_EV), "RX", now_ms);
        publish_all(evs, esps_morse_tick(&s_tx, now_us, evs, MAX_EV), "TX", now_ms);

        report_drops();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(MORSE_TASK_PERIOD_MS));
    }
}

/* --- thresholds and telemetry --------------------------------------------- */

bool esps_morse_set_thresholds(bool rx, const esps_morse_thresholds_t *th) {
    /* The decoder validates and refuses incoherent sets itself, so a bad
     * EXP_SET can never leave a decoder in a state where letters never
     * close. */
    return esps_morse_dec_set_thresholds(rx ? &s_rx : &s_tx, th);
}

bool esps_morse_get_thresholds(bool rx, esps_morse_thresholds_t *out) {
    if (out == NULL) {
        return false;
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
        return false;
    }
    return true;
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
    s_ready = true;

    if (xTaskCreatePinnedToCore(morse_task, "morse", MORSE_TASK_STACK, NULL,
                                MORSE_TASK_PRIO, &s_task, MORSE_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        s_ready = false;
        return false;
    }
    return true;
}

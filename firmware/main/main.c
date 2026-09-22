/* main.c — wiring only, no logic that belongs in a component.
 *
 * Boot order matters here and mirrors the dependency chain, not just
 * convenience: NVS before node identity (identity is persisted there),
 * node identity before the link (frames need a node id to put in the
 * header), the link before the log hook (the hook needs somewhere to send
 * frames), and the log hook before anything chatty happens (so as much of
 * boot as possible is visible to the station as structured LOG frames
 * rather than being lost).
 *
 * CMD handling runs synchronously inside the link's RX callback (i.e. on
 * the UART RX task). That satisfies PROTOCOL.md S4.7's "single-threaded and
 * non-blocking" requirement for every op here except node.reboot, which
 * answers CMD_ACK first and then hands off to a short-lived task to
 * actually restart — restarting inline would race the ACK frame still
 * sitting in the TX queue.
 */
#include "esps_enlp.h"
#include "esps_frame.h"
#include "esps_health.h"
#include "esps_link.h"
#include "esps_link_uart.h"
#include "esps_log_hook.h"
#include "esps_node_id.h"
#include "esps_time.h"

/* The digital link is a build variant (esp32dev_dio_a / esp32dev_dio_b in
 * platformio.ini), not a runtime setting, because the experiment runtime that
 * would configure it does not exist until S3. With ESPS_DIO_ROLE unset or 0
 * this file does not reference esps_dio at all and the firmware is the base
 * firmware, unchanged.
 * TODO(S3): role, bit rate and phase periods come from the EXP_SET spec. */
#if defined(ESPS_DIO_ROLE) && (ESPS_DIO_ROLE != 0)
#define ESPS_DIO_ENABLED 1
#include "esps_dio.h"
#else
#define ESPS_DIO_ENABLED 0
#endif

/* The Morse transceiver is symmetric: both boards run the same binary, so
 * this is an on/off and not a role. Set by the platformio environment
 * esp32dev_morse. With it unset this file does not reference esps_morse at
 * all and the firmware is the base one. */
#if defined(ESPS_MORSE_BUILD) && (ESPS_MORSE_BUILD != 0)
#define ESPS_MORSE_ENABLED 1
#include "esps_morse.h"
#else
#define ESPS_MORSE_ENABLED 0
#endif

#include "cJSON.h"

#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

#ifndef ESPS_FW_VERSION
#define ESPS_FW_VERSION "0.0.0-dev" /* platformio.ini normally supplies the real value */
#endif

static const char *TAG = "main";

/* NDB channel ids for the mandatory system channels (espstation.protocol.yaml
 * `system_channels`, ids 1-3; sys.vbat/sys.temp are `optional: true` and this
 * board has no sensor for either, so they are not declared). */
#define ESPS_CH_SYS_HEAP_FREE 1
#define ESPS_CH_SYS_RSSI 2
#define ESPS_CH_SYS_UPTIME 3

#define ESPS_HELLO_RETRY_MS 30000

/* The two ceilings a single frame's payload has to clear. Both are silent
 * when exceeded — esps_enlp_encode_cobs() simply returns 0 — which is why
 * they are computed from the real constants here rather than assumed:
 *
 *   - PROTOCOL.md S3 caps any payload at ESPS_ENLP_MAX_PAYLOAD (1024 B).
 *   - the UART transport encodes into an ESPS_LINK_UART_MAX_FRAME buffer that
 *     has to hold the 8-byte header, the payload, the 2-byte CRC, COBS's own
 *     overhead (one byte per 254 of body) and the 0x00 delimiter. That works
 *     out smaller than the protocol ceiling, so it is the binding one.
 *
 * Getting this wrong produces a node that never announces itself and says
 * nothing about why, which is exactly what a full NDB used to do here. */
#define ESPS_FRAME_UART_PAYLOAD_MAX                                                          \
    (ESPS_LINK_UART_MAX_FRAME - 1u /* delimiter */                                           \
     - (ESPS_ENLP_HEADER_SIZE + ESPS_ENLP_CRC_SIZE) /* frame body overhead */                \
     - ((ESPS_LINK_UART_MAX_FRAME / 254u) + 1u) /* worst-case COBS overhead */)

#define ESPS_FRAME_PAYLOAD_MAX                                                               \
    ((ESPS_FRAME_UART_PAYLOAD_MAX < ESPS_ENLP_MAX_PAYLOAD) ? ESPS_FRAME_UART_PAYLOAD_MAX     \
                                                           : ESPS_ENLP_MAX_PAYLOAD)

/* Margin under the hard ceiling when deciding how many NDB channels to pack
 * into one HELLO. ndb_entry_json_bytes() is already an upper bound per entry,
 * but the one message that makes a node visible at all is not the place to
 * bet on an estimate being exact. */
#define ESPS_HELLO_NDB_MARGIN  32u
#define ESPS_HELLO_CHUNK_MAX   8u /* HELLO frames per announcement round */

/* Every NDB channel this firmware can declare: the 3 system channels plus the
 * 6 the digital link adds. */
#define ESPS_NDB_MAX 16u

static esps_link_if_t g_link;

/* HELLO_ACK accounting across a chunked announcement.
 *
 * The station answers one HELLO_ACK per HELLO it receives, so with a chunked
 * NDB a single ACK proves only that ONE chunk arrived. Stopping on the first
 * one loses every other chunk permanently, and the node ends up advertising
 * half its channels forever (finding A2). The round is complete only when as
 * many accepted ACKs have come back as chunks went out.
 *
 * Concurrency: g_hello_acks is incremented by on_frame() on the link's RX task
 * and read by hello_task. One writer, one reader, 32-bit aligned, so a plain
 * volatile is sufficient — no read-modify-write can be lost because no second
 * task ever increments it. hello_task resets it BEFORE sending the round's
 * first chunk, which is the ordering that matters: an ACK arriving mid-round
 * counts towards the round in flight, and any ACK still in flight from the
 * previous round is correctly discarded. */
static volatile uint32_t g_hello_acks = 0;

/* --- small framing helpers -------------------------------------------------- */

static bool link_sink(const uint8_t *frame, size_t len, void *ctx) {
    (void)ctx;
    return g_link.send(&g_link, frame, len);
}

static bool send_raw_frame(uint8_t type, const uint8_t *payload, size_t payload_len) {
    uint8_t frame[ESPS_LINK_UART_MAX_FRAME];
    size_t frame_len = esps_enlp_encode_cobs(ESPS_ENLP_VERSION, type, esps_node_id_get(),
                                              esps_frame_next_seq(), payload, payload_len, frame,
                                              sizeof(frame));
    if (frame_len == 0) {
        /* The only ways the encoder returns 0 are an over-long payload or a
         * destination that cannot hold the encoded frame — both are bugs in
         * the caller, and both used to drop the frame without a trace. A
         * message that is too big to send must never be indistinguishable
         * from a message that was sent. */
        ESP_LOGE(TAG, "frame type 0x%02x DROPPED: %u B payload exceeds the %u B this transport "
                      "can carry",
                 type, (unsigned)payload_len, (unsigned)ESPS_FRAME_PAYLOAD_MAX);
        return false;
    }
    return g_link.send(&g_link, frame, frame_len);
}

/* JSON is the control-plane encoding (PROTOCOL.md S1.1) precisely because it
 * is low-rate — the malloc cJSON does internally is fine here and would not
 * be fine in the TELEMETRY/HEARTBEAT/LOG hot path, which never uses it. */
static bool send_json_frame(uint8_t type, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return false;
    }
    bool ok = send_raw_frame(type, (const uint8_t *)json, strlen(json));
    cJSON_free(json);
    return ok;
}

/* --- HELLO ------------------------------------------------------------------- */

static const char *reset_reason_str(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "int_wdt";
        case ESP_RST_TASK_WDT:
            return "task_wdt";
        case ESP_RST_WDT:
            return "wdt";
        case ESP_RST_DEEPSLEEP:
            return "deepsleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        default:
            return "unknown";
    }
}

static const char *chip_model_str(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:
            return "esp32";
        case CHIP_ESP32S2:
            return "esp32s2";
        case CHIP_ESP32S3:
            return "esp32s3";
        case CHIP_ESP32C3:
            return "esp32c3";
#ifdef CHIP_ESP32C6
        case CHIP_ESP32C6:
            return "esp32c6";
#endif
        default:
            return "unknown";
    }
}

/* One NDB channel declaration, in the shape PROTOCOL.md S4.1 puts on the
 * wire. Kept as data rather than as straight-line cJSON calls so the same
 * chunking code can place the system channels and the link's channels. */
typedef struct {
    uint8_t id;
    const char *key;
    const char *name;
    const char *unit;
    const char *type;
    uint16_t rate_hz;
    const char *group;
} ndb_entry_t;

static const ndb_entry_t g_sys_ndb[] = {
    {ESPS_CH_SYS_HEAP_FREE, "sys.heap_free", "Heap free", "B", "u32", 1, "system"},
    /* Declared per system_channels even though this sprint has no radio link
     * to sample it from yet — HEARTBEAT.rssi is 0 under the same condition
     * (PROTOCOL.md S4.3), so a 0-valued channel is consistent, not
     * misleading. */
    {ESPS_CH_SYS_RSSI, "sys.rssi", "WiFi RSSI", "dBm", "i8", 1, "system"},
    {ESPS_CH_SYS_UPTIME, "sys.uptime", "Uptime", "s", "u32", 1, "system"},
};

/* Bytes one entry costs inside the serialised `ndb` array, counted rather
 * than guessed:
 *
 *   ,{"id":N,"key":"K","name":"N","unit":"U","type":"T","rate_hz":R,"group":"G"}
 *
 * The fixed punctuation above — including the separating comma — is 69
 * characters. `id` is at most 3 digits and `rate_hz` at most 5, and both are
 * charged at their maximum, so the figure can only ever be an over-estimate.
 * An over-estimate costs at worst one extra HELLO frame; an under-estimate
 * costs the whole announcement. */
#define ESPS_NDB_ENTRY_FIXED_BYTES (69u + 3u + 5u)

static size_t ndb_entry_json_bytes(const ndb_entry_t *e) {
    return ESPS_NDB_ENTRY_FIXED_BYTES + strlen(e->key) + strlen(e->name) + strlen(e->unit) +
           strlen(e->type) + strlen(e->group);
}

/* Every channel this node can declare has to fit, and the place to find out
 * is the compiler, not a node in the field that quietly stops advertising
 * half of them. */
#define ESPS_SYS_NDB_COUNT (sizeof(g_sys_ndb) / sizeof(g_sys_ndb[0]))
#if ESPS_DIO_ENABLED
#define ESPS_DIO_NDB_N ESPS_DIO_NDB_COUNT
#else
#define ESPS_DIO_NDB_N 0u
#endif
#if ESPS_MORSE_ENABLED
#define ESPS_MORSE_NDB_N ESPS_MORSE_NDB_COUNT
#else
#define ESPS_MORSE_NDB_N 0u
#endif
_Static_assert(ESPS_SYS_NDB_COUNT + ESPS_DIO_NDB_N + ESPS_MORSE_NDB_N <= ESPS_NDB_MAX,
               "ESPS_NDB_MAX is too small for the system channels plus the bench "
               "link's — raise it, do not let collect_ndb() truncate");

/* Fills `out` with every channel this node declares, system first. Returns how
 * many were written.
 *
 * The `n < cap` guards are the last line of defence, not the design: the
 * _Static_assert above means they cannot fire for this firmware's own
 * channels. They still shout if they ever do, because a truncated NDB is the
 * kind of failure that looks like a working node with missing channels. */
static size_t collect_ndb(ndb_entry_t *out, size_t cap) {
    size_t n = 0;
    size_t dropped = 0;

    for (size_t i = 0; i < ESPS_SYS_NDB_COUNT; i++) {
        if (n < cap) {
            out[n++] = g_sys_ndb[i];
        } else {
            dropped++;
        }
    }
#if ESPS_DIO_ENABLED
    size_t dio_count = 0;
    const esps_dio_ndb_entry_t *dio = esps_dio_ndb(&dio_count);
    for (size_t i = 0; i < dio_count; i++) {
        if (n >= cap) {
            dropped++;
            continue;
        }
        out[n].id = dio[i].id;
        out[n].key = dio[i].key;
        out[n].name = dio[i].name;
        out[n].unit = dio[i].unit;
        out[n].type = dio[i].type;
        out[n].rate_hz = dio[i].rate_hz;
        out[n].group = dio[i].group;
        n++;
    }
#endif
#if ESPS_MORSE_ENABLED
    size_t morse_count = 0;
    const esps_morse_ndb_entry_t *morse = esps_morse_ndb(&morse_count);
    for (size_t i = 0; i < morse_count; i++) {
        if (n >= cap) {
            dropped++;
            continue;
        }
        out[n].id = morse[i].id;
        out[n].key = morse[i].key;
        out[n].name = morse[i].name;
        out[n].unit = morse[i].unit;
        out[n].type = morse[i].type;
        out[n].rate_hz = morse[i].rate_hz;
        out[n].group = morse[i].group;
        n++;
    }
#endif

    if (dropped > 0) {
        ESP_LOGE(TAG, "NDB truncated: %u channel(s) dropped, only %u of %u fit — raise "
                      "ESPS_NDB_MAX (the station will never see them)",
                 (unsigned)dropped, (unsigned)n, (unsigned)(n + dropped));
    }
    return n;
}

/* Everything a HELLO carries except `ndb`. Built fresh per chunk, because
 * every chunk is a complete, independently valid HELLO — the station merges
 * them by node, and PROTOCOL.md S4.1 already says re-sending HELLO extends
 * the NDB rather than replacing it. */
static cJSON *hello_envelope(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint8_t mac[6];
    esps_node_id_get_mac(mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);

    char label[ESPS_NODE_LABEL_MAX];
    esps_node_id_get_label(label, sizeof(label));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddNumberToObject(root, "node_id", esps_node_id_get());
    cJSON_AddStringToObject(root, "label", label);

    cJSON *chip_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(chip_obj, "model", chip_model_str(chip.model));
    cJSON_AddNumberToObject(chip_obj, "revision", chip.revision);
    cJSON_AddNumberToObject(chip_obj, "cores", chip.cores);
    cJSON *features = cJSON_CreateArray();
    if (chip.features & CHIP_FEATURE_WIFI_BGN) {
        cJSON_AddItemToArray(features, cJSON_CreateString("wifi"));
    }
    if (chip.features & CHIP_FEATURE_BT) {
        cJSON_AddItemToArray(features, cJSON_CreateString("bt"));
    }
    if (chip.features & CHIP_FEATURE_BLE) {
        cJSON_AddItemToArray(features, cJSON_CreateString("ble"));
    }
    cJSON_AddItemToObject(chip_obj, "features", features);
    cJSON_AddItemToObject(root, "chip", chip_obj);

    cJSON *fw = cJSON_CreateObject();
    cJSON_AddStringToObject(fw, "version", ESPS_FW_VERSION);
    /* No wall-clock time is known this early (TIME_SYNC hasn't happened
     * yet); the compile timestamp is a stand-in, and HELLO_ACK.host_time is
     * the actual source of truth for wall time going forward. */
    cJSON_AddStringToObject(fw, "build", __DATE__ " " __TIME__);
    char idf_ver[16];
    snprintf(idf_ver, sizeof(idf_ver), "%d.%d.%d", ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR,
             ESP_IDF_VERSION_PATCH);
    cJSON_AddStringToObject(fw, "idf", idf_ver);
    cJSON_AddStringToObject(fw, "target", CONFIG_IDF_TARGET);
    cJSON_AddItemToObject(root, "fw", fw);

    /* Honest capability list: only what this firmware actually implements.
     * The digital link adds no capability here on purpose — it is expressed
     * entirely as NDB channels and EVENTs (D-7), which is the point of the
     * NDB. Declaring "experiment" would be a lie until S3. */
    cJSON *caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("telemetry"));
    cJSON_AddItemToObject(root, "caps", caps);

    cJSON *boot = cJSON_CreateObject();
    cJSON_AddNumberToObject(boot, "count", esps_node_id_get_boot_count());
    cJSON_AddStringToObject(boot, "reason", reset_reason_str());
    cJSON_AddNumberToObject(boot, "uptime_ms", esps_time_now_ms());
    cJSON_AddItemToObject(root, "boot", boot);

    return root;
}

/* Returns true only when the chunk actually reached the link — that is what
 * the round's ACK expectation is counted against, so a chunk the transport
 * refused must not inflate it. */
static bool send_hello_chunk(const ndb_entry_t *entries, size_t count, unsigned index,
                             unsigned total) {
    cJSON *root = hello_envelope();
    if (!root) {
        ESP_LOGE(TAG, "HELLO %u/%u: out of memory building the envelope", index, total);
        return false;
    }
    cJSON *ndb = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddNumberToObject(ch, "id", entries[i].id);
        cJSON_AddStringToObject(ch, "key", entries[i].key);
        cJSON_AddStringToObject(ch, "name", entries[i].name);
        cJSON_AddStringToObject(ch, "unit", entries[i].unit);
        cJSON_AddStringToObject(ch, "type", entries[i].type);
        cJSON_AddNumberToObject(ch, "rate_hz", entries[i].rate_hz);
        cJSON_AddStringToObject(ch, "group", entries[i].group);
        cJSON_AddItemToArray(ndb, ch);
    }
    cJSON_AddItemToObject(root, "ndb", ndb);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        ESP_LOGE(TAG, "HELLO %u/%u: out of memory serialising", index, total);
        return false;
    }
    const size_t len = strlen(json);

    bool ok = false;
    if (len > ESPS_FRAME_PAYLOAD_MAX) {
        /* The chunker's estimate was wrong. Never silent: this is the failure
         * that makes a node invisible to the station. */
        ESP_LOGE(TAG, "HELLO %u/%u is %u B, over the %u B ceiling — NOT SENT (%u channels)",
                 index, total, (unsigned)len, (unsigned)ESPS_FRAME_PAYLOAD_MAX, (unsigned)count);
    } else if (!send_raw_frame(ESPS_MSG_HELLO, (const uint8_t *)json, len)) {
        ESP_LOGW(TAG, "HELLO %u/%u (%u B, %u channels) not sent: link queue full", index, total,
                 (unsigned)len, (unsigned)count);
    } else {
        ESP_LOGI(TAG, "HELLO %u/%u sent: %u B, %u channels", index, total, (unsigned)len,
                 (unsigned)count);
        ok = true;
    }
    cJSON_free(json);
    return ok;
}

/* Announces the node, splitting the NDB across as many complete HELLO frames
 * as it takes.
 *
 * A node with the digital link enabled declares 9 channels, which serialise to
 * ~1200 B — past PROTOCOL.md's 1024 B payload ceiling and well past what the
 * UART transport's buffer can carry. The protocol already has the answer
 * (S4.1: re-sending HELLO extends the NDB), so each chunk is a full HELLO
 * with the same mac/node_id/chip/fw/caps/boot and a slice of `ndb`, and the
 * station merges them. Every retry re-sends every chunk, because the station
 * may have missed any one of them.
 *
 * With the link disabled the 3 system channels fit comfortably and this emits
 * exactly one HELLO, identical to what it always sent — and one ACK then
 * completes the round, exactly as before.
 *
 * Returns how many chunks actually reached the link, which is how many
 * accepted HELLO_ACKs the round must collect before the node stops
 * re-announcing. */
static unsigned send_hello(void) {
    ndb_entry_t ndb[ESPS_NDB_MAX];
    const size_t ndb_count = collect_ndb(ndb, ESPS_NDB_MAX);

    /* The envelope is measured, not estimated: `label` is operator-settable
     * up to ESPS_NODE_LABEL_MAX and the build/idf strings move with the
     * toolchain, so a hard-coded figure would rot silently and the symptom
     * would be a HELLO that stops arriving. One extra serialise per
     * announcement round (at most once every 30 s) is a fair price. */
    size_t envelope_bytes = 0;
    {
        cJSON *probe = hello_envelope();
        if (!probe) {
            ESP_LOGE(TAG, "HELLO: out of memory");
            return 0;
        }
        cJSON_AddItemToObject(probe, "ndb", cJSON_CreateArray());
        char *s = cJSON_PrintUnformatted(probe);
        if (s) {
            envelope_bytes = strlen(s);
            cJSON_free(s);
        }
        cJSON_Delete(probe);
    }
    if (envelope_bytes == 0 || envelope_bytes + ESPS_HELLO_NDB_MARGIN >= ESPS_FRAME_PAYLOAD_MAX) {
        ESP_LOGE(TAG, "HELLO envelope alone is %u B against a %u B ceiling — cannot announce",
                 (unsigned)envelope_bytes, (unsigned)ESPS_FRAME_PAYLOAD_MAX);
        return 0;
    }
    const size_t ndb_budget = ESPS_FRAME_PAYLOAD_MAX - ESPS_HELLO_NDB_MARGIN - envelope_bytes;

    /* Two passes so each frame can be labelled i/total, which makes a missing
     * chunk obvious in a log instead of something you have to infer. */
    size_t starts[ESPS_HELLO_CHUNK_MAX];
    size_t counts[ESPS_HELLO_CHUNK_MAX];
    unsigned chunks = 0;
    size_t i = 0;
    while (i < ndb_count && chunks < ESPS_HELLO_CHUNK_MAX) {
        size_t used = 0;
        size_t n = 0;
        while (i + n < ndb_count) {
            const size_t cost = ndb_entry_json_bytes(&ndb[i + n]);
            if (used + cost > ndb_budget) {
                break;
            }
            used += cost;
            n++;
        }
        if (n == 0) {
            /* This one entry cannot fit even an otherwise empty chunk. That
             * is permanent, not backpressure, so skip it loudly rather than
             * looping on it forever. */
            ESP_LOGE(TAG, "NDB channel %u (%s) needs %u B against a %u B budget — dropped",
                     (unsigned)ndb[i].id, ndb[i].key, (unsigned)ndb_entry_json_bytes(&ndb[i]),
                     (unsigned)ndb_budget);
            i++;
            continue;
        }
        starts[chunks] = i;
        counts[chunks] = n;
        chunks++;
        i += n;
    }
    if (i < ndb_count) {
        ESP_LOGE(TAG, "NDB needs more than %u HELLO frames; %u channel(s) not announced",
                 (unsigned)ESPS_HELLO_CHUNK_MAX, (unsigned)(ndb_count - i));
    }

    /* The ACK counter is reset here, before a single chunk goes out, so that
     * an ACK arriving while the rest of the round is still being sent counts
     * towards this round rather than being dropped. */
    g_hello_acks = 0;

    if (chunks == 0) {
        /* A node with no channels still has to announce itself. */
        return send_hello_chunk(NULL, 0, 1, 1) ? 1u : 0u;
    }
    unsigned sent = 0;
    for (unsigned c = 0; c < chunks; c++) {
        if (send_hello_chunk(&ndb[starts[c]], counts[c], c + 1, chunks)) {
            sent++;
        }
    }
    return sent;
}

static void hello_task(void *arg) {
    (void)arg;
    for (;;) {
        /* Every round re-sends every chunk. The station may have missed any
         * one of them, and a HELLO is idempotent (PROTOCOL.md S4.1: re-sending
         * it extends the NDB), so replaying the whole round is both correct
         * and simpler than tracking which chunk went missing. */
        const unsigned sent = send_hello();

        bool complete = false;
        for (int waited_ms = 0; waited_ms < ESPS_HELLO_RETRY_MS; waited_ms += 500) {
            if (sent > 0 && g_hello_acks >= sent) {
                complete = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        /* One last look, so an ACK that lands during the final sleep is not
         * wasted into another full retry cycle. */
        if (sent > 0 && g_hello_acks >= sent) {
            complete = true;
        }

        ESP_LOGI(TAG, "HELLO round: %u chunks sent, %u acked%s", sent, (unsigned)g_hello_acks,
                 complete ? "" : " — retrying");
        if (complete) {
            break;
        }
    }
    vTaskDelete(NULL);
}

/* --- HEARTBEAT / TELEMETRY tasks --------------------------------------------- */

static void heartbeat_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* PROTOCOL.md timing.heartbeat_hz: 1 */
        esps_heartbeat_t hb;
        esps_health_build_heartbeat(&hb);
        uint8_t payload[ESPS_HEARTBEAT_SIZE];
        size_t len;
        if (esps_enlp_pack_heartbeat(&hb, payload, sizeof(payload), &len)) {
            send_raw_frame(ESPS_MSG_HEARTBEAT, payload, len);
        }
    }
}

/* Telemetry payload sizing, derived from the builder's own constants rather
 * than from a round number that happened to be big enough. A sample costs
 * ESPS_TELEMETRY_SAMPLE_HEADER_SIZE (ch + dt_ms) + 1 encoding byte + up to 4
 * value bytes, so the worst case is 8 B each.
 *
 * The previous 64-byte buffer held exactly the two system samples with 42 B
 * to spare — which is precisely the six the link adds. It would have started
 * silently dropping channels the moment the last one no longer fit, because
 * esps_telemetry_builder_add() reports a full buffer by returning false and
 * the old code ignored it. */
/* Derived, not a constant, so adding a bench variant cannot quietly start
 * dropping channels again. The base firmware samples two system channels
 * (heap_free and uptime; sys.rssi is declared but has no radio to read yet),
 * and each variant samples at most one per NDB channel it declares.
 *
 * This was a fixed 8 and the Morse variant's eight channels overflowed it on
 * the first boot: `2 telemetry sample(s) did not fit in 70 B`. The builder
 * reports a full buffer honestly, so nothing was silently lost -- but two
 * channels were missing from every batch, which on a chart looks exactly
 * like a node that is not sampling them. */
#define ESPS_TELEM_SYS_SAMPLES 2u
#define ESPS_TELEM_SAMPLES_MAX (ESPS_TELEM_SYS_SAMPLES + ESPS_DIO_NDB_N + ESPS_MORSE_NDB_N)
#define ESPS_TELEM_PAYLOAD_CAP                                                               \
    (ESPS_TELEMETRY_HEADER_SIZE +                                                            \
     ESPS_TELEM_SAMPLES_MAX * (ESPS_TELEMETRY_SAMPLE_HEADER_SIZE + 1u + 4u))

_Static_assert(ESPS_TELEM_SAMPLES_MAX <= ESPS_TELEMETRY_MAX_COUNT,
               "a TELEMETRY batch carries at most ESPS_TELEMETRY_MAX_COUNT samples");
_Static_assert(ESPS_TELEM_PAYLOAD_CAP <= ESPS_FRAME_PAYLOAD_MAX,
               "TELEMETRY payload must fit one frame on this transport");

static void telemetry_task(void *arg) {
    (void)arg;
    static bool overflow_logged = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t heap_free = esp_get_free_heap_size();
        uint32_t uptime_s = esps_time_now_ms() / 1000u;

        uint8_t payload[ESPS_TELEM_PAYLOAD_CAP];
        esps_telemetry_builder_t b;
        esps_telemetry_builder_init(&b, payload, sizeof(payload));

        unsigned rejected = 0;
        rejected += !esps_telemetry_builder_add(&b, ESPS_CH_SYS_HEAP_FREE, 0, ESPS_ENC_U32,
                                                &heap_free);
        rejected += !esps_telemetry_builder_add(&b, ESPS_CH_SYS_UPTIME, 0, ESPS_ENC_U32,
                                                &uptime_s);
        /* sys.rssi omitted: no radio link exists this sprint to sample it
         * from (the field is declared in the NDB regardless, see g_sys_ndb). */

#if ESPS_DIO_ENABLED
        esps_dio_sample_t dio;
        esps_dio_get_sample(&dio);
        rejected += !esps_telemetry_builder_add(&b, ESPS_DIO_CH_TX, 0, ESPS_ENC_U8,
                                                &dio.tx_level);
        rejected += !esps_telemetry_builder_add(&b, ESPS_DIO_CH_RX, 0, ESPS_ENC_U8,
                                                &dio.rx_level);
        /* SPEC-LINK.md: link.rtt_us is NOT published when the handshake timed
         * out, and role B never measures one at all. Skipping the sample is
         * the honest encoding of "no measurement"; sending 0 or the previous
         * value would both plot as a real round trip. */
        if (dio.rtt_valid) {
            rejected += !esps_telemetry_builder_add(&b, ESPS_DIO_CH_RTT_US, 0, ESPS_ENC_U32,
                                                    &dio.rtt_us);
        }
        rejected += !esps_telemetry_builder_add(&b, ESPS_DIO_CH_FRAMES_OK, 0, ESPS_ENC_U32,
                                                &dio.frames_ok);
        rejected += !esps_telemetry_builder_add(&b, ESPS_DIO_CH_FRAMES_ERR, 0, ESPS_ENC_U32,
                                                &dio.frames_err);
        rejected += !esps_telemetry_builder_add(&b, ESPS_DIO_CH_BER, 0, ESPS_ENC_F32, &dio.ber);
#endif
#if ESPS_MORSE_ENABLED
        esps_morse_sample_t morse;
        esps_morse_get_sample(&morse);
        rejected += !esps_telemetry_builder_add(&b, ESPS_MORSE_CH_TX, 0, ESPS_ENC_U8,
                                                &morse.tx_level);
        rejected += !esps_telemetry_builder_add(&b, ESPS_MORSE_CH_RX, 0, ESPS_ENC_U8,
                                                &morse.rx_level);
        rejected += !esps_telemetry_builder_add(&b, ESPS_MORSE_CH_PULSE_MS, 0, ESPS_ENC_U32,
                                                &morse.pulse_ms);
        rejected += !esps_telemetry_builder_add(&b, ESPS_MORSE_CH_GAP_MS, 0, ESPS_ENC_U32,
                                                &morse.gap_ms);
        rejected += !esps_telemetry_builder_add(&b, ESPS_MORSE_CH_SYMBOLS, 0, ESPS_ENC_U32,
                                                &morse.symbols);
        rejected += !esps_telemetry_builder_add(&b, ESPS_MORSE_CH_LETTERS, 0, ESPS_ENC_U32,
                                                &morse.letters);
        rejected += !esps_telemetry_builder_add(&b, ESPS_MORSE_CH_UNKNOWN, 0, ESPS_ENC_U32,
                                                &morse.unknown);
        rejected += !esps_telemetry_builder_add(&b, ESPS_MORSE_CH_BOUNCES, 0, ESPS_ENC_U32,
                                                &morse.bounces);
#endif

        if (rejected > 0 && !overflow_logged) {
            overflow_logged = true;
            ESP_LOGE(TAG, "%u telemetry sample(s) did not fit in %u B — channels are being "
                          "dropped; raise ESPS_TELEM_SAMPLES_MAX",
                     rejected, (unsigned)ESPS_TELEM_PAYLOAD_CAP);
        }

        size_t len;
        if (esps_telemetry_builder_finish(&b, esps_time_now_ms(), 0, &len)) {
            send_raw_frame(ESPS_MSG_TELEMETRY, payload, len);
        }
    }
}

/* --- EVENT (0x21) --------------------------------------------------------------- */

#if ESPS_DIO_ENABLED
static const char *dio_severity_str(esps_dio_severity_t sev) {
    switch (sev) {
        case ESPS_DIO_SEV_DEBUG:
            return "debug";
        case ESPS_DIO_SEV_WARNING:
            return "warning";
        case ESPS_DIO_SEV_ERROR:
            return "error";
        case ESPS_DIO_SEV_INFO:
        default:
            return "info";
    }
}

/* esps_dio hands over a code, a severity and a couple of named numbers; the
 * JSON shape of PROTOCOL.md S4.6 is assembled here. The component stays
 * ignorant of cJSON and of the link, which is what lets the same link logic
 * be driven by the host tests and by the simulator.
 *
 * Runs on the link's phase task, never in an ISR — cJSON allocates. */
static void dio_event_sink(const esps_dio_event_t *ev, void *ctx) {
    (void)ctx;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    /* D-10: node monotonic ms, converted to epoch seconds exactly once, by
     * the gateway. Nothing on the node does that conversion. */
    cJSON_AddNumberToObject(root, "ts_ms", esps_time_now_ms());
    cJSON_AddStringToObject(root, "code", ev->code);
    cJSON_AddStringToObject(root, "severity", dio_severity_str(ev->severity));

    cJSON *data = cJSON_CreateObject();
    if (ev->a_key) {
        cJSON_AddNumberToObject(data, ev->a_key, ev->a_val);
    }
    if (ev->b_key) {
        cJSON_AddNumberToObject(data, ev->b_key, ev->b_val);
    }
    if (ev->reason) {
        cJSON_AddStringToObject(data, "reason", ev->reason);
    }
    if (ev->suppressed > 0) {
        cJSON_AddNumberToObject(data, "suppressed", ev->suppressed);
    }
    cJSON_AddItemToObject(root, "data", data);

    send_json_frame(ESPS_MSG_EVENT, root);
    cJSON_Delete(root);
}
#endif /* ESPS_DIO_ENABLED */

#if ESPS_MORSE_ENABLED
static const char *morse_severity_str(esps_morse_severity_t sev) {
    switch (sev) {
        case ESPS_MORSE_SEV_DEBUG:
            return "debug";
        case ESPS_MORSE_SEV_WARNING:
            return "warning";
        case ESPS_MORSE_SEV_ERROR:
            return "error";
        case ESPS_MORSE_SEV_INFO:
        default:
            return "info";
    }
}

/* The Morse transceiver hands over a code, a severity, a direction and at
 * most one character; the JSON shape of PROTOCOL.md S4.6 is assembled here.
 * The `dir` field is what makes a duplex event readable: the same code means
 * different things depending on whose hand produced it.
 *
 * The field names match what the gateway's adapter emits for the Arduino
 * sketch (transports/morse_sketch.py), so the desktop's Morse section reads
 * a real node and an adapted one with the same code.
 *
 * Runs on the Morse task, never in an ISR — cJSON allocates. */
static void morse_event_sink(const esps_morse_station_event_t *ev, void *ctx) {
    (void)ctx;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddNumberToObject(root, "ts_ms", esps_time_now_ms());
    cJSON_AddStringToObject(root, "code", ev->code);
    cJSON_AddStringToObject(root, "severity", morse_severity_str(ev->severity));

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "dir", ev->dir);
    if (ev->symbol != '\0') {
        const char sym[2] = {ev->symbol, '\0'};
        cJSON_AddStringToObject(data, "symbol", sym);
    }
    if (ev->letter != '\0') {
        const char let[2] = {ev->letter, '\0'};
        cJSON_AddStringToObject(data, "letter", let);
        cJSON_AddNumberToObject(data, "byte", (unsigned char)ev->letter);
    }
    if (ev->text != NULL) {
        cJSON_AddStringToObject(data, "code", ev->text);
    }
    if (ev->ms > 0) {
        cJSON_AddNumberToObject(data, "ms", ev->ms);
    }
    if (ev->suppressed > 0) {
        cJSON_AddNumberToObject(data, "suppressed", ev->suppressed);
    }
    cJSON_AddItemToObject(root, "data", data);

    send_json_frame(ESPS_MSG_EVENT, root);
    cJSON_Delete(root);
}
#endif /* ESPS_MORSE_ENABLED */

/* --- CMD dispatcher ------------------------------------------------------------ */

static void reboot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the TX task actually drain the CMD_ACK first */
    esp_restart();
}

static void send_cmd_ack_ok(int cmd_id, cJSON *data /* NULL or a value this call takes ownership of */) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", cmd_id);
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data ? data : cJSON_CreateObject());
    send_json_frame(ESPS_MSG_CMD_ACK, root);
    cJSON_Delete(root);
}

static void send_cmd_ack_err(int cmd_id, const char *code, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", cmd_id);
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON *err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(root, "err", err);
    send_json_frame(ESPS_MSG_CMD_ACK, root);
    cJSON_Delete(root);
}

static esp_log_level_t parse_log_level(const char *s) {
    if (!strcmp(s, "none")) return ESP_LOG_NONE;
    if (!strcmp(s, "error")) return ESP_LOG_ERROR;
    if (!strcmp(s, "warn")) return ESP_LOG_WARN;
    if (!strcmp(s, "info")) return ESP_LOG_INFO;
    if (!strcmp(s, "debug")) return ESP_LOG_DEBUG;
    if (!strcmp(s, "verbose")) return ESP_LOG_VERBOSE;
    return (esp_log_level_t)-1;
}

static void handle_cmd(const uint8_t *payload, size_t len) {
    cJSON *root = cJSON_ParseWithLength((const char *)payload, len);
    if (!root) {
        return; /* not valid JSON — no `id` to ack against, nothing safe to send back */
    }

    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *op_item = cJSON_GetObjectItemCaseSensitive(root, "op");
    cJSON *args_item = cJSON_GetObjectItemCaseSensitive(root, "args");
    int cmd_id = cJSON_IsNumber(id_item) ? id_item->valueint : 0;
    const char *op = cJSON_IsString(op_item) ? op_item->valuestring : "";

    if (strcmp(op, "node.ping") == 0) {
        send_cmd_ack_ok(cmd_id, NULL);

    } else if (strcmp(op, "node.info") == 0) {
        char label[ESPS_NODE_LABEL_MAX];
        esps_node_id_get_label(label, sizeof(label));
        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "node_id", esps_node_id_get());
        cJSON_AddStringToObject(data, "label", label);
        cJSON_AddNumberToObject(data, "boot_count", esps_node_id_get_boot_count());
        cJSON_AddNumberToObject(data, "uptime_ms", esps_time_now_ms());
        cJSON_AddNumberToObject(data, "heap_free", esp_get_free_heap_size());
        cJSON_AddStringToObject(data, "fw_version", ESPS_FW_VERSION);
        cJSON_AddNumberToObject(data, "state", (int)esps_health_get_state());
        send_cmd_ack_ok(cmd_id, data);

    } else if (strcmp(op, "node.reboot") == 0) {
        send_cmd_ack_ok(cmd_id, NULL);
        xTaskCreate(reboot_task, "esps_reboot", 2048, NULL, 5, NULL);

    } else if (strcmp(op, "node.set_label") == 0) {
        cJSON *label_item = args_item ? cJSON_GetObjectItemCaseSensitive(args_item, "label") : NULL;
        if (!cJSON_IsString(label_item)) {
            send_cmd_ack_err(cmd_id, "invalid_args", "args.label (string) is required");
        } else if (!esps_node_id_set_label(label_item->valuestring)) {
            send_cmd_ack_err(cmd_id, "failed", "label rejected (empty or too long)");
        } else {
            send_cmd_ack_ok(cmd_id, NULL);
        }

    } else if (strcmp(op, "node.set_log_level") == 0) {
        cJSON *level_item = args_item ? cJSON_GetObjectItemCaseSensitive(args_item, "level") : NULL;
        esp_log_level_t level =
            cJSON_IsString(level_item) ? parse_log_level(level_item->valuestring) : (esp_log_level_t)-1;
        if ((int)level < 0) {
            send_cmd_ack_err(cmd_id, "invalid_args",
                              "args.level must be one of none|error|warn|info|debug|verbose");
        } else {
            esp_log_level_set("*", level);
            send_cmd_ack_ok(cmd_id, NULL);
        }

    } else {
        send_cmd_ack_err(cmd_id, "unsupported", "op not implemented by this firmware");
    }

    cJSON_Delete(root);
}

/* --- link callbacks -------------------------------------------------------- */

static void on_frame(const esps_enlp_frame_t *frame, void *ctx) {
    (void)ctx;
    switch (frame->type) {
        case ESPS_MSG_HELLO_ACK: {
            cJSON *root = cJSON_ParseWithLength((const char *)frame->payload, frame->payload_len);
            if (root) {
                cJSON *accepted = cJSON_GetObjectItemCaseSensitive(root, "accepted");
                /* Absent `accepted` is treated as accepted (the example in
                 * PROTOCOL.md S4.2 always includes it, but nothing requires
                 * the field when true, only when false + reason). */
                if (!cJSON_IsBool(accepted) || cJSON_IsTrue(accepted)) {
                    /* Counted, not latched: the station sends one ACK per
                     * HELLO, so with a chunked NDB the round is only done when
                     * as many have come back as chunks went out. A rejected
                     * ACK deliberately does not count — the node keeps
                     * announcing rather than going quiet on a refusal. */
                    g_hello_acks++;
                }
                cJSON_Delete(root);
            }
            break;
        }
        case ESPS_MSG_CMD:
            handle_cmd(frame->payload, frame->payload_len);
            break;
        case ESPS_MSG_TIME_SYNC: {
            uint32_t t2 = esps_time_now_ms(); /* captured immediately on receipt, per PROTOCOL.md S4.11 */
            esps_time_sync_t req;
            if (esps_enlp_unpack_time_sync(frame->payload, frame->payload_len, &req)) {
                uint8_t reply[ESPS_TIME_SYNC_SIZE];
                size_t reply_len;
                if (esps_time_build_reply(req.t1_host_us, t2, reply, sizeof(reply), &reply_len)) {
                    send_raw_frame(ESPS_MSG_TIME_SYNC, reply, reply_len);
                }
            }
            break;
        }
        default:
            /* EVENT/EXP_SET/TELEM_ACK/NET_CMD/BULK_* land with esps_experiment,
             * esps_store and esps_net in later sprints — not silently mis-handled,
             * just not built yet. */
            break;
    }
}

static void on_raw(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    (void)data;
    ESP_LOGW(TAG, "undecodable bytes on link (%u B) — boot text or serial noise", (unsigned)len);
}

/* --- app_main ---------------------------------------------------------------- */

void app_main(void) {
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    esps_health_init();
    esps_node_id_init();

#if ESPS_DIO_ENABLED
    /* D-1, and the ordering is the whole point of it: the experiment starts
     * BEFORE the station link is opened, not after.
     *
     * The loop below retries g_link.open() every 5 s forever. If the transport
     * never comes up — no USB host, a held-down UART, a driver failure — then
     * anything started after it never starts at all. A node whose experiment
     * depends on a station being reachable is exactly the design D-1 exists to
     * forbid, so esps_dio_start() goes here, ahead of it.
     *
     * Nothing it needs is initialised later: it uses only the allow-list (pure
     * C), esp_timer, the GPIO driver and FreeRTOS, and it configures its own
     * pins and installs its own interrupts from inside its task. It does not
     * touch NVS, the node identity or the link.
     *
     * The event sink is installed after the link opens, because that is when
     * there is somewhere for an event to go; dio_emit() with a NULL sink is a
     * no-op, so events raised before then are dropped rather than queued. Any
     * that matter (a rejected pin) are also logged, and reach the station as
     * raw console output once it connects (PROTOCOL.md S2.1). */
    if (!esps_dio_start()) {
        ESP_LOGE(TAG, "digital link did not start; the node continues without it");
    }
#endif

#if ESPS_MORSE_ENABLED
    /* Same placement as the digital link, and for the same reason (D-1): two
     * operators must be able to key Morse at each other with no station
     * attached at all. Starting this after the link's retry loop would make
     * the practice depend on a laptop being reachable, which is precisely the
     * design that invariant forbids. */
    if (!esps_morse_start()) {
        ESP_LOGE(TAG, "morse transceiver did not start; the node continues without it");
    }
#endif

    esps_link_uart_config_t uart_cfg = ESPS_LINK_UART_CONFIG_DEFAULT();
    esps_link_uart_init(&g_link, &uart_cfg);

    while (!g_link.open(&g_link, on_frame, on_raw, NULL)) {
        printf("espstation-fw: UART link open failed, retrying in 5s...\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    esps_log_hook_init(link_sink, NULL);
    esps_health_set_state(ESPS_NODE_STATE_IDLE);

    ESP_LOGI(TAG, "espstation-fw %s booting, node_id=%u, boot_count=%u", ESPS_FW_VERSION,
             (unsigned)esps_node_id_get(), (unsigned)esps_node_id_get_boot_count());

#if ESPS_DIO_ENABLED
    /* The link has been running since before the transport opened (see above).
     * All this does is give its events somewhere to go, now that there is a
     * somewhere. */
    esps_dio_set_event_sink(dio_event_sink, NULL);
#endif

#if ESPS_MORSE_ENABLED
    /* The transceiver has been running since before the transport opened.
     * All this does is give its events somewhere to go, now that there is a
     * somewhere. */
    esps_morse_set_event_sink(morse_event_sink, NULL);
#endif

    xTaskCreate(hello_task, "esps_hello", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "esps_heartbeat", 3584, NULL, 5, NULL);
    xTaskCreate(telemetry_task, "esps_telemetry", 3584, NULL, 5, NULL);
}

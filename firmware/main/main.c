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

static esps_link_if_t g_link;
static volatile bool g_hello_acked = false;

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

static cJSON *build_ndb(void) {
    cJSON *ndb = cJSON_CreateArray();

    cJSON *heap = cJSON_CreateObject();
    cJSON_AddNumberToObject(heap, "id", ESPS_CH_SYS_HEAP_FREE);
    cJSON_AddStringToObject(heap, "key", "sys.heap_free");
    cJSON_AddStringToObject(heap, "name", "Heap free");
    cJSON_AddStringToObject(heap, "unit", "B");
    cJSON_AddStringToObject(heap, "type", "u32");
    cJSON_AddNumberToObject(heap, "rate_hz", 1);
    cJSON_AddStringToObject(heap, "group", "system");
    cJSON_AddItemToArray(ndb, heap);

    /* Declared per system_channels even though this sprint has no radio
     * link to sample it from yet — HEARTBEAT.rssi is 0 under the same
     * condition (PROTOCOL.md S4.3), so a 0-valued channel is consistent,
     * not misleading. */
    cJSON *rssi = cJSON_CreateObject();
    cJSON_AddNumberToObject(rssi, "id", ESPS_CH_SYS_RSSI);
    cJSON_AddStringToObject(rssi, "key", "sys.rssi");
    cJSON_AddStringToObject(rssi, "name", "WiFi RSSI");
    cJSON_AddStringToObject(rssi, "unit", "dBm");
    cJSON_AddStringToObject(rssi, "type", "i8");
    cJSON_AddNumberToObject(rssi, "rate_hz", 1);
    cJSON_AddStringToObject(rssi, "group", "system");
    cJSON_AddItemToArray(ndb, rssi);

    cJSON *uptime = cJSON_CreateObject();
    cJSON_AddNumberToObject(uptime, "id", ESPS_CH_SYS_UPTIME);
    cJSON_AddStringToObject(uptime, "key", "sys.uptime");
    cJSON_AddStringToObject(uptime, "name", "Uptime");
    cJSON_AddStringToObject(uptime, "unit", "s");
    cJSON_AddStringToObject(uptime, "type", "u32");
    cJSON_AddNumberToObject(uptime, "rate_hz", 1);
    cJSON_AddStringToObject(uptime, "group", "system");
    cJSON_AddItemToArray(ndb, uptime);

    return ndb;
}

static void send_hello(void) {
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

    /* Honest capability list: only what this sprint actually implements.
     * experiment/espnow/store_forward/ota are later sprints, not yet true. */
    cJSON *caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("telemetry"));
    cJSON_AddItemToObject(root, "caps", caps);

    cJSON *boot = cJSON_CreateObject();
    cJSON_AddNumberToObject(boot, "count", esps_node_id_get_boot_count());
    cJSON_AddStringToObject(boot, "reason", reset_reason_str());
    cJSON_AddNumberToObject(boot, "uptime_ms", esps_time_now_ms());
    cJSON_AddItemToObject(root, "boot", boot);

    cJSON_AddItemToObject(root, "ndb", build_ndb());

    if (!send_json_frame(ESPS_MSG_HELLO, root)) {
        ESP_LOGW(TAG, "HELLO send failed (link queue full?)");
    }
    cJSON_Delete(root);
}

static void hello_task(void *arg) {
    (void)arg;
    while (!g_hello_acked) {
        send_hello();
        for (int waited_ms = 0; waited_ms < ESPS_HELLO_RETRY_MS && !g_hello_acked;
             waited_ms += 500) {
            vTaskDelay(pdMS_TO_TICKS(500));
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

static void telemetry_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t heap_free = esp_get_free_heap_size();
        uint32_t uptime_s = esps_time_now_ms() / 1000u;

        uint8_t payload[64];
        esps_telemetry_builder_t b;
        esps_telemetry_builder_init(&b, payload, sizeof(payload));
        esps_telemetry_builder_add(&b, ESPS_CH_SYS_HEAP_FREE, 0, ESPS_ENC_U32, &heap_free);
        esps_telemetry_builder_add(&b, ESPS_CH_SYS_UPTIME, 0, ESPS_ENC_U32, &uptime_s);
        /* sys.rssi omitted: no radio link exists this sprint to sample it
         * from (the field is declared in the NDB regardless, see build_ndb). */

        size_t len;
        if (esps_telemetry_builder_finish(&b, esps_time_now_ms(), 0, &len)) {
            send_raw_frame(ESPS_MSG_TELEMETRY, payload, len);
        }
    }
}

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
                    g_hello_acked = true;
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

    xTaskCreate(hello_task, "esps_hello", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "esps_heartbeat", 3584, NULL, 5, NULL);
    xTaskCreate(telemetry_task, "esps_telemetry", 3584, NULL, 5, NULL);
}

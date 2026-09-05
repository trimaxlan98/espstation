/* See esps_health.h. */
#include "esps_health.h"

#include "esp_system.h"
#include "esp_timer.h"

static esps_node_state_t s_state = ESPS_NODE_STATE_BOOT;
static uint8_t s_latched_flags = 0;
static volatile bool s_link_lost = false;
static volatile bool s_buffered_pending = false;
static volatile int8_t s_rssi = 0;

void esps_health_init(void) {
    s_state = ESPS_NODE_STATE_BOOT;
    s_latched_flags = 0;

    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_BROWNOUT) {
        s_latched_flags |= ESPS_HEARTBEAT_FLAG_BROWNOUT_SINCE_BOOT;
    }
    if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT) {
        s_latched_flags |= ESPS_HEARTBEAT_FLAG_WATCHDOG_RESET;
    }
}

void esps_health_set_state(esps_node_state_t state) {
    s_state = state;
}

esps_node_state_t esps_health_get_state(void) {
    return s_state;
}

void esps_health_set_link_lost(bool lost) {
    s_link_lost = lost;
}

void esps_health_set_buffered_pending(bool pending) {
    s_buffered_pending = pending;
}

void esps_health_set_rssi(int8_t rssi) {
    s_rssi = rssi;
}

void esps_health_build_heartbeat(esps_heartbeat_t *out) {
    out->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    out->heap_free = esp_get_free_heap_size();
    out->heap_min = esp_get_minimum_free_heap_size();
    out->state = (uint8_t)s_state;

    uint8_t flags = s_latched_flags;
    if (s_buffered_pending) {
        flags |= ESPS_HEARTBEAT_FLAG_BUFFERED_PENDING;
    }
    if (s_link_lost) {
        flags |= ESPS_HEARTBEAT_FLAG_LINK_WAS_LOST;
    }
    out->flags = flags;
    out->rssi = s_rssi;
}

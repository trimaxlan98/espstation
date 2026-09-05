/* See esps_node_id.h. Short id derivation reuses esps_crc16 (the same
 * checksum already linked in for the wire protocol) rather than pulling in
 * a second hash — it is not cryptographic, just needs to spread 6 MAC bytes
 * across 16 bits, and 0 is remapped to 1 because node id 0 means "the
 * station itself" (PROTOCOL.md S3.1).
 */
#include "esps_node_id.h"
#include "esps_crc16.h"

#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

#define ESPS_NVS_NAMESPACE "espstation"
#define ESPS_NVS_KEY_NODE_ID "node_id"
#define ESPS_NVS_KEY_LABEL "label"
#define ESPS_NVS_KEY_BOOT_COUNT "boot_count"

static uint16_t s_node_id = 0;
static uint32_t s_boot_count = 0;
static uint8_t s_mac[6] = {0};
static char s_label[ESPS_NODE_LABEL_MAX] = {0};

static uint16_t derive_from_mac(const uint8_t mac[6]) {
    uint16_t id = esps_crc16(mac, 6);
    if (id == 0) {
        id = 1;
    }
    return id;
}

static void default_label(char *buf, size_t cap, uint16_t node_id) {
    snprintf(buf, cap, "node-%u", (unsigned)node_id);
}

void esps_node_id_init(void) {
    esp_efuse_mac_get_default(s_mac);
    uint16_t derived = derive_from_mac(s_mac);

    nvs_handle_t h;
    if (nvs_open(ESPS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        /* No persistence this session; keep the node usable rather than
         * refusing to boot (docs/EXPERIMENTS.md: run what you can). */
        s_node_id = derived;
        s_boot_count = 1;
        default_label(s_label, sizeof(s_label), s_node_id);
        return;
    }

    uint16_t stored_id = 0;
    if (nvs_get_u16(h, ESPS_NVS_KEY_NODE_ID, &stored_id) == ESP_OK && stored_id != 0) {
        s_node_id = stored_id;
    } else {
        s_node_id = derived;
        nvs_set_u16(h, ESPS_NVS_KEY_NODE_ID, s_node_id);
    }

    size_t label_cap = sizeof(s_label);
    if (nvs_get_str(h, ESPS_NVS_KEY_LABEL, s_label, &label_cap) != ESP_OK || s_label[0] == '\0') {
        default_label(s_label, sizeof(s_label), s_node_id);
        nvs_set_str(h, ESPS_NVS_KEY_LABEL, s_label);
    }

    uint32_t boot_count = 0;
    nvs_get_u32(h, ESPS_NVS_KEY_BOOT_COUNT, &boot_count); /* 0 if key absent (first boot) */
    boot_count++;
    nvs_set_u32(h, ESPS_NVS_KEY_BOOT_COUNT, boot_count);
    s_boot_count = boot_count;

    nvs_commit(h);
    nvs_close(h);
}

uint16_t esps_node_id_get(void) {
    return s_node_id;
}

uint32_t esps_node_id_get_boot_count(void) {
    return s_boot_count;
}

void esps_node_id_get_mac(uint8_t mac_out[6]) {
    memcpy(mac_out, s_mac, 6);
}

void esps_node_id_get_label(char *buf, size_t cap) {
    if (cap == 0) {
        return;
    }
    size_t n = strnlen(s_label, sizeof(s_label));
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(buf, s_label, n);
    buf[n] = '\0';
}

bool esps_node_id_set_label(const char *label) {
    if (!label) {
        return false;
    }
    size_t n = strnlen(label, ESPS_NODE_LABEL_MAX);
    if (n >= ESPS_NODE_LABEL_MAX) {
        return false; /* reject rather than silently truncate an operator's label */
    }

    nvs_handle_t h;
    if (nvs_open(ESPS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, ESPS_NVS_KEY_LABEL, label);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    memcpy(s_label, label, n);
    s_label[n] = '\0';
    return true;
}

bool esps_node_id_set_override(uint16_t node_id) {
    if (node_id == 0) {
        return false; /* 0 is reserved for the station */
    }
    nvs_handle_t h;
    if (nvs_open(ESPS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_u16(h, ESPS_NVS_KEY_NODE_ID, node_id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    s_node_id = node_id;
    return true;
}

/* Node identity: the u16 short id nodes are addressed by on the wire
 * (PROTOCOL.md S3.1), plus the human label and boot counter that ride along
 * in HELLO. Backed by NVS namespace "espstation" so identity survives
 * reflash-without-erase and, more importantly, so an operator's
 * `node.set_id` / `node.set_label` override sticks across reboots.
 */
#ifndef ESPS_NODE_ID_H
#define ESPS_NODE_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPS_NODE_LABEL_MAX 32

/* Call once at boot, after nvs_flash_init(). On first boot, derives the
 * short id from the factory MAC and persists it; on every boot, increments
 * and persists boot_count. Never fails outward — if NVS is unavailable the
 * node still gets a usable (unpersisted) identity for that session rather
 * than refusing to boot, per docs/EXPERIMENTS.md's "run what you can"
 * philosophy. */
void esps_node_id_init(void);

uint16_t esps_node_id_get(void);
uint32_t esps_node_id_get_boot_count(void);

/* Factory MAC (esp_efuse_mac_get_default), 6 bytes, for HELLO.mac. */
void esps_node_id_get_mac(uint8_t mac_out[6]);

/* NUL-terminated, truncated to cap. Defaults to "node-<id>" until set. */
void esps_node_id_get_label(char *buf, size_t cap);

/* CMD node.set_label: persists immediately. */
bool esps_node_id_set_label(const char *label);

/* CMD node.set_id: overrides the derived short id and persists it. Short
 * ids can collide across a fleet (PROTOCOL.md S3.1); resolving a collision
 * is exactly what this exists for. */
bool esps_node_id_set_override(uint16_t node_id);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_NODE_ID_H */

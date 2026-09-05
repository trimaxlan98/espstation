/* CRC-16/CCITT-FALSE, bit-by-bit (no lookup table). This runs once per frame
 * on both the node (per TX) and the host tooling, and the payload is at most
 * 1024 B (240 B on ESP-NOW) — a 256-entry table would save cycles nobody is
 * short on here, at the cost of 512 B of flash/RAM the table would occupy on
 * every ESP32 target. Bit-by-bit keeps this file trivial to eyeball against
 * the spec, which matters more than throughput at this size.
 */
#include "esps_crc16.h"

uint16_t esps_crc16_update(uint16_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

uint16_t esps_crc16(const void *buf, size_t len) {
    return esps_crc16_update(ESPS_CRC16_INIT, buf, len);
}

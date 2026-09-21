/* CRC-8/SMBUS, bit-by-bit (no lookup table). The receiver calls this from an
 * ISR once per completed byte; a table living in flash would be unreadable
 * while the flash cache is disabled, and 8 iterations per byte is negligible
 * at the link's bit rates.
 */
#include "esps_dio_crc8.h"

uint8_t esps_dio_crc8(const uint8_t *data, size_t len, uint8_t init) {
    uint8_t crc = init;
    if (data == NULL) {
        return crc;
    }
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80u) {
                crc = (uint8_t)((crc << 1) ^ 0x07u);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

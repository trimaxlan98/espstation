/* CRC-8/SMBUS vectors from SPEC-LINK.md. The golden values are shared with the
 * Arduino sketches and the gateway simulator; they were also recomputed with
 * an independent Python reference, not derived from this C code. */
#include "esps_dio_crc8.h"
#include "harness.h"

int test_dio_crc8_all(void) {
    int fails = 0;

    ESPS_CHECK_EQ(&fails, esps_dio_crc8((const uint8_t *)"123456789", 9, 0), 0xF4);
    static const uint8_t v0[] = {0x00};
    static const uint8_t v1[] = {0x01, 0x00};
    ESPS_CHECK_EQ(&fails, esps_dio_crc8(v0, 1, 0), 0x00);
    ESPS_CHECK_EQ(&fails, esps_dio_crc8(v1, 2, 0), 0x15);

    /* Empty input returns init unchanged; NULL is treated as empty. */
    ESPS_CHECK_EQ(&fails, esps_dio_crc8(v0, 0, ESPS_DIO_CRC8_INIT), 0x00);
    ESPS_CHECK_EQ(&fails, esps_dio_crc8(v0, 0, 0x5A), 0x5A);
    ESPS_CHECK_EQ(&fails, esps_dio_crc8(NULL, 4, 0x5A), 0x5A);

    /* Chaining must equal the one-shot result however the input is cut. */
    uint8_t crc = ESPS_DIO_CRC8_INIT;
    crc = esps_dio_crc8((const uint8_t *)"1234", 4, crc);
    crc = esps_dio_crc8((const uint8_t *)"56789", 5, crc);
    ESPS_CHECK_EQ(&fails, crc, 0xF4);

    crc = ESPS_DIO_CRC8_INIT;
    const char *s = "123456789";
    for (size_t i = 0; i < 9; i++) {
        crc = esps_dio_crc8((const uint8_t *)&s[i], 1, crc);
    }
    ESPS_CHECK_EQ(&fails, crc, 0xF4);

    return fails;
}

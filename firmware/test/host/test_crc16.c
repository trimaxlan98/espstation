/* CRC-16/CCITT-FALSE vectors. The "123456789" check value (0x29B1) is the
 * standard conformance vector for this variant; the binary vectors were
 * computed independently with a reference Python implementation of the same
 * poly/init/no-reflect parameters (see the task notes — not re-derived from
 * this C code, so a bug shared between the two would still be caught).
 */
#include "esps_crc16.h"
#include "harness.h"

#include <string.h>

int test_crc16_all(void) {
    int fails = 0;

    ESPS_CHECK_EQ(&fails, esps_crc16("", 0), 0xFFFF);
    ESPS_CHECK_EQ(&fails, esps_crc16("123456789", 9), 0x29B1);

    static const unsigned char vec_seq[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                               0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    ESPS_CHECK_EQ(&fails, esps_crc16(vec_seq, sizeof(vec_seq)), 0x3B37);

    static const unsigned char vec2[12] = {0x01, 0x03, 0x12, 0x34, 0x01, 0x00,
                                            0x04, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
    ESPS_CHECK_EQ(&fails, esps_crc16(vec2, sizeof(vec2)), 0x58A2);

    /* Incremental API must equal the one-shot result regardless of how the
     * input is chunked. */
    uint16_t crc = ESPS_CRC16_INIT;
    crc = esps_crc16_update(crc, "1234", 4);
    crc = esps_crc16_update(crc, "56789", 5);
    ESPS_CHECK_EQ(&fails, crc, 0x29B1);

    /* Byte-at-a-time incremental update must also match. */
    crc = ESPS_CRC16_INIT;
    const char *s = "123456789";
    for (size_t i = 0; i < 9; i++) {
        crc = esps_crc16_update(crc, &s[i], 1);
    }
    ESPS_CHECK_EQ(&fails, crc, 0x29B1);

    return fails;
}

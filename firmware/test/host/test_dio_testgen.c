/* Test-payload generator and resync rule. The seq=0/1/2/999 payloads are the
 * SPEC-LINK.md golden vectors (recomputed with an independent Python
 * implementation of the spec's pseudo-code). */
#include "dio_helpers.h"
#include "esps_dio_testgen.h"
#include "harness.h"

#include <string.h>

static void check_seq(int *fails, uint32_t seq, const char *payload_hex) {
    uint8_t want[32];
    size_t want_len = dio_unhex(payload_hex, want, sizeof(want));
    uint8_t got[32];
    memset(got, 0xEE, sizeof(got)); /* poison: the zero tail must be written */
    size_t len = esps_dio_testframe_payload(seq, got);
    ESPS_CHECK_EQ(fails, len, want_len);
    ESPS_CHECK(fails, len == want_len && memcmp(got, want, want_len) == 0);
    for (size_t i = len; i < 32; i++) {
        ESPS_CHECK_EQ(fails, got[i], 0);
    }
}

int test_dio_testgen_all(void) {
    int fails = 0;

    check_seq(&fails, 0, "004a9d11b4cb431957fd2d5e40a7affdd0");
    check_seq(&fails, 1, "012f");
    check_seq(&fails, 2, "022464f86b91dc51d3fac2fb8ca4561b5197dee1");
    check_seq(&fails, 999, "e741");

    /* xorshift32: 0 is a fixed point; 1 -> 270369 (13,17,5 triple). */
    ESPS_CHECK_EQ(&fails, esps_dio_xorshift32(0), 0);
    ESPS_CHECK_EQ(&fails, esps_dio_xorshift32(1), 270369);

    /* Invariants over a wide range: len in 1..32 and byte 0 = seq & 0xFF. */
    for (uint32_t seq = 0; seq < 5000; seq++) {
        uint8_t p[32];
        size_t len = esps_dio_testframe_payload(seq, p);
        ESPS_CHECK(&fails, len >= 1 && len <= 32);
        ESPS_CHECK_EQ(&fails, p[0], seq & 0xFF);
    }
    /* The st == 0 guard: no seq in the 32-bit range must hang or crash; the
     * one that would zero the state is SEED ^ (seq*K) == 0. Solve it with the
     * modular inverse of K instead of brute force. */
    {
        uint32_t k_inv = 1;
        for (int i = 0; i < 5; i++) { /* Newton iteration: each step doubles the correct bits */
            k_inv *= 2u - 2654435761u * k_inv;
        }
        ESPS_CHECK_EQ(&fails, (uint32_t)(2654435761u * k_inv), 1u);
        uint32_t seq0 = (uint32_t)(ESPS_DIO_TEST_SEED * k_inv);
        uint8_t p[32];
        size_t len = esps_dio_testframe_payload(seq0, p);
        ESPS_CHECK(&fails, len >= 1 && len <= 32);
        ESPS_CHECK_EQ(&fails, p[0], seq0 & 0xFF);
    }

    /* NULL out: nothing written, 0 returned. */
    ESPS_CHECK_EQ(&fails, esps_dio_testframe_payload(1, NULL), 0);

    /* Resync: k + ((byte0 - k) & 0xFF). */
    ESPS_CHECK_EQ(&fails, esps_dio_resync_index(0, 0), 0);
    ESPS_CHECK_EQ(&fails, esps_dio_resync_index(5, 5), 5);    /* in step */
    ESPS_CHECK_EQ(&fails, esps_dio_resync_index(5, 9), 9);    /* lost 4 */
    ESPS_CHECK_EQ(&fails, esps_dio_resync_index(5, 4), 260);  /* low byte wrapped: k+255 */
    ESPS_CHECK_EQ(&fails, esps_dio_resync_index(250, 3), 259);
    ESPS_CHECK_EQ(&fails, esps_dio_resync_index(999, 0xE7), 999);
    ESPS_CHECK_EQ(&fails, esps_dio_resync_index(999, 0xE8), 1000);
    ESPS_CHECK_EQ(&fails, esps_dio_resync_index(1000, 0xE7), 1255);
    ESPS_CHECK(&fails, esps_dio_resync_index(0xFFFFFFFFu, 0x01) == 0x00000001u); /* uint32 wrap */

    return fails;
}

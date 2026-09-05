/* COBS round-trip tests, including the block-boundary cases (254/255 bytes)
 * where naive COBS implementations classically break, and rejection of
 * malformed encodings without reading/writing out of bounds (ASan/UBSan
 * catch that class of bug directly — see the Makefile).
 */
#include "esps_cobs.h"
#include "harness.h"

#include <string.h>

static void roundtrip(int *fails, const uint8_t *src, size_t src_len) {
    uint8_t enc[ESPS_COBS_MAX_ENCODED(600)];
    size_t enc_len = 0;
    esps_cobs_err_t rc = esps_cobs_encode(src, src_len, enc, sizeof(enc), &enc_len);
    ESPS_CHECK_EQ(fails, rc, ESPS_COBS_OK);

    /* Encoded form must never contain a 0x00 — that is the entire point. */
    for (size_t i = 0; i < enc_len; i++) {
        ESPS_CHECK(fails, enc[i] != 0x00);
    }

    uint8_t dec[600];
    size_t dec_len = 0;
    rc = esps_cobs_decode(enc, enc_len, dec, sizeof(dec), &dec_len);
    ESPS_CHECK_EQ(fails, rc, ESPS_COBS_OK);
    ESPS_CHECK_EQ(fails, dec_len, src_len);
    if (dec_len == src_len) {
        ESPS_CHECK(fails, memcmp(dec, src, src_len) == 0);
    }
}

int test_cobs_all(void) {
    int fails = 0;

    /* Empty input. */
    roundtrip(&fails, (const uint8_t *)"", 0);
    {
        uint8_t enc[4];
        size_t enc_len = 0;
        ESPS_CHECK_EQ(&fails, esps_cobs_encode(NULL, 0, enc, sizeof(enc), &enc_len), ESPS_COBS_OK);
        ESPS_CHECK_EQ(&fails, enc_len, 1);
        ESPS_CHECK_EQ(&fails, enc[0], 0x01);
    }

    /* All-zero buffers of a few lengths. */
    {
        uint8_t zeros[10] = {0};
        for (size_t n = 1; n <= sizeof(zeros); n++) {
            roundtrip(&fails, zeros, n);
        }
    }

    /* Block-boundary cases: exactly 254 and exactly 255 non-zero bytes. */
    {
        uint8_t buf254[254];
        for (size_t i = 0; i < sizeof(buf254); i++) {
            buf254[i] = (uint8_t)(i + 1); /* never zero */
        }
        uint8_t enc[ESPS_COBS_MAX_ENCODED(255)];
        size_t enc_len = 0;
        esps_cobs_err_t rc = esps_cobs_encode(buf254, sizeof(buf254), enc, sizeof(enc), &enc_len);
        ESPS_CHECK_EQ(&fails, rc, ESPS_COBS_OK);
        /* The classic COBS gotcha: input ending exactly on a 254-byte block
         * boundary needs a trailing empty-block code byte (0x01) even though
         * no data follows, because the 0xFF block that just closed carries
         * no implicit terminator of its own — [0xFF][254 bytes][0x01]. */
        ESPS_CHECK_EQ(&fails, enc_len, 256);
        roundtrip(&fails, buf254, sizeof(buf254));

        uint8_t buf255[255];
        for (size_t i = 0; i < sizeof(buf255); i++) {
            buf255[i] = (uint8_t)(i + 1);
        }
        rc = esps_cobs_encode(buf255, sizeof(buf255), enc, sizeof(enc), &enc_len);
        ESPS_CHECK_EQ(&fails, rc, ESPS_COBS_OK);
        /* 254 bytes under one 0xFF block + 1 byte under a trailing 0x02 block. */
        ESPS_CHECK_EQ(&fails, enc_len, 257);
        roundtrip(&fails, buf255, sizeof(buf255));
    }

    /* A block with a zero byte in the middle, requiring two code blocks. */
    {
        uint8_t mixed[8] = {1, 2, 0, 3, 4, 5, 0, 6};
        roundtrip(&fails, mixed, sizeof(mixed));
    }

    /* Malformed decode: code byte claims a block longer than what remains. */
    {
        uint8_t bad[] = {0x05, 0x01, 0x02}; /* claims 4 data bytes, only 2 present */
        uint8_t dec[16];
        size_t dec_len = 0;
        esps_cobs_err_t rc = esps_cobs_decode(bad, sizeof(bad), dec, sizeof(dec), &dec_len);
        ESPS_CHECK_EQ(&fails, rc, ESPS_COBS_ERR_MALFORMED);
    }

    /* Malformed decode: an embedded 0x00 (never legal inside a COBS block). */
    {
        uint8_t bad[] = {0x02, 0x01, 0x00, 0x02, 0xAA};
        uint8_t dec[16];
        size_t dec_len = 0;
        esps_cobs_err_t rc = esps_cobs_decode(bad, sizeof(bad), dec, sizeof(dec), &dec_len);
        ESPS_CHECK_EQ(&fails, rc, ESPS_COBS_ERR_MALFORMED);
    }

    /* Capacity rejection: encode into a too-small destination. */
    {
        uint8_t src[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        uint8_t tiny[3];
        size_t out_len = 0;
        esps_cobs_err_t rc = esps_cobs_encode(src, sizeof(src), tiny, sizeof(tiny), &out_len);
        ESPS_CHECK_EQ(&fails, rc, ESPS_COBS_ERR_CAPACITY);
    }

    /* Capacity rejection: decode into a too-small destination. */
    {
        uint8_t src[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        uint8_t enc[ESPS_COBS_MAX_ENCODED(10)];
        size_t enc_len = 0;
        esps_cobs_encode(src, sizeof(src), enc, sizeof(enc), &enc_len);
        uint8_t tiny[3];
        size_t out_len = 0;
        esps_cobs_err_t rc = esps_cobs_decode(enc, enc_len, tiny, sizeof(tiny), &out_len);
        ESPS_CHECK_EQ(&fails, rc, ESPS_COBS_ERR_CAPACITY);
    }

    return fails;
}

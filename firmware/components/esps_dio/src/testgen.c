#include "esps_dio_testgen.h"

uint32_t esps_dio_xorshift32(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

size_t esps_dio_testframe_payload(uint32_t seq, uint8_t out[ESPS_DIO_MAX_PAYLOAD]) {
    if (out == NULL) {
        return 0;
    }
    /* 2654435761 is Knuth's multiplicative hash constant (2^32 / golden
     * ratio); it spreads consecutive seq values across the state space so
     * neighbouring frames do not get correlated lengths. */
    uint32_t st = (uint32_t)ESPS_DIO_TEST_SEED ^ (uint32_t)(seq * 2654435761u);
    if (st == 0u) {
        st = 1u;
    }
    st = esps_dio_xorshift32(st);
    const size_t len = 1u + (size_t)(st % 32u);
    out[0] = (uint8_t)(seq & 0xFFu);
    for (size_t i = 1; i < len; i++) {
        st = esps_dio_xorshift32(st);
        out[i] = (uint8_t)((st >> 8) & 0xFFu);
    }
    for (size_t i = len; i < ESPS_DIO_MAX_PAYLOAD; i++) {
        out[i] = 0;
    }
    return len;
}

uint32_t esps_dio_resync_index(uint32_t expected_k, uint8_t byte0) {
    return expected_k + (((uint32_t)byte0 - expected_k) & 0xFFu);
}

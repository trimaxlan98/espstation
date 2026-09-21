/* Frame builder, bit-serial receiver and txbits iterator. Golden frames come
 * from SPEC-LINK.md and were cross-checked with an independent Python model;
 * the CRC-corruption sweep below is also mirrored by that model. */
#include "dio_helpers.h"
#include "esps_dio_crc8.h"
#include "esps_dio_frame.h"
#include "esps_dio_testgen.h"
#include "harness.h"

#include <string.h>

typedef struct {
    const char *name;
    const char *payload_hex;
    const char *frame_hex;
} golden_t;

static const golden_t GOLDEN[] = {
    {"[0x00]", "00", "aa010015"},
    {"ESP", "455350", "aa03455350f8"},
    {"seq=1", "012f", "aa02012f0e"},
    {"seq=999", "e741", "aa02e7413e"},
    {"seq=0", "004a9d11b4cb431957fd2d5e40a7affdd0", "aa11004a9d11b4cb431957fd2d5e40a7affdd017"},
    {"seq=2", "022464f86b91dc51d3fac2fb8ca4561b5197dee1",
     "aa14022464f86b91dc51d3fac2fb8ca4561b5197dee1a0"},
};
#define N_GOLDEN (sizeof(GOLDEN) / sizeof(GOLDEN[0]))

static void test_build_golden(int *fails) {
    for (size_t i = 0; i < N_GOLDEN; i++) {
        uint8_t payload[32], want[40], out[40];
        size_t plen = dio_unhex(GOLDEN[i].payload_hex, payload, sizeof(payload));
        size_t wlen = dio_unhex(GOLDEN[i].frame_hex, want, sizeof(want));
        size_t olen = 0;
        ESPS_CHECK(fails, plen > 0 && wlen == plen + 3);
        ESPS_CHECK(fails, esps_dio_frame_build(payload, plen, out, sizeof(out), &olen));
        ESPS_CHECK_EQ(fails, olen, wlen);
        ESPS_CHECK(fails, olen == wlen && memcmp(out, want, wlen) == 0);
    }
}

static void test_build_rejections(int *fails) {
    uint8_t payload[40];
    memset(payload, 0x5A, sizeof(payload));
    uint8_t out[64];
    size_t olen = 99;

    /* len bounds */
    memset(out, 0xEE, sizeof(out));
    ESPS_CHECK(fails, !esps_dio_frame_build(payload, 0, out, sizeof(out), &olen));
    ESPS_CHECK_EQ(fails, olen, 0);
    olen = 99;
    ESPS_CHECK(fails, !esps_dio_frame_build(payload, 33, out, sizeof(out), &olen));
    ESPS_CHECK_EQ(fails, olen, 0);
    olen = 99;
    ESPS_CHECK(fails, !esps_dio_frame_build(payload, (size_t)-1, out, sizeof(out), &olen));
    ESPS_CHECK_EQ(fails, olen, 0);
    ESPS_CHECK(fails, esps_dio_frame_build(payload, 32, out, sizeof(out), &olen));
    ESPS_CHECK_EQ(fails, olen, 35);
    ESPS_CHECK_EQ(fails, ESPS_DIO_FRAME_MAX, 35);

    /* capacity: exactly len+3 fits, len+2 does not and leaves out untouched */
    uint8_t tight[35];
    memset(tight, 0xEE, sizeof(tight));
    ESPS_CHECK(fails, esps_dio_frame_build(payload, 5, tight, 8, &olen));
    ESPS_CHECK_EQ(fails, olen, 8);
    memset(tight, 0xEE, sizeof(tight));
    olen = 99;
    ESPS_CHECK(fails, !esps_dio_frame_build(payload, 5, tight, 7, &olen));
    ESPS_CHECK_EQ(fails, olen, 0);
    for (size_t i = 0; i < sizeof(tight); i++) {
        ESPS_CHECK_EQ(fails, tight[i], 0xEE);
    }
    ESPS_CHECK(fails, !esps_dio_frame_build(payload, 5, tight, 0, &olen));

    /* NULLs */
    olen = 99;
    ESPS_CHECK(fails, !esps_dio_frame_build(NULL, 5, out, sizeof(out), &olen));
    ESPS_CHECK_EQ(fails, olen, 0);
    ESPS_CHECK(fails, !esps_dio_frame_build(payload, 5, NULL, sizeof(out), &olen));
    ESPS_CHECK(fails, !esps_dio_frame_build(payload, 5, out, sizeof(out), NULL));
}

static void test_rx_golden_roundtrip(int *fails) {
    for (size_t i = 0; i < N_GOLDEN; i++) {
        uint8_t payload[32], frame[40];
        size_t plen = dio_unhex(GOLDEN[i].payload_hex, payload, sizeof(payload));
        size_t flen = dio_unhex(GOLDEN[i].frame_hex, frame, sizeof(frame));
        esps_dio_rx_t rx;
        esps_dio_rx_init(&rx);
        esps_dio_rx_result_t r = dio_feed(&rx, frame, flen);
        ESPS_CHECK_EQ(fails, r, ESPS_DIO_RX_FRAME_OK);
        ESPS_CHECK_EQ(fails, rx.len, plen);
        ESPS_CHECK(fails, rx.len == plen && memcmp(rx.payload, payload, plen) == 0);
        ESPS_CHECK_EQ(fails, rx.state, ESPS_DIO_RXS_HUNT);
    }
}

/* build -> bit-serial rx over the generator's payloads: every length 1..32
 * occurs across this range, so all payload sizes are exercised. */
static void test_rx_roundtrip_generated(int *fails) {
    unsigned seen_len[33] = {0};
    for (uint32_t seq = 0; seq < 2000; seq++) {
        uint8_t p[32], frame[ESPS_DIO_FRAME_MAX];
        size_t len = esps_dio_testframe_payload(seq, p);
        size_t flen = 0;
        ESPS_CHECK(fails, esps_dio_frame_build(p, len, frame, sizeof(frame), &flen));
        esps_dio_rx_t rx;
        esps_dio_rx_init(&rx);
        ESPS_CHECK_EQ(fails, dio_feed(&rx, frame, flen), ESPS_DIO_RX_FRAME_OK);
        ESPS_CHECK_EQ(fails, rx.len, len);
        ESPS_CHECK(fails, memcmp(rx.payload, p, len) == 0);
        seen_len[len]++;
    }
    for (size_t l = 1; l <= 32; l++) {
        ESPS_CHECK(fails, seen_len[l] > 0);
    }
}

static void test_rx_garbage_before_sync(int *fails) {
    uint8_t frame[ESPS_DIO_FRAME_MAX];
    size_t flen = 0;
    static const uint8_t payload[] = {0x12, 0x34, 0x56};
    ESPS_CHECK(fails, esps_dio_frame_build(payload, sizeof(payload), frame, sizeof(frame), &flen));

    /* Whole garbage bytes with no 0xAA at any bit alignment. */
    static const uint8_t junk[] = {0x00, 0xFF, 0x0F, 0x00, 0x33, 0xC3};
    esps_dio_rx_t rx;
    esps_dio_rx_init(&rx);
    ESPS_CHECK_EQ(fails, dio_feed(&rx, junk, sizeof(junk)), ESPS_DIO_RX_NONE);
    ESPS_CHECK_EQ(fails, dio_feed(&rx, frame, flen), ESPS_DIO_RX_FRAME_OK);
    ESPS_CHECK(fails, rx.len == 3 && memcmp(rx.payload, payload, 3) == 0);

    /* Bit-misaligned prefix: 3 stray bits (1,0,1) shift the frame off the byte
     * grid; the hunt must lock on bit granularity. */
    esps_dio_rx_init(&rx);
    ESPS_CHECK_EQ(fails, esps_dio_rx_push_bit(&rx, 1), ESPS_DIO_RX_NONE);
    ESPS_CHECK_EQ(fails, esps_dio_rx_push_bit(&rx, 0), ESPS_DIO_RX_NONE);
    ESPS_CHECK_EQ(fails, esps_dio_rx_push_bit(&rx, 1), ESPS_DIO_RX_NONE);
    ESPS_CHECK_EQ(fails, dio_feed(&rx, frame, flen), ESPS_DIO_RX_FRAME_OK);
    ESPS_CHECK(fails, rx.len == 3 && memcmp(rx.payload, payload, 3) == 0);

    /* A long idle line (DATA=0) before the frame changes nothing. */
    uint8_t idle[8] = {0};
    esps_dio_rx_init(&rx);
    ESPS_CHECK_EQ(fails, dio_feed(&rx, idle, sizeof(idle)), ESPS_DIO_RX_NONE);
    ESPS_CHECK_EQ(fails, dio_feed(&rx, frame, flen), ESPS_DIO_RX_FRAME_OK);
}

static void test_rx_sync_byte_inside_payload(int *fails) {
    static const uint8_t payload[] = {0xAA, 0xAA, 0xAA, 0x55, 0xAA};
    uint8_t frame[ESPS_DIO_FRAME_MAX];
    size_t flen = 0;
    ESPS_CHECK(fails, esps_dio_frame_build(payload, sizeof(payload), frame, sizeof(frame), &flen));
    esps_dio_rx_t rx;
    esps_dio_rx_init(&rx);
    esps_dio_rx_result_t res[4];
    size_t n = dio_feed_all(&rx, frame, flen, res, 4);
    ESPS_CHECK_EQ(fails, n, 1);
    ESPS_CHECK_EQ(fails, res[0], ESPS_DIO_RX_FRAME_OK);
    ESPS_CHECK(fails, rx.len == sizeof(payload) && memcmp(rx.payload, payload, sizeof(payload)) == 0);

    /* A payload byte pair straddling 0xAA on a non-byte boundary (0x55 0x55
     * carries 10101010 at bit offset 1) must not resync either. */
    static const uint8_t straddle[] = {0x55, 0x55, 0x55, 0x2A, 0xA8};
    ESPS_CHECK(fails, esps_dio_frame_build(straddle, sizeof(straddle), frame, sizeof(frame), &flen));
    esps_dio_rx_init(&rx);
    n = dio_feed_all(&rx, frame, flen, res, 4);
    ESPS_CHECK_EQ(fails, n, 1);
    ESPS_CHECK_EQ(fails, res[0], ESPS_DIO_RX_FRAME_OK);
    ESPS_CHECK(fails, memcmp(rx.payload, straddle, sizeof(straddle)) == 0);
}

static void test_rx_len_errors(int *fails) {
    esps_dio_rx_t rx;
    esps_dio_rx_result_t r;

    /* LEN = 0 */
    static const uint8_t len0[] = {0xAA, 0x00};
    esps_dio_rx_init(&rx);
    r = dio_feed(&rx, len0, sizeof(len0));
    ESPS_CHECK_EQ(fails, r, ESPS_DIO_RX_FRAME_LEN_ERR);
    ESPS_CHECK_EQ(fails, rx.len, 0);
    ESPS_CHECK_EQ(fails, rx.state, ESPS_DIO_RXS_HUNT);

    /* LEN = 33, and the extremes: 0x21, 0x80, 0xAA, 0xFF */
    static const uint8_t bad_lens[] = {33, 0x80, 0xAA, 0xFF};
    for (size_t i = 0; i < sizeof(bad_lens); i++) {
        const uint8_t buf[] = {0xAA, bad_lens[i]};
        esps_dio_rx_init(&rx);
        r = dio_feed(&rx, buf, sizeof(buf));
        ESPS_CHECK_EQ(fails, r, ESPS_DIO_RX_FRAME_LEN_ERR);
        ESPS_CHECK_EQ(fails, rx.state, ESPS_DIO_RXS_HUNT);
    }

    /* LEN = 1 and 32 are the accepted extremes. */
    static const uint8_t ok_lens[] = {1, 32};
    for (size_t i = 0; i < sizeof(ok_lens); i++) {
        const uint8_t buf[] = {0xAA, ok_lens[i]};
        esps_dio_rx_init(&rx);
        r = dio_feed(&rx, buf, sizeof(buf));
        ESPS_CHECK_EQ(fails, r, ESPS_DIO_RX_NONE);
        ESPS_CHECK_EQ(fails, rx.state, ESPS_DIO_RXS_PAYLOAD);
        ESPS_CHECK_EQ(fails, rx.len, ok_lens[i]);
    }

    /* Recovery: a bad LEN followed immediately by a good frame. 0xFA ends in
     * 1010, which would false-sync 4 bits early if the shift register were
     * not cleared on LEN_ERR. */
    uint8_t good[ESPS_DIO_FRAME_MAX];
    size_t glen = 0;
    static const uint8_t payload[] = {0x42};
    ESPS_CHECK(fails, esps_dio_frame_build(payload, 1, good, sizeof(good), &glen));
    uint8_t stream[2 + ESPS_DIO_FRAME_MAX];
    stream[0] = 0xAA;
    stream[1] = 0xFA;
    memcpy(&stream[2], good, glen);
    esps_dio_rx_result_t res[4];
    esps_dio_rx_init(&rx);
    size_t n = dio_feed_all(&rx, stream, 2 + glen, res, 4);
    ESPS_CHECK_EQ(fails, n, 2);
    ESPS_CHECK_EQ(fails, res[0], ESPS_DIO_RX_FRAME_LEN_ERR);
    ESPS_CHECK_EQ(fails, res[1], ESPS_DIO_RX_FRAME_OK);
    ESPS_CHECK(fails, rx.len == 1 && rx.payload[0] == 0x42);
}

static void test_rx_two_frames(int *fails) {
    static const uint8_t pa[] = {0x01, 0x02, 0x03};
    static const uint8_t pb[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00};
    uint8_t stream[2 * ESPS_DIO_FRAME_MAX];
    size_t la = 0, lb = 0;
    ESPS_CHECK(fails, esps_dio_frame_build(pa, sizeof(pa), stream, sizeof(stream), &la));
    ESPS_CHECK(fails, esps_dio_frame_build(pb, sizeof(pb), &stream[la], sizeof(stream) - la, &lb));

    esps_dio_rx_t rx;
    esps_dio_rx_init(&rx);
    /* Feed bit by bit and snapshot the payload at each frame boundary, since
     * rx keeps only the latest one. */
    esps_dio_txbits_t tx;
    esps_dio_txbits_init(&tx, stream, la + lb);
    uint8_t bit, first[32], second[32];
    uint8_t first_len = 0, second_len = 0;
    int frames = 0;
    while (esps_dio_txbits_next(&tx, &bit)) {
        esps_dio_rx_result_t r = esps_dio_rx_push_bit(&rx, bit);
        if (r == ESPS_DIO_RX_FRAME_OK) {
            if (frames == 0) { first_len = rx.len; memcpy(first, rx.payload, rx.len); }
            if (frames == 1) { second_len = rx.len; memcpy(second, rx.payload, rx.len); }
            frames++;
        } else {
            ESPS_CHECK_EQ(fails, r, ESPS_DIO_RX_NONE);
        }
    }
    ESPS_CHECK_EQ(fails, frames, 2);
    ESPS_CHECK(fails, first_len == sizeof(pa) && memcmp(first, pa, sizeof(pa)) == 0);
    ESPS_CHECK(fails, second_len == sizeof(pb) && memcmp(second, pb, sizeof(pb)) == 0);

    /* Zero gap between frames must work even when the first frame's last bits
     * look like the start of a sync. Find a 1-byte payload whose CRC ends in
     * 1010 (low nibble 0xA): then "...1010" + "1010 1010" would false-sync
     * 4 bits early if the shift register were not cleared after the CRC. */
    int found = -1;
    for (int v = 0; v < 256 && found < 0; v++) {
        uint8_t p1 = (uint8_t)v;
        uint8_t body[2] = {1, p1};
        if ((esps_dio_crc8(body, 2, 0) & 0x0Fu) == 0x0Au) found = v;
    }
    ESPS_CHECK(fails, found >= 0);
    uint8_t p1 = (uint8_t)found, one[4], both[8];
    size_t l1 = 0;
    ESPS_CHECK(fails, esps_dio_frame_build(&p1, 1, one, sizeof(one), &l1));
    memcpy(both, one, l1);
    memcpy(&both[l1], one, l1);
    esps_dio_rx_result_t res[4];
    esps_dio_rx_init(&rx);
    ESPS_CHECK_EQ(fails, dio_feed_all(&rx, both, 2 * l1, res, 4), 2);
    ESPS_CHECK_EQ(fails, res[0], ESPS_DIO_RX_FRAME_OK);
    ESPS_CHECK_EQ(fails, res[1], ESPS_DIO_RX_FRAME_OK);
    ESPS_CHECK(fails, rx.len == 1 && rx.payload[0] == p1);
}

/* Flip every body bit (LEN, PAYLOAD, CRC) of a frame, one at a time, and
 * classify what the receiver reports first. The frame is followed by idle
 * zeros so a LEN that grew still completes.
 *
 * Bit flips inside PAYLOAD or CRC keep the frame length fixed, and a CRC with
 * a non-zero constant term detects every single-bit error, so those can never
 * be FRAME_OK. A flip in LEN is different: it moves the frame boundary, the
 * receiver then compares an unrelated byte with an unrelated CRC, and roughly
 * 1 in 256 of those match by chance — a real collision, not a bug. */
typedef struct {
    unsigned len_flips, len_ok;         /* flips in LEN and how many yielded OK */
    unsigned other_flips, other_ok;     /* flips in PAYLOAD/CRC and how many yielded OK */
} flip_tally_t;

static void sweep_flips(const uint8_t *frame, size_t flen, flip_tally_t *t) {
    uint8_t g[ESPS_DIO_FRAME_MAX + 40];
    for (size_t bi = 8; bi < flen * 8; bi++) {
        memset(g, 0, sizeof(g));
        memcpy(g, frame, flen);
        g[bi / 8] ^= (uint8_t)(0x80u >> (bi % 8));
        esps_dio_rx_t rx;
        esps_dio_rx_init(&rx);
        esps_dio_rx_result_t r = dio_feed(&rx, g, flen + 36);
        if (bi < 16) {
            t->len_flips++;
            if (r == ESPS_DIO_RX_FRAME_OK) t->len_ok++;
        } else {
            t->other_flips++;
            if (r == ESPS_DIO_RX_FRAME_OK) t->other_ok++;
        }
    }
}

static void test_rx_bit_flip_sweep(int *fails) {
    /* Golden frames: no flip of any kind yields OK. */
    for (size_t i = 0; i < N_GOLDEN; i++) {
        uint8_t frame[40];
        size_t flen = dio_unhex(GOLDEN[i].frame_hex, frame, sizeof(frame));
        flip_tally_t t = {0, 0, 0, 0};
        sweep_flips(frame, flen, &t);
        ESPS_CHECK_EQ(fails, t.len_flips, 8);
        ESPS_CHECK_EQ(fails, t.other_flips, (flen - 2) * 8);
        ESPS_CHECK_EQ(fails, t.len_ok, 0);
        ESPS_CHECK_EQ(fails, t.other_ok, 0);
    }

    /* Corpus sweep over generated frames seq 0..999. The expected collision
     * count (13 of 8000 LEN flips) comes from the independent Python model of
     * the same state machine, so this also cross-checks the C receiver. */
    flip_tally_t t = {0, 0, 0, 0};
    for (uint32_t seq = 0; seq < 1000; seq++) {
        uint8_t p[32], frame[ESPS_DIO_FRAME_MAX];
        size_t len = esps_dio_testframe_payload(seq, p), flen = 0;
        ESPS_CHECK(fails, esps_dio_frame_build(p, len, frame, sizeof(frame), &flen));
        sweep_flips(frame, flen, &t);
    }
    fprintf(stderr, "  bit-flip sweep, seq 0..999: LEN flips %u -> %u spurious FRAME_OK; "
                    "PAYLOAD/CRC flips %u -> %u\n",
            t.len_flips, t.len_ok, t.other_flips, t.other_ok);
    ESPS_CHECK_EQ(fails, t.len_flips, 8000);
    ESPS_CHECK_EQ(fails, t.other_ok, 0);
    ESPS_CHECK_EQ(fails, t.len_ok, 13);
}

static void test_rx_null_and_state(int *fails) {
    esps_dio_rx_init(NULL); /* must not crash */
    ESPS_CHECK_EQ(fails, esps_dio_rx_push_bit(NULL, 1), ESPS_DIO_RX_NONE);

    esps_dio_rx_t rx;
    memset(&rx, 0xFF, sizeof(rx));
    esps_dio_rx_init(&rx);
    ESPS_CHECK_EQ(fails, rx.state, ESPS_DIO_RXS_HUNT);
    ESPS_CHECK_EQ(fails, rx.len, 0);

    /* Any non-zero bit value counts as 1. */
    static const uint8_t frame[] = {0xAA, 0x01, 0x00, 0x15};
    esps_dio_txbits_t tx;
    esps_dio_txbits_init(&tx, frame, sizeof(frame));
    uint8_t bit;
    esps_dio_rx_result_t r = ESPS_DIO_RX_NONE;
    while (esps_dio_txbits_next(&tx, &bit)) {
        r = esps_dio_rx_push_bit(&rx, bit ? 0x80 : 0);
    }
    ESPS_CHECK_EQ(fails, r, ESPS_DIO_RX_FRAME_OK);

    /* Payload of a completed frame survives until the next frame's bytes
     * arrive; pushing hunt bits does not disturb it. */
    ESPS_CHECK_EQ(fails, rx.payload[0], 0x00);
    esps_dio_rx_push_bit(&rx, 0);
    esps_dio_rx_push_bit(&rx, 1);
    ESPS_CHECK_EQ(fails, rx.len, 1);
}

static void test_txbits(int *fails) {
    static const uint8_t bytes[] = {0xA5, 0x01, 0x80};
    static const uint8_t want[24] = {1, 0, 1, 0, 0, 1, 0, 1,  /* 0xA5 MSB first */
                                     0, 0, 0, 0, 0, 0, 0, 1,  /* 0x01 */
                                     1, 0, 0, 0, 0, 0, 0, 0}; /* 0x80 */
    esps_dio_txbits_t tx;
    esps_dio_txbits_init(&tx, bytes, sizeof(bytes));
    uint8_t bit = 0xEE;
    for (size_t i = 0; i < 24; i++) {
        ESPS_CHECK(fails, esps_dio_txbits_next(&tx, &bit));
        ESPS_CHECK_EQ(fails, bit, want[i]);
    }
    bit = 0xEE;
    ESPS_CHECK(fails, !esps_dio_txbits_next(&tx, &bit));
    ESPS_CHECK_EQ(fails, bit, 0xEE); /* untouched when exhausted */
    ESPS_CHECK(fails, !esps_dio_txbits_next(&tx, &bit));

    esps_dio_txbits_init(&tx, bytes, 0);
    ESPS_CHECK(fails, !esps_dio_txbits_next(&tx, &bit));
    esps_dio_txbits_init(&tx, NULL, 5);
    ESPS_CHECK(fails, !esps_dio_txbits_next(&tx, &bit));

    esps_dio_txbits_init(NULL, bytes, 3);
    ESPS_CHECK(fails, !esps_dio_txbits_next(NULL, &bit));
    esps_dio_txbits_init(&tx, bytes, 3);
    ESPS_CHECK(fails, !esps_dio_txbits_next(&tx, NULL));

    /* Feeding a whole built frame through txbits reproduces its bytes. */
    static const uint8_t esp[] = {'E', 'S', 'P'};
    uint8_t frame[ESPS_DIO_FRAME_MAX], rebuilt[ESPS_DIO_FRAME_MAX];
    size_t flen = 0;
    ESPS_CHECK(fails, esps_dio_frame_build(esp, 3, frame, sizeof(frame), &flen));
    esps_dio_txbits_init(&tx, frame, flen);
    memset(rebuilt, 0, sizeof(rebuilt));
    size_t nbits = 0;
    while (esps_dio_txbits_next(&tx, &bit)) {
        rebuilt[nbits / 8] = (uint8_t)((rebuilt[nbits / 8] << 1) | bit);
        nbits++;
    }
    ESPS_CHECK_EQ(fails, nbits, flen * 8);
    ESPS_CHECK(fails, memcmp(rebuilt, frame, flen) == 0);
}

int test_dio_frame_all(void) {
    int fails = 0;
    test_build_golden(&fails);
    test_build_rejections(&fails);
    test_rx_golden_roundtrip(&fails);
    test_rx_roundtrip_generated(&fails);
    test_rx_garbage_before_sync(&fails);
    test_rx_sync_byte_inside_payload(&fails);
    test_rx_len_errors(&fails);
    test_rx_two_frames(&fails);
    test_rx_bit_flip_sweep(&fails);
    test_rx_null_and_state(&fails);
    test_txbits(&fails);
    return fails;
}

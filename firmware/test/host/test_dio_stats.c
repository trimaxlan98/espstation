/* Link accounting: bit-exact BER with errors injected at known positions,
 * driven through the real build -> bit-serial rx -> account pipeline. */
#include "dio_helpers.h"
#include "esps_dio_frame.h"
#include "esps_dio_stats.h"
#include "esps_dio_testgen.h"
#include "harness.h"

#include <string.h>

static int approx(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d <= 1e-9f;
}

/* Sends the expected payload of `seq` over the wire with the given payload
 * bit positions flipped (bit index counted MSB-first from the start of the
 * payload, i.e. wire order), receives it and accounts it. Returns the rx
 * result. */
static esps_dio_rx_result_t send_and_account(esps_dio_link_stats_t *st, uint32_t seq,
                                             const unsigned *flip_bits, size_t n_flips) {
    uint8_t exp[32], frame[ESPS_DIO_FRAME_MAX];
    size_t len = esps_dio_testframe_payload(seq, exp), flen = 0;
    esps_dio_frame_build(exp, len, frame, sizeof(frame), &flen);
    for (size_t i = 0; i < n_flips; i++) {
        frame[2 + flip_bits[i] / 8] ^= (uint8_t)(0x80u >> (flip_bits[i] % 8));
    }
    esps_dio_rx_t rx;
    esps_dio_rx_init(&rx);
    esps_dio_rx_result_t r = dio_feed(&rx, frame, flen);
    esps_dio_stats_account(st, r, rx.payload, exp, rx.len);
    return r;
}

int test_dio_stats_all(void) {
    int fails = 0;

    /* Empty stats: BER is 0.0, not NaN. */
    esps_dio_link_stats_t st;
    memset(&st, 0, sizeof(st));
    ESPS_CHECK(&fails, esps_dio_stats_ber(&st) == 0.0f);
    ESPS_CHECK(&fails, esps_dio_stats_ber(NULL) == 0.0f);

    /* Clean traffic: seq 0..99, zero errors, bits_rx = sum 8*(len+2). */
    uint32_t want_bits = 0;
    for (uint32_t seq = 0; seq < 100; seq++) {
        uint8_t p[32];
        want_bits += 8u * ((uint32_t)esps_dio_testframe_payload(seq, p) + 2u);
        ESPS_CHECK_EQ(&fails, send_and_account(&st, seq, NULL, 0), ESPS_DIO_RX_FRAME_OK);
    }
    ESPS_CHECK_EQ(&fails, st.frames_ok, 100);
    ESPS_CHECK_EQ(&fails, st.frames_crc_err, 0);
    ESPS_CHECK_EQ(&fails, st.frames_len_err, 0);
    ESPS_CHECK_EQ(&fails, st.bit_errors, 0);
    ESPS_CHECK_EQ(&fails, st.bits_rx, want_bits);
    ESPS_CHECK(&fails, esps_dio_stats_ber(&st) == 0.0f);

    /* Known injected errors. seq=0 is 17 B (payload wire bits 0..135); flips
     * at bits 3 (byte 0), 20 and 22 (byte 2, two different bits of the same
     * byte), 135 (last bit of the last byte) = exactly 4 errors. The receiver
     * sees a CRC error, and the payload it hands over still carries the
     * flips, so the popcount is exact. */
    memset(&st, 0, sizeof(st));
    static const unsigned flips4[] = {3, 20, 22, 135};
    ESPS_CHECK_EQ(&fails, send_and_account(&st, 0, flips4, 4), ESPS_DIO_RX_FRAME_CRC_ERR);
    ESPS_CHECK_EQ(&fails, st.frames_crc_err, 1);
    ESPS_CHECK_EQ(&fails, st.frames_ok, 0);
    ESPS_CHECK_EQ(&fails, st.bit_errors, 4);
    ESPS_CHECK_EQ(&fails, st.bits_rx, 8 * (17 + 2));
    ESPS_CHECK(&fails, approx(esps_dio_stats_ber(&st), 4.0f / 152.0f));

    /* One more corrupted frame (seq=1, 2 B): a single flip. Totals add up. */
    static const unsigned flips1[] = {9};
    ESPS_CHECK_EQ(&fails, send_and_account(&st, 1, flips1, 1), ESPS_DIO_RX_FRAME_CRC_ERR);
    ESPS_CHECK_EQ(&fails, st.frames_crc_err, 2);
    ESPS_CHECK_EQ(&fails, st.bit_errors, 5);
    ESPS_CHECK_EQ(&fails, st.bits_rx, 8 * (17 + 2) + 8 * (2 + 2));
    ESPS_CHECK(&fails, approx(esps_dio_stats_ber(&st), 5.0f / 184.0f));

    /* LEN_ERR: counted, but adds no bits and no bit errors. */
    static const uint8_t bad_len[] = {0xAA, 0x00};
    esps_dio_rx_t rx;
    esps_dio_rx_init(&rx);
    esps_dio_rx_result_t r = dio_feed(&rx, bad_len, sizeof(bad_len));
    ESPS_CHECK_EQ(&fails, r, ESPS_DIO_RX_FRAME_LEN_ERR);
    uint8_t exp[32];
    esps_dio_testframe_payload(0, exp);
    esps_dio_stats_account(&st, r, rx.payload, exp, rx.len);
    ESPS_CHECK_EQ(&fails, st.frames_len_err, 1);
    ESPS_CHECK_EQ(&fails, st.bits_rx, 8 * (17 + 2) + 8 * (2 + 2));
    ESPS_CHECK_EQ(&fails, st.bit_errors, 5);

    /* NONE is a no-op. */
    esps_dio_link_stats_t before = st;
    esps_dio_stats_account(&st, ESPS_DIO_RX_NONE, exp, exp, 4);
    ESPS_CHECK(&fails, memcmp(&before, &st, sizeof(st)) == 0);

    /* CRC collision: valid CRC, payload differs from the expected one in 3
     * bits. Counted as frames_ok AND in bit_errors, so it is visible. */
    memset(&st, 0, sizeof(st));
    {
        uint8_t want[32], sent[32], frame[ESPS_DIO_FRAME_MAX];
        size_t len = esps_dio_testframe_payload(2, want), flen = 0;
        memcpy(sent, want, len);
        sent[0] ^= 0x01;
        sent[7] ^= 0x88; /* two bits of one byte */
        esps_dio_frame_build(sent, len, frame, sizeof(frame), &flen);
        esps_dio_rx_init(&rx);
        r = dio_feed(&rx, frame, flen);
        ESPS_CHECK_EQ(&fails, r, ESPS_DIO_RX_FRAME_OK);
        esps_dio_stats_account(&st, r, rx.payload, want, rx.len);
        ESPS_CHECK_EQ(&fails, st.frames_ok, 1);
        ESPS_CHECK_EQ(&fails, st.bit_errors, 3);
        ESPS_CHECK_EQ(&fails, st.bits_rx, 8 * (20 + 2));
    }

    /* Worst-case popcount: every payload bit wrong (len 32 -> 256 errors). */
    memset(&st, 0, sizeof(st));
    {
        uint8_t got[32], want[32];
        memset(got, 0xFF, sizeof(got));
        memset(want, 0x00, sizeof(want));
        esps_dio_stats_account(&st, ESPS_DIO_RX_FRAME_CRC_ERR, got, want, 32);
        ESPS_CHECK_EQ(&fails, st.bit_errors, 256);
        ESPS_CHECK_EQ(&fails, st.bits_rx, 8 * 34);
        ESPS_CHECK(&fails, approx(esps_dio_stats_ber(&st), 256.0f / 272.0f));
    }

    /* Resync scenario: frames 3 and 4 are lost. The receiver's expected index
     * is re-derived from byte 0 of the next valid frame, after which the
     * expected payload matches again and BER stays 0. */
    memset(&st, 0, sizeof(st));
    {
        uint32_t k = 0;
        static const uint32_t sent_seqs[] = {0, 1, 2, 5, 6, 7};
        for (size_t i = 0; i < sizeof(sent_seqs) / sizeof(sent_seqs[0]); i++) {
            uint8_t p[32], frame[ESPS_DIO_FRAME_MAX], want[32];
            size_t len = esps_dio_testframe_payload(sent_seqs[i], p), flen = 0;
            esps_dio_frame_build(p, len, frame, sizeof(frame), &flen);
            esps_dio_rx_init(&rx);
            r = dio_feed(&rx, frame, flen);
            ESPS_CHECK_EQ(&fails, r, ESPS_DIO_RX_FRAME_OK);
            k = esps_dio_resync_index(k, rx.payload[0]);
            ESPS_CHECK_EQ(&fails, k, sent_seqs[i]);
            esps_dio_testframe_payload(k, want);
            esps_dio_stats_account(&st, r, rx.payload, want, rx.len);
            k++;
        }
        ESPS_CHECK_EQ(&fails, st.frames_ok, 6);
        ESPS_CHECK_EQ(&fails, st.bit_errors, 0);
    }

    /* Defensive handling. */
    memset(&st, 0, sizeof(st));
    esps_dio_stats_account(NULL, ESPS_DIO_RX_FRAME_OK, exp, exp, 4); /* no crash */
    esps_dio_stats_account(&st, ESPS_DIO_RX_FRAME_OK, exp, exp, 0);  /* len 0: ignored */
    esps_dio_stats_account(&st, ESPS_DIO_RX_FRAME_OK, exp, exp, 33); /* len 33: ignored */
    ESPS_CHECK_EQ(&fails, st.frames_ok, 0);
    ESPS_CHECK_EQ(&fails, st.bits_rx, 0);
    esps_dio_stats_account(&st, ESPS_DIO_RX_FRAME_OK, NULL, exp, 4); /* counted, no errors */
    esps_dio_stats_account(&st, ESPS_DIO_RX_FRAME_CRC_ERR, exp, NULL, 4);
    ESPS_CHECK_EQ(&fails, st.frames_ok, 1);
    ESPS_CHECK_EQ(&fails, st.frames_crc_err, 1);
    ESPS_CHECK_EQ(&fails, st.bits_rx, 2 * 8 * 6);
    ESPS_CHECK_EQ(&fails, st.bit_errors, 0);
    esps_dio_stats_account(&st, ESPS_DIO_RX_FRAME_LEN_ERR, NULL, NULL, 0);
    ESPS_CHECK_EQ(&fails, st.frames_len_err, 1);

    return fails;
}

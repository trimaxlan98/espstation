/* ENLP frame, message struct, and streaming-decoder tests. */
#include "esps_cobs.h"
#include "esps_crc16.h"
#include "esps_enlp.h"
#include "harness.h"

#include <string.h>

/* --- frame build/parse ----------------------------------------------------- */

static int test_frame_roundtrip_all_types(void) {
    int fails = 0;
    static const uint8_t types[] = {
        ESPS_MSG_HELLO,     ESPS_MSG_HELLO_ACK,  ESPS_MSG_HEARTBEAT, ESPS_MSG_TELEMETRY,
        ESPS_MSG_TELEM_ACK, ESPS_MSG_LOG,        ESPS_MSG_EVENT,     ESPS_MSG_CMD,
        ESPS_MSG_CMD_ACK,   ESPS_MSG_EXP_SET,    ESPS_MSG_EXP_STATE, ESPS_MSG_BULK_BEGIN,
        ESPS_MSG_BULK_CHUNK, ESPS_MSG_BULK_END,  ESPS_MSG_NET_REPORT, ESPS_MSG_NET_CMD,
        ESPS_MSG_TIME_SYNC,
    };
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        uint8_t buf[ESPS_ENLP_MAX_FRAME];
        size_t len = esps_enlp_encode(ESPS_ENLP_VERSION, types[i], 4711, (uint16_t)i,
                                       payload, sizeof(payload), buf, sizeof(buf));
        ESPS_CHECK(&fails, len == ESPS_ENLP_HEADER_SIZE + sizeof(payload) + ESPS_ENLP_CRC_SIZE);

        esps_enlp_frame_t frame;
        esps_enlp_err_t rc = esps_enlp_parse(buf, len, &frame);
        ESPS_CHECK_EQ(&fails, rc, ESPS_ENLP_OK);
        ESPS_CHECK_EQ(&fails, frame.ver, ESPS_ENLP_VERSION);
        ESPS_CHECK_EQ(&fails, frame.type, types[i]);
        ESPS_CHECK_EQ(&fails, frame.node, 4711);
        ESPS_CHECK_EQ(&fails, frame.seq, (uint16_t)i);
        ESPS_CHECK_EQ(&fails, frame.payload_len, sizeof(payload));
        ESPS_CHECK(&fails, memcmp(frame.payload, payload, sizeof(payload)) == 0);
    }

    /* Zero-length payload must also round-trip (payload pointer is NULL). */
    {
        uint8_t buf[ESPS_ENLP_HEADER_SIZE + ESPS_ENLP_CRC_SIZE];
        size_t len = esps_enlp_encode(ESPS_ENLP_VERSION, ESPS_MSG_HEARTBEAT, 1, 1, NULL, 0, buf,
                                       sizeof(buf));
        ESPS_CHECK_EQ(&fails, len, sizeof(buf));
        esps_enlp_frame_t frame;
        ESPS_CHECK_EQ(&fails, esps_enlp_parse(buf, len, &frame), ESPS_ENLP_OK);
        ESPS_CHECK_EQ(&fails, frame.payload_len, 0);
    }

    return fails;
}

static int test_parse_errors(void) {
    int fails = 0;
    const uint8_t payload[] = {1, 2, 3, 4};
    uint8_t buf[ESPS_ENLP_MAX_FRAME];
    size_t len = esps_enlp_encode(ESPS_ENLP_VERSION, ESPS_MSG_HEARTBEAT, 42, 7, payload,
                                   sizeof(payload), buf, sizeof(buf));
    ESPS_CHECK(&fails, len > 0);

    /* TOO_SHORT: fewer than 8 bytes, can't even read the header. */
    {
        esps_enlp_frame_t frame;
        ESPS_CHECK_EQ(&fails, esps_enlp_parse(buf, 3, &frame), ESPS_ENLP_ERR_TOO_SHORT);
    }

    /* BAD_VERSION: header intact, version byte wrong. */
    {
        uint8_t bad[ESPS_ENLP_MAX_FRAME];
        memcpy(bad, buf, len);
        bad[0] = ESPS_ENLP_VERSION + 1;
        esps_enlp_frame_t frame;
        ESPS_CHECK_EQ(&fails, esps_enlp_parse(bad, len, &frame), ESPS_ENLP_ERR_BAD_VERSION);
    }

    /* BAD_LENGTH: declared len field exceeds ESPS_ENLP_MAX_PAYLOAD. */
    {
        uint8_t bad[ESPS_ENLP_HEADER_SIZE + ESPS_ENLP_CRC_SIZE];
        memset(bad, 0, sizeof(bad));
        bad[0] = ESPS_ENLP_VERSION;
        bad[6] = 0xFF;
        bad[7] = 0xFF; /* len = 65535, way over MAX_PAYLOAD */
        esps_enlp_frame_t frame;
        ESPS_CHECK_EQ(&fails, esps_enlp_parse(bad, sizeof(bad), &frame), ESPS_ENLP_ERR_BAD_LENGTH);
    }

    /* BAD_LENGTH: a truncated frame — header says 4 bytes of payload, but the
     * buffer handed to parse() is short of that (mid-payload cut, distinct
     * from the TOO_SHORT case above which can't even read the header). */
    {
        esps_enlp_frame_t frame;
        ESPS_CHECK_EQ(&fails, esps_enlp_parse(buf, len - 3, &frame), ESPS_ENLP_ERR_BAD_LENGTH);
    }

    /* BAD_CRC: flip a bit in the payload without recomputing the trailer. */
    {
        uint8_t bad[ESPS_ENLP_MAX_FRAME];
        memcpy(bad, buf, len);
        bad[ESPS_ENLP_HEADER_SIZE] ^= 0x01;
        esps_enlp_frame_t frame;
        ESPS_CHECK_EQ(&fails, esps_enlp_parse(bad, len, &frame), ESPS_ENLP_ERR_BAD_CRC);
    }

    return fails;
}

static int test_cobs_wire_roundtrip(void) {
    int fails = 0;
    const uint8_t payload[] = {0x00, 0x11, 0x00, 0x22, 0x33, 0x00, 0x00, 0x44};
    uint8_t wire[ESPS_COBS_MAX_ENCODED(ESPS_ENLP_MAX_FRAME) + 1];
    size_t wire_len = esps_enlp_encode_cobs(ESPS_ENLP_VERSION, ESPS_MSG_TELEMETRY, 9, 3, payload,
                                             sizeof(payload), wire, sizeof(wire));
    ESPS_CHECK(&fails, wire_len > 0);
    ESPS_CHECK_EQ(&fails, wire[wire_len - 1], 0x00); /* trailing delimiter */

    for (size_t i = 0; i + 1 < wire_len; i++) {
        ESPS_CHECK(&fails, wire[i] != 0x00); /* no delimiter mid-body */
    }

    uint8_t decoded[ESPS_ENLP_MAX_FRAME];
    size_t decoded_len = 0;
    esps_cobs_err_t rc = esps_cobs_decode(wire, wire_len - 1, decoded, sizeof(decoded), &decoded_len);
    ESPS_CHECK_EQ(&fails, rc, ESPS_COBS_OK);

    esps_enlp_frame_t frame;
    ESPS_CHECK_EQ(&fails, esps_enlp_parse(decoded, decoded_len, &frame), ESPS_ENLP_OK);
    ESPS_CHECK_EQ(&fails, frame.type, ESPS_MSG_TELEMETRY);
    ESPS_CHECK_EQ(&fails, frame.node, 9);
    ESPS_CHECK_EQ(&fails, frame.seq, 3);
    ESPS_CHECK_EQ(&fails, frame.payload_len, sizeof(payload));
    ESPS_CHECK(&fails, memcmp(frame.payload, payload, sizeof(payload)) == 0);

    return fails;
}

/* --- packed message structs -------------------------------------------------- */

static int test_messages(void) {
    int fails = 0;

    {
        esps_heartbeat_t hb = {.uptime_ms = 123456, .heap_free = 45000, .heap_min = 30000,
                                .state = 2, .flags = ESPS_HEARTBEAT_FLAG_LINK_WAS_LOST, .rssi = -61};
        uint8_t buf[ESPS_HEARTBEAT_SIZE];
        size_t out_len = 0;
        ESPS_CHECK(&fails, esps_enlp_pack_heartbeat(&hb, buf, sizeof(buf), &out_len));
        ESPS_CHECK_EQ(&fails, out_len, ESPS_HEARTBEAT_SIZE);

        esps_heartbeat_t back;
        ESPS_CHECK(&fails, esps_enlp_unpack_heartbeat(buf, out_len, &back));
        ESPS_CHECK_EQ(&fails, back.uptime_ms, hb.uptime_ms);
        ESPS_CHECK_EQ(&fails, back.heap_free, hb.heap_free);
        ESPS_CHECK_EQ(&fails, back.heap_min, hb.heap_min);
        ESPS_CHECK_EQ(&fails, back.state, hb.state);
        ESPS_CHECK_EQ(&fails, back.flags, hb.flags);
        ESPS_CHECK_EQ(&fails, back.rssi, hb.rssi);

        /* Wrong size must be rejected, not partially parsed. */
        esps_heartbeat_t junk;
        ESPS_CHECK(&fails, !esps_enlp_unpack_heartbeat(buf, out_len - 1, &junk));
    }

    {
        esps_telem_ack_t ack = {.node = 4711, .last_seq = 6001, .flags = 0x0001};
        uint8_t buf[ESPS_TELEM_ACK_SIZE];
        size_t out_len = 0;
        ESPS_CHECK(&fails, esps_enlp_pack_telem_ack(&ack, buf, sizeof(buf), &out_len));
        esps_telem_ack_t back;
        ESPS_CHECK(&fails, esps_enlp_unpack_telem_ack(buf, out_len, &back));
        ESPS_CHECK_EQ(&fails, back.node, ack.node);
        ESPS_CHECK_EQ(&fails, back.last_seq, ack.last_seq);
        ESPS_CHECK_EQ(&fails, back.flags, ack.flags);
    }

    {
        esps_time_sync_t ts = {.t1_host_us = 1788573060412345ULL, .t2_node_ms = 91234,
                                .t3_node_ms = 91235};
        uint8_t buf[ESPS_TIME_SYNC_SIZE];
        size_t out_len = 0;
        ESPS_CHECK(&fails, esps_enlp_pack_time_sync(&ts, buf, sizeof(buf), &out_len));
        esps_time_sync_t back;
        ESPS_CHECK(&fails, esps_enlp_unpack_time_sync(buf, out_len, &back));
        ESPS_CHECK_EQ(&fails, back.t1_host_us, ts.t1_host_us);
        ESPS_CHECK_EQ(&fails, back.t2_node_ms, ts.t2_node_ms);
        ESPS_CHECK_EQ(&fails, back.t3_node_ms, ts.t3_node_ms);
    }

    {
        const char *tag = "wifi";
        const char *msg = "connected to AP";
        uint8_t buf[64];
        size_t out_len = 0;
        ESPS_CHECK(&fails, esps_enlp_pack_log(99000, 3, tag, strlen(tag), msg, strlen(msg), buf,
                                               sizeof(buf), &out_len));
        esps_log_view_t view;
        ESPS_CHECK(&fails, esps_enlp_unpack_log(buf, out_len, &view));
        ESPS_CHECK_EQ(&fails, view.ts_ms, 99000);
        ESPS_CHECK_EQ(&fails, view.level, 3);
        ESPS_CHECK_EQ(&fails, view.tag_len, strlen(tag));
        ESPS_CHECK(&fails, memcmp(view.tag, tag, view.tag_len) == 0);
        ESPS_CHECK_EQ(&fails, view.msg_len, strlen(msg));
        ESPS_CHECK(&fails, memcmp(view.msg, msg, view.msg_len) == 0);
    }

    return fails;
}

/* --- TELEMETRY sample walker -------------------------------------------------- */

static int test_telemetry(void) {
    int fails = 0;

    {
        uint8_t payload[256];
        esps_telemetry_builder_t b;
        esps_telemetry_builder_init(&b, payload, sizeof(payload));

        const uint8_t v_u8 = 200;
        const int8_t v_i8 = -50;
        const uint16_t v_u16 = 60000;
        const int16_t v_i16 = -12000;
        const uint32_t v_u32 = 4000000000u;
        const int32_t v_i32 = -2000000000;
        const uint8_t v_f32[4] = {0x00, 0x00, 0x80, 0x3F}; /* raw bytes, opaque to the codec */
        const uint8_t v_bool = 1;

        ESPS_CHECK(&fails, esps_telemetry_builder_add(&b, 1, 0, ESPS_ENC_U8, &v_u8));
        ESPS_CHECK(&fails, esps_telemetry_builder_add(&b, 2, 10, ESPS_ENC_I8, &v_i8));
        ESPS_CHECK(&fails, esps_telemetry_builder_add(&b, 3, 20, ESPS_ENC_U16, &v_u16));
        ESPS_CHECK(&fails, esps_telemetry_builder_add(&b, 4, 30, ESPS_ENC_I16, &v_i16));
        ESPS_CHECK(&fails, esps_telemetry_builder_add(&b, 5, 40, ESPS_ENC_U32, &v_u32));
        ESPS_CHECK(&fails, esps_telemetry_builder_add(&b, 6, 50, ESPS_ENC_I32, &v_i32));
        ESPS_CHECK(&fails, esps_telemetry_builder_add(&b, 7, 60, ESPS_ENC_F32, v_f32));
        ESPS_CHECK(&fails, esps_telemetry_builder_add(&b, 16, 70, ESPS_ENC_BOOL, &v_bool));

        size_t total_len = 0;
        ESPS_CHECK(&fails, esps_telemetry_builder_finish(&b, 500000, ESPS_TELEMETRY_FLAG_GAP_BEFORE,
                                                           &total_len));

        uint32_t base_ts = 0;
        uint8_t flags = 0, count = 0;
        esps_telemetry_reader_t r;
        ESPS_CHECK(&fails, esps_telemetry_reader_init(&r, payload, total_len, &base_ts, &flags, &count));
        ESPS_CHECK_EQ(&fails, base_ts, 500000);
        ESPS_CHECK_EQ(&fails, flags, ESPS_TELEMETRY_FLAG_GAP_BEFORE);
        ESPS_CHECK_EQ(&fails, count, 8);

        struct {
            uint8_t ch;
            uint16_t dt_ms;
            uint8_t enc;
            const void *value;
            size_t value_len;
        } expected[8] = {
            {1, 0, ESPS_ENC_U8, &v_u8, 1},   {2, 10, ESPS_ENC_I8, &v_i8, 1},
            {3, 20, ESPS_ENC_U16, &v_u16, 2}, {4, 30, ESPS_ENC_I16, &v_i16, 2},
            {5, 40, ESPS_ENC_U32, &v_u32, 4}, {6, 50, ESPS_ENC_I32, &v_i32, 4},
            {7, 60, ESPS_ENC_F32, v_f32, 4},  {16, 70, ESPS_ENC_BOOL, &v_bool, 1},
        };

        for (int i = 0; i < 8; i++) {
            esps_telemetry_sample_t s;
            ESPS_CHECK(&fails, esps_telemetry_reader_next(&r, &s));
            ESPS_CHECK_EQ(&fails, s.ch, expected[i].ch);
            ESPS_CHECK_EQ(&fails, s.dt_ms, expected[i].dt_ms);
            ESPS_CHECK_EQ(&fails, s.enc, expected[i].enc);
            ESPS_CHECK_EQ(&fails, s.value_len, expected[i].value_len);
            ESPS_CHECK(&fails, memcmp(s.value, expected[i].value, expected[i].value_len) == 0);
        }
        esps_telemetry_sample_t extra;
        ESPS_CHECK(&fails, !esps_telemetry_reader_next(&r, &extra)); /* exhausted */
    }

    /* Max-count (64) batch, and rejection of a 65th sample. */
    {
        uint8_t payload[64 * 8 + 8];
        esps_telemetry_builder_t b;
        esps_telemetry_builder_init(&b, payload, sizeof(payload));
        for (int i = 0; i < 64; i++) {
            uint8_t v = (uint8_t)i;
            ESPS_CHECK(&fails, esps_telemetry_builder_add(&b, (uint8_t)i, (uint16_t)i, ESPS_ENC_U8, &v));
        }
        uint8_t v65 = 65;
        ESPS_CHECK(&fails, !esps_telemetry_builder_add(&b, 65, 65, ESPS_ENC_U8, &v65));

        size_t total_len = 0;
        ESPS_CHECK(&fails, esps_telemetry_builder_finish(&b, 0, 0, &total_len));

        uint8_t count = 0;
        esps_telemetry_reader_t r;
        ESPS_CHECK(&fails, esps_telemetry_reader_init(&r, payload, total_len, NULL, NULL, &count));
        ESPS_CHECK_EQ(&fails, count, 64);
        int n = 0;
        esps_telemetry_sample_t s;
        while (esps_telemetry_reader_next(&r, &s)) {
            n++;
        }
        ESPS_CHECK_EQ(&fails, n, 64);
    }

    /* count == 0 is invalid per PROTOCOL.md (1..64). */
    {
        uint8_t payload[ESPS_TELEMETRY_HEADER_SIZE] = {0, 0, 0, 0, 0, 0};
        esps_telemetry_reader_t r;
        uint8_t count = 99;
        ESPS_CHECK(&fails, !esps_telemetry_reader_init(&r, payload, sizeof(payload), NULL, NULL, &count));
    }

    return fails;
}

/* --- streaming serial decoder ------------------------------------------------- */

typedef struct {
    int frame_count;
    uint8_t last_type;
    uint16_t last_node;
    uint16_t last_seq;
    uint8_t last_payload[64];
    size_t last_payload_len;
    int raw_count;
    uint8_t last_raw[128];
    size_t last_raw_len;
} stream_ctx_t;

static void on_frame_cb(const esps_enlp_frame_t *f, void *ctx) {
    stream_ctx_t *c = (stream_ctx_t *)ctx;
    c->frame_count++;
    c->last_type = f->type;
    c->last_node = f->node;
    c->last_seq = f->seq;
    c->last_payload_len = f->payload_len;
    if (f->payload_len > 0 && f->payload_len <= sizeof(c->last_payload)) {
        memcpy(c->last_payload, f->payload, f->payload_len);
    }
}

static void on_raw_cb(const uint8_t *data, size_t len, void *ctx) {
    stream_ctx_t *c = (stream_ctx_t *)ctx;
    c->raw_count++;
    c->last_raw_len = len;
    if (len <= sizeof(c->last_raw)) {
        memcpy(c->last_raw, data, len);
    }
}

static int test_stream_split_at_every_offset(void) {
    int fails = 0;
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t wire[64];
    size_t wire_len = esps_enlp_encode_cobs(ESPS_ENLP_VERSION, ESPS_MSG_HEARTBEAT, 55, 9, payload,
                                             sizeof(payload), wire, sizeof(wire));
    ESPS_CHECK(&fails, wire_len > 0);

    for (size_t split = 0; split <= wire_len; split++) {
        stream_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        esps_enlp_stream_t s;
        esps_enlp_stream_init(&s, on_frame_cb, on_raw_cb, &ctx);

        esps_enlp_stream_feed(&s, wire, split);
        esps_enlp_stream_feed(&s, wire + split, wire_len - split);

        ESPS_CHECK_EQ(&fails, ctx.frame_count, 1);
        ESPS_CHECK_EQ(&fails, ctx.raw_count, 0);
        if (ctx.frame_count == 1) {
            ESPS_CHECK_EQ(&fails, ctx.last_type, ESPS_MSG_HEARTBEAT);
            ESPS_CHECK_EQ(&fails, ctx.last_node, 55);
            ESPS_CHECK_EQ(&fails, ctx.last_seq, 9);
            ESPS_CHECK_EQ(&fails, ctx.last_payload_len, sizeof(payload));
            ESPS_CHECK(&fails, memcmp(ctx.last_payload, payload, sizeof(payload)) == 0);
        }
    }
    return fails;
}

static int test_stream_two_frames_one_chunk(void) {
    int fails = 0;
    const uint8_t p1[] = {0xAA, 0xBB};
    const uint8_t p2[] = {0xCC, 0xDD, 0xEE};
    uint8_t buf[128];
    size_t len1 = esps_enlp_encode_cobs(ESPS_ENLP_VERSION, ESPS_MSG_LOG, 1, 100, p1, sizeof(p1),
                                         buf, sizeof(buf));
    size_t len2 = esps_enlp_encode_cobs(ESPS_ENLP_VERSION, ESPS_MSG_EVENT, 2, 200, p2, sizeof(p2),
                                         buf + len1, sizeof(buf) - len1);
    ESPS_CHECK(&fails, len1 > 0 && len2 > 0);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    esps_enlp_stream_t s;
    esps_enlp_stream_init(&s, on_frame_cb, on_raw_cb, &ctx);
    esps_enlp_stream_feed(&s, buf, len1 + len2);

    ESPS_CHECK_EQ(&fails, ctx.frame_count, 2);
    ESPS_CHECK_EQ(&fails, ctx.raw_count, 0);
    /* Only the last frame's fields survive in ctx; verify it is frame 2. */
    ESPS_CHECK_EQ(&fails, ctx.last_type, ESPS_MSG_EVENT);
    ESPS_CHECK_EQ(&fails, ctx.last_node, 2);
    ESPS_CHECK_EQ(&fails, ctx.last_seq, 200);
    ESPS_CHECK_EQ(&fails, ctx.last_payload_len, sizeof(p2));
    ESPS_CHECK(&fails, memcmp(ctx.last_payload, p2, sizeof(p2)) == 0);

    return fails;
}

static int test_stream_garbage_then_frame(void) {
    int fails = 0;
    /* Boot-ROM-style ASCII text, terminated by a delimiter it did not intend
     * as one — this is exactly the PROTOCOL.md S2.1 scenario. */
    const uint8_t garbage[] = "rst:0x1 (POWERON),boot:0x13\r\n";
    size_t garbage_len = sizeof(garbage) - 1;

    const uint8_t payload[] = {9, 9, 9};
    uint8_t frame_wire[64];
    size_t frame_len = esps_enlp_encode_cobs(ESPS_ENLP_VERSION, ESPS_MSG_HELLO, 3, 1, payload,
                                              sizeof(payload), frame_wire, sizeof(frame_wire));
    ESPS_CHECK(&fails, frame_len > 0);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    esps_enlp_stream_t s;
    esps_enlp_stream_init(&s, on_frame_cb, on_raw_cb, &ctx);

    esps_enlp_stream_feed(&s, garbage, garbage_len);
    esps_enlp_stream_feed(&s, (const uint8_t *)"\x00", 1); /* delimiter flushes the garbage */
    esps_enlp_stream_feed(&s, frame_wire, frame_len);

    ESPS_CHECK_EQ(&fails, ctx.raw_count, 1);
    ESPS_CHECK_EQ(&fails, ctx.last_raw_len, garbage_len);
    ESPS_CHECK(&fails, memcmp(ctx.last_raw, garbage, garbage_len) == 0);

    ESPS_CHECK_EQ(&fails, ctx.frame_count, 1);
    ESPS_CHECK_EQ(&fails, ctx.last_type, ESPS_MSG_HELLO);
    ESPS_CHECK_EQ(&fails, ctx.last_payload_len, sizeof(payload));
    ESPS_CHECK(&fails, memcmp(ctx.last_payload, payload, sizeof(payload)) == 0);

    return fails;
}

static int test_stream_interrupted_by_resync(void) {
    int fails = 0;
    /* A payload long enough that splitting it mid-block guarantees the COBS
     * decode of the truncated half fails (rather than happening to still be
     * valid COBS by coincidence). */
    uint8_t payload[20];
    for (int i = 0; i < 20; i++) {
        payload[i] = (uint8_t)(i + 1); /* no zero bytes -> one long data block */
    }
    uint8_t wire[64];
    size_t wire_len = esps_enlp_encode_cobs(ESPS_ENLP_VERSION, ESPS_MSG_TELEMETRY, 5, 2, payload,
                                             sizeof(payload), wire, sizeof(wire));
    ESPS_CHECK(&fails, wire_len > 10);

    stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    esps_enlp_stream_t s;
    esps_enlp_stream_init(&s, on_frame_cb, on_raw_cb, &ctx);

    /* Feed roughly half of the encoded body (excluding the delimiter), then
     * a stray resync delimiter — this must surface as raw, not silently
     * vanish and not desync the decoder for what follows. */
    size_t half = (wire_len - 1) / 2;
    esps_enlp_stream_feed(&s, wire, half);
    esps_enlp_stream_feed(&s, (const uint8_t *)"\x00", 1);

    ESPS_CHECK_EQ(&fails, ctx.raw_count, 1);
    ESPS_CHECK_EQ(&fails, ctx.frame_count, 0);

    /* A fresh, complete frame afterwards must parse normally. */
    const uint8_t p2[] = {7, 8};
    uint8_t wire2[32];
    size_t wire2_len = esps_enlp_encode_cobs(ESPS_ENLP_VERSION, ESPS_MSG_HEARTBEAT, 6, 3, p2,
                                              sizeof(p2), wire2, sizeof(wire2));
    esps_enlp_stream_feed(&s, wire2, wire2_len);

    ESPS_CHECK_EQ(&fails, ctx.raw_count, 1); /* unchanged */
    ESPS_CHECK_EQ(&fails, ctx.frame_count, 1);
    ESPS_CHECK_EQ(&fails, ctx.last_type, ESPS_MSG_HEARTBEAT);
    ESPS_CHECK_EQ(&fails, ctx.last_payload_len, sizeof(p2));
    ESPS_CHECK(&fails, memcmp(ctx.last_payload, p2, sizeof(p2)) == 0);

    return fails;
}

int test_enlp_all(void) {
    int fails = 0;
    fails += test_frame_roundtrip_all_types();
    fails += test_parse_errors();
    fails += test_cobs_wire_roundtrip();
    fails += test_messages();
    fails += test_telemetry();
    fails += test_stream_split_at_every_offset();
    fails += test_stream_two_frames_one_chunk();
    fails += test_stream_garbage_then_frame();
    fails += test_stream_interrupted_by_resync();
    return fails;
}

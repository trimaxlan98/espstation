/* ENLP frame codec implementation. See esps_enlp.h for the API rationale.
 *
 * All multi-byte fields are little-endian (PROTOCOL.md S3) regardless of
 * host/target byte order, so every field is written/read one byte at a time
 * rather than through a typed pointer.
 */
#include "esps_enlp.h"
#include "esps_cobs.h"
#include "esps_crc16.h"

#include <string.h>

/* --- little-endian helpers ------------------------------------------------ */

static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static void put_u64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
    }
}
static uint16_t get_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* --- frame build/parse ----------------------------------------------------- */

size_t esps_enlp_encode(uint8_t ver, uint8_t type, uint16_t node, uint16_t seq,
                         const uint8_t *payload, size_t payload_len,
                         uint8_t *dst, size_t dst_cap) {
    if (payload_len > ESPS_ENLP_MAX_PAYLOAD) {
        return 0;
    }
    size_t total = ESPS_ENLP_HEADER_SIZE + payload_len + ESPS_ENLP_CRC_SIZE;
    if (dst_cap < total) {
        return 0;
    }

    dst[0] = ver;
    dst[1] = type;
    put_u16le(&dst[2], node);
    put_u16le(&dst[4], seq);
    put_u16le(&dst[6], (uint16_t)payload_len);
    if (payload_len > 0) {
        memcpy(&dst[ESPS_ENLP_HEADER_SIZE], payload, payload_len);
    }

    uint16_t crc = esps_crc16(dst, ESPS_ENLP_HEADER_SIZE + payload_len);
    put_u16le(&dst[ESPS_ENLP_HEADER_SIZE + payload_len], crc);

    return total;
}

size_t esps_enlp_encode_cobs(uint8_t ver, uint8_t type, uint16_t node, uint16_t seq,
                              const uint8_t *payload, size_t payload_len,
                              uint8_t *dst, size_t dst_cap) {
    uint8_t raw[ESPS_ENLP_MAX_FRAME];
    size_t raw_len = esps_enlp_encode(ver, type, node, seq, payload, payload_len, raw, sizeof(raw));
    if (raw_len == 0) {
        return 0;
    }

    size_t enc_len = 0;
    if (esps_cobs_encode(raw, raw_len, dst, dst_cap, &enc_len) != ESPS_COBS_OK) {
        return 0;
    }
    if (enc_len >= dst_cap) {
        return 0; /* no room left for the trailing delimiter */
    }
    dst[enc_len] = 0x00;
    return enc_len + 1;
}

esps_enlp_err_t esps_enlp_parse(const uint8_t *buf, size_t len, esps_enlp_frame_t *out) {
    if (len < ESPS_ENLP_HEADER_SIZE) {
        return ESPS_ENLP_ERR_TOO_SHORT;
    }
    uint8_t ver = buf[0];
    if (ver != ESPS_ENLP_VERSION) {
        return ESPS_ENLP_ERR_BAD_VERSION;
    }

    uint16_t plen = get_u16le(&buf[6]);
    if (plen > ESPS_ENLP_MAX_PAYLOAD) {
        return ESPS_ENLP_ERR_BAD_LENGTH;
    }
    size_t expected = ESPS_ENLP_HEADER_SIZE + (size_t)plen + ESPS_ENLP_CRC_SIZE;
    if (len != expected) {
        return ESPS_ENLP_ERR_BAD_LENGTH;
    }

    uint16_t crc_calc = esps_crc16(buf, ESPS_ENLP_HEADER_SIZE + plen);
    uint16_t crc_recv = get_u16le(&buf[ESPS_ENLP_HEADER_SIZE + plen]);
    if (crc_calc != crc_recv) {
        return ESPS_ENLP_ERR_BAD_CRC;
    }

    out->ver = ver;
    out->type = buf[1];
    out->node = get_u16le(&buf[2]);
    out->seq = get_u16le(&buf[4]);
    out->payload = (plen > 0) ? &buf[ESPS_ENLP_HEADER_SIZE] : NULL;
    out->payload_len = plen;
    return ESPS_ENLP_OK;
}

/* --- streaming serial decoder ---------------------------------------------- */

void esps_enlp_stream_init(esps_enlp_stream_t *s, esps_enlp_on_frame_cb on_frame,
                            esps_enlp_on_raw_cb on_raw, void *ctx) {
    s->raw_len = 0;
    s->on_frame = on_frame;
    s->on_raw = on_raw;
    s->ctx = ctx;
}

static void stream_flush_segment(esps_enlp_stream_t *s) {
    if (s->raw_len == 0) {
        /* Two delimiters back-to-back: normal COBS resync flush, not data. */
        return;
    }

    size_t decoded_len = 0;
    esps_cobs_err_t crc_rc = esps_cobs_decode(s->raw_buf, s->raw_len, s->decode_buf,
                                               sizeof(s->decode_buf), &decoded_len);
    if (crc_rc != ESPS_COBS_OK) {
        if (s->on_raw) {
            s->on_raw(s->raw_buf, s->raw_len, s->ctx);
        }
        s->raw_len = 0;
        return;
    }

    esps_enlp_frame_t frame;
    if (esps_enlp_parse(s->decode_buf, decoded_len, &frame) != ESPS_ENLP_OK) {
        if (s->on_raw) {
            s->on_raw(s->raw_buf, s->raw_len, s->ctx);
        }
    } else if (s->on_frame) {
        s->on_frame(&frame, s->ctx);
    }
    s->raw_len = 0;
}

void esps_enlp_stream_feed(esps_enlp_stream_t *s, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == 0x00) {
            stream_flush_segment(s);
        } else {
            if (s->raw_len >= sizeof(s->raw_buf)) {
                /* Longer than any valid frame could ever encode to — this
                 * can only be noise; flush it as raw and keep resyncing
                 * rather than growing without bound. */
                if (s->on_raw) {
                    s->on_raw(s->raw_buf, s->raw_len, s->ctx);
                }
                s->raw_len = 0;
            }
            s->raw_buf[s->raw_len++] = b;
        }
    }
}

/* --- HEARTBEAT -------------------------------------------------------------- */

bool esps_enlp_pack_heartbeat(const esps_heartbeat_t *hb, uint8_t *buf, size_t cap, size_t *out_len) {
    if (cap < ESPS_HEARTBEAT_SIZE) {
        return false;
    }
    put_u32le(&buf[0], hb->uptime_ms);
    put_u32le(&buf[4], hb->heap_free);
    put_u32le(&buf[8], hb->heap_min);
    buf[12] = hb->state;
    buf[13] = hb->flags;
    buf[14] = (uint8_t)hb->rssi;
    buf[15] = 0; /* reserved */
    *out_len = ESPS_HEARTBEAT_SIZE;
    return true;
}

bool esps_enlp_unpack_heartbeat(const uint8_t *buf, size_t len, esps_heartbeat_t *out) {
    if (len != ESPS_HEARTBEAT_SIZE) {
        return false;
    }
    out->uptime_ms = get_u32le(&buf[0]);
    out->heap_free = get_u32le(&buf[4]);
    out->heap_min = get_u32le(&buf[8]);
    out->state = buf[12];
    out->flags = buf[13];
    out->rssi = (int8_t)buf[14];
    return true;
}

/* --- TELEM_ACK --------------------------------------------------------------- */

bool esps_enlp_pack_telem_ack(const esps_telem_ack_t *ack, uint8_t *buf, size_t cap, size_t *out_len) {
    if (cap < ESPS_TELEM_ACK_SIZE) {
        return false;
    }
    put_u16le(&buf[0], ack->node);
    put_u16le(&buf[2], ack->last_seq);
    put_u16le(&buf[4], ack->flags);
    *out_len = ESPS_TELEM_ACK_SIZE;
    return true;
}

bool esps_enlp_unpack_telem_ack(const uint8_t *buf, size_t len, esps_telem_ack_t *out) {
    if (len != ESPS_TELEM_ACK_SIZE) {
        return false;
    }
    out->node = get_u16le(&buf[0]);
    out->last_seq = get_u16le(&buf[2]);
    out->flags = get_u16le(&buf[4]);
    return true;
}

/* --- TIME_SYNC --------------------------------------------------------------- */

bool esps_enlp_pack_time_sync(const esps_time_sync_t *ts, uint8_t *buf, size_t cap, size_t *out_len) {
    if (cap < ESPS_TIME_SYNC_SIZE) {
        return false;
    }
    put_u64le(&buf[0], ts->t1_host_us);
    put_u32le(&buf[8], ts->t2_node_ms);
    put_u32le(&buf[12], ts->t3_node_ms);
    put_u64le(&buf[16], 0); /* reserved */
    *out_len = ESPS_TIME_SYNC_SIZE;
    return true;
}

bool esps_enlp_unpack_time_sync(const uint8_t *buf, size_t len, esps_time_sync_t *out) {
    if (len != ESPS_TIME_SYNC_SIZE) {
        return false;
    }
    out->t1_host_us = get_u64le(&buf[0]);
    out->t2_node_ms = get_u32le(&buf[8]);
    out->t3_node_ms = get_u32le(&buf[12]);
    return true;
}

/* --- LOG ----------------------------------------------------------------------- */

bool esps_enlp_pack_log(uint32_t ts_ms, uint8_t level,
                         const char *tag, size_t tag_len,
                         const char *msg, size_t msg_len,
                         uint8_t *buf, size_t cap, size_t *out_len) {
    if (tag_len > 0xFFu) {
        return false; /* tag_len is a single byte on the wire */
    }
    size_t total = ESPS_LOG_HEADER_SIZE + tag_len + msg_len;
    if (total > cap) {
        return false;
    }
    put_u32le(&buf[0], ts_ms);
    buf[4] = level;
    buf[5] = (uint8_t)tag_len;
    if (tag_len > 0) {
        memcpy(&buf[ESPS_LOG_HEADER_SIZE], tag, tag_len);
    }
    if (msg_len > 0) {
        memcpy(&buf[ESPS_LOG_HEADER_SIZE + tag_len], msg, msg_len);
    }
    *out_len = total;
    return true;
}

bool esps_enlp_unpack_log(const uint8_t *buf, size_t len, esps_log_view_t *out) {
    if (len < ESPS_LOG_HEADER_SIZE) {
        return false;
    }
    uint8_t tag_len = buf[5];
    if ((size_t)ESPS_LOG_HEADER_SIZE + tag_len > len) {
        return false;
    }
    out->ts_ms = get_u32le(&buf[0]);
    out->level = buf[4];
    out->tag = (const char *)&buf[ESPS_LOG_HEADER_SIZE];
    out->tag_len = tag_len;
    out->msg = (const char *)&buf[ESPS_LOG_HEADER_SIZE + tag_len];
    out->msg_len = len - ESPS_LOG_HEADER_SIZE - tag_len;
    return true;
}

/* --- TELEMETRY ------------------------------------------------------------------ */

int esps_enlp_enc_size(uint8_t enc) {
    static const int8_t sizes[8] = {1, 1, 2, 2, 4, 4, 4, 1};
    if (enc >= 8) {
        return -1;
    }
    return sizes[enc];
}

void esps_telemetry_builder_init(esps_telemetry_builder_t *b, uint8_t *buf, size_t cap) {
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->count = 0;
}

bool esps_telemetry_builder_add(esps_telemetry_builder_t *b, uint8_t ch, uint16_t dt_ms,
                                 uint8_t enc, const void *value) {
    if (b->count >= ESPS_TELEMETRY_MAX_COUNT) {
        return false; /* batch full — not an error, caller should finish() what it has */
    }
    int vsize = esps_enlp_enc_size(enc);
    if (vsize < 0) {
        return false;
    }
    size_t sample_size = ESPS_TELEMETRY_SAMPLE_HEADER_SIZE + 1u /* enc byte */ + (size_t)vsize;
    size_t needed = ESPS_TELEMETRY_HEADER_SIZE + b->len + sample_size;
    if (needed > b->cap) {
        return false;
    }

    uint8_t *p = &b->buf[ESPS_TELEMETRY_HEADER_SIZE + b->len];
    p[0] = ch;
    put_u16le(&p[1], dt_ms);
    p[3] = enc;
    memcpy(&p[4], value, (size_t)vsize);

    b->len += sample_size;
    b->count++;
    return true;
}

bool esps_telemetry_builder_finish(esps_telemetry_builder_t *b, uint32_t base_ts_ms,
                                    uint8_t flags, size_t *out_len) {
    if (b->count == 0) {
        return false; /* PROTOCOL.md S4.4: count is 1..64, never 0 */
    }
    if (ESPS_TELEMETRY_HEADER_SIZE > b->cap) {
        return false;
    }
    put_u32le(&b->buf[0], base_ts_ms);
    b->buf[4] = b->count;
    b->buf[5] = flags;
    *out_len = ESPS_TELEMETRY_HEADER_SIZE + b->len;
    return true;
}

bool esps_telemetry_reader_init(esps_telemetry_reader_t *r, const uint8_t *payload, size_t len,
                                 uint32_t *base_ts_ms, uint8_t *flags, uint8_t *count) {
    if (len < ESPS_TELEMETRY_HEADER_SIZE) {
        return false;
    }
    uint8_t c = payload[4];
    if (c < 1 || c > ESPS_TELEMETRY_MAX_COUNT) {
        return false;
    }
    r->payload = payload;
    r->len = len;
    r->pos = ESPS_TELEMETRY_HEADER_SIZE;
    r->count = c;
    r->idx = 0;

    if (base_ts_ms) {
        *base_ts_ms = get_u32le(&payload[0]);
    }
    if (flags) {
        *flags = payload[5];
    }
    if (count) {
        *count = c;
    }
    return true;
}

bool esps_telemetry_reader_next(esps_telemetry_reader_t *r, esps_telemetry_sample_t *out) {
    if (r->idx >= r->count) {
        return false;
    }
    if (r->pos + ESPS_TELEMETRY_SAMPLE_HEADER_SIZE + 1u > r->len) {
        return false; /* malformed: not enough bytes left for ch+dt_ms+enc */
    }
    const uint8_t *p = &r->payload[r->pos];
    uint8_t ch = p[0];
    uint16_t dt_ms = get_u16le(&p[1]);
    uint8_t enc = p[3];
    int vsize = esps_enlp_enc_size(enc);
    if (vsize < 0) {
        return false; /* malformed: unknown enc */
    }
    size_t sample_size = ESPS_TELEMETRY_SAMPLE_HEADER_SIZE + 1u + (size_t)vsize;
    if (r->pos + sample_size > r->len) {
        return false; /* malformed: value would read past payload */
    }

    out->ch = ch;
    out->dt_ms = dt_ms;
    out->enc = enc;
    out->value = &p[4];
    out->value_len = (size_t)vsize;

    r->pos += sample_size;
    r->idx++;
    return true;
}

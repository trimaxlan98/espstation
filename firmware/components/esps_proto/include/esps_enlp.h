/* ENLP frame codec — PROTOCOL.md S3-4. Pure C11, no ESP-IDF dependency (see
 * esps_crc16.h / esps_cobs.h for why). Every struct here is either a *view*
 * into a caller-owned buffer (no copies, no allocation) or packed/unpacked
 * with explicit byte reads and writes — never by casting a struct pointer
 * onto wire bytes, because host and target disagree on alignment and padding
 * for e.g. { uint32_t; uint8_t; uint8_t; int8_t }.
 */
#ifndef ESPS_ENLP_H
#define ESPS_ENLP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esps_cobs.h" /* ESPS_COBS_MAX_ENCODED, used by ESPS_ENLP_STREAM_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Frame body geometry (PROTOCOL.md S3) -------------------------------- */

#define ESPS_ENLP_VERSION      1u
#define ESPS_ENLP_HEADER_SIZE  8u
#define ESPS_ENLP_CRC_SIZE     2u
#define ESPS_ENLP_MAX_PAYLOAD  1024u /* serial/TCP; ESP-NOW uses 240 (esps_link) */
#define ESPS_ENLP_MAX_FRAME    (ESPS_ENLP_HEADER_SIZE + ESPS_ENLP_MAX_PAYLOAD + ESPS_ENLP_CRC_SIZE)

/* --- Message types (PROTOCOL.md S4 / espstation.protocol.yaml) ----------- */

#define ESPS_MSG_HELLO       0x01u
#define ESPS_MSG_HELLO_ACK   0x02u
#define ESPS_MSG_HEARTBEAT   0x03u
#define ESPS_MSG_TELEMETRY   0x10u
#define ESPS_MSG_TELEM_ACK   0x11u
#define ESPS_MSG_LOG         0x20u
#define ESPS_MSG_EVENT       0x21u
#define ESPS_MSG_CMD         0x30u
#define ESPS_MSG_CMD_ACK     0x31u
#define ESPS_MSG_EXP_SET     0x40u
#define ESPS_MSG_EXP_STATE   0x41u
#define ESPS_MSG_BULK_BEGIN  0x50u
#define ESPS_MSG_BULK_CHUNK  0x51u
#define ESPS_MSG_BULK_END    0x52u
#define ESPS_MSG_NET_REPORT  0x60u
#define ESPS_MSG_NET_CMD     0x61u
#define ESPS_MSG_TIME_SYNC   0x70u

/* --- Frame build/parse ---------------------------------------------------- */

typedef enum {
    ESPS_ENLP_OK = 0,
    ESPS_ENLP_ERR_TOO_SHORT,   /* fewer than ESPS_ENLP_HEADER_SIZE bytes: can't even read `len` */
    ESPS_ENLP_ERR_BAD_VERSION, /* ver != ESPS_ENLP_VERSION */
    ESPS_ENLP_ERR_BAD_LENGTH,  /* declared len > MAX_PAYLOAD, or buffer size != header+len+crc */
    ESPS_ENLP_ERR_BAD_CRC,     /* CRC-16 mismatch over [0, 8+len) */
} esps_enlp_err_t;

/* A parsed frame is a view into the buffer esps_enlp_parse() was given —
 * payload is never copied. Valid only as long as that buffer is. */
typedef struct {
    uint8_t ver;
    uint8_t type;
    uint16_t node;
    uint16_t seq;
    const uint8_t *payload;
    size_t payload_len;
} esps_enlp_frame_t;

/* Builds header + payload + CRC16 into dst. Returns the total frame length,
 * or 0 if payload_len exceeds ESPS_ENLP_MAX_PAYLOAD or dst_cap is too small. */
size_t esps_enlp_encode(uint8_t ver, uint8_t type, uint16_t node, uint16_t seq,
                         const uint8_t *payload, size_t payload_len,
                         uint8_t *dst, size_t dst_cap);

/* Builds the frame, COBS-encodes it and appends the 0x00 delimiter — the
 * serial wire form (PROTOCOL.md S2.1). Returns total bytes written to dst
 * (including the delimiter), or 0 on failure. Uses an internal MAX_FRAME
 * scratch buffer on the stack; no heap involved. */
size_t esps_enlp_encode_cobs(uint8_t ver, uint8_t type, uint16_t node, uint16_t seq,
                              const uint8_t *payload, size_t payload_len,
                              uint8_t *dst, size_t dst_cap);

/* Parses a raw (already COBS-decoded, delimiter-stripped) frame body.
 * The four error cases are kept distinct — see esps_enlp_err_t — because
 * the gateway acts differently on each (a version mismatch is a fatal
 * incompatibility; a bad CRC is routine serial noise). */
esps_enlp_err_t esps_enlp_parse(const uint8_t *buf, size_t len, esps_enlp_frame_t *out);

/* --- Streaming serial decoder --------------------------------------------- */

/* Max bytes a single COBS-encoded frame can occupy on the wire between two
 * 0x00 delimiters; sizes the stream decoder's resync buffer. */
#define ESPS_ENLP_STREAM_MAX ESPS_COBS_MAX_ENCODED(ESPS_ENLP_MAX_FRAME)

/* Called once per successfully decoded+validated frame. `frame` (and the
 * buffer it points into) is valid only for the duration of the callback. */
typedef void (*esps_enlp_on_frame_cb)(const esps_enlp_frame_t *frame, void *ctx);

/* Called for any delimited segment that failed to COBS-decode or failed
 * esps_enlp_parse (bad version/length/CRC included) — PROTOCOL.md S2.1 makes
 * this mandatory, not optional, so boot-ROM text and any pre-link printf
 * reach the station instead of vanishing. `data`/`len` are the raw bytes as
 * received (still COBS-encoded), valid only for the duration of the callback. */
typedef void (*esps_enlp_on_raw_cb)(const uint8_t *data, size_t len, void *ctx);

typedef struct {
    uint8_t raw_buf[ESPS_ENLP_STREAM_MAX];
    size_t raw_len;
    uint8_t decode_buf[ESPS_ENLP_MAX_FRAME];
    esps_enlp_on_frame_cb on_frame;
    esps_enlp_on_raw_cb on_raw;
    void *ctx;
} esps_enlp_stream_t;

void esps_enlp_stream_init(esps_enlp_stream_t *s, esps_enlp_on_frame_cb on_frame,
                            esps_enlp_on_raw_cb on_raw, void *ctx);

/* Feed an arbitrary chunk of bytes (any split point, including mid-frame).
 * State persists across calls; a 0x00 always resynchronises. */
void esps_enlp_stream_feed(esps_enlp_stream_t *s, const uint8_t *data, size_t len);

/* --- Packed message structs (PROTOCOL.md S4.3/4/5/10/11) ------------------ */

#define ESPS_HEARTBEAT_SIZE 16u

typedef struct {
    uint32_t uptime_ms;
    uint32_t heap_free;
    uint32_t heap_min;
    uint8_t state;   /* node_state enum: 0 boot,1 idle,2 running,3 degraded,4 safe */
    uint8_t flags;   /* bit0 buffered_pending, bit1 link_was_lost, bit2 brownout_since_boot, bit3 watchdog_reset */
    int8_t rssi;     /* dBm, 0 if no radio link */
} esps_heartbeat_t;

#define ESPS_HEARTBEAT_FLAG_BUFFERED_PENDING      (1u << 0)
#define ESPS_HEARTBEAT_FLAG_LINK_WAS_LOST         (1u << 1)
#define ESPS_HEARTBEAT_FLAG_BROWNOUT_SINCE_BOOT   (1u << 2)
#define ESPS_HEARTBEAT_FLAG_WATCHDOG_RESET        (1u << 3)

bool esps_enlp_pack_heartbeat(const esps_heartbeat_t *hb, uint8_t *buf, size_t cap, size_t *out_len);
bool esps_enlp_unpack_heartbeat(const uint8_t *buf, size_t len, esps_heartbeat_t *out);

#define ESPS_TELEM_ACK_SIZE 6u

typedef struct {
    uint16_t node;
    uint16_t last_seq;
    uint16_t flags;
} esps_telem_ack_t;

bool esps_enlp_pack_telem_ack(const esps_telem_ack_t *ack, uint8_t *buf, size_t cap, size_t *out_len);
bool esps_enlp_unpack_telem_ack(const uint8_t *buf, size_t len, esps_telem_ack_t *out);

#define ESPS_TIME_SYNC_SIZE 24u

typedef struct {
    uint64_t t1_host_us; /* station send time, us since Unix epoch */
    uint32_t t2_node_ms; /* node receive time, monotonic ms */
    uint32_t t3_node_ms; /* node send time, monotonic ms */
} esps_time_sync_t;

bool esps_enlp_pack_time_sync(const esps_time_sync_t *ts, uint8_t *buf, size_t cap, size_t *out_len);
bool esps_enlp_unpack_time_sync(const uint8_t *buf, size_t len, esps_time_sync_t *out);

/* LOG (PROTOCOL.md S4.5): fixed 6-byte header, then `tag` (tag_len bytes),
 * then `msg` filling the rest of the payload. tag/msg are UTF-8, not
 * NUL-terminated on the wire, so unpack yields pointer+length views rather
 * than C strings. */
#define ESPS_LOG_HEADER_SIZE 6u

typedef struct {
    uint32_t ts_ms;
    uint8_t level; /* ESP-IDF esp_log_level_t order: 0 none..5 verbose */
    const char *tag;
    size_t tag_len;
    const char *msg;
    size_t msg_len;
} esps_log_view_t;

bool esps_enlp_pack_log(uint32_t ts_ms, uint8_t level,
                         const char *tag, size_t tag_len,
                         const char *msg, size_t msg_len,
                         uint8_t *buf, size_t cap, size_t *out_len);
bool esps_enlp_unpack_log(const uint8_t *buf, size_t len, esps_log_view_t *out);

/* --- TELEMETRY (PROTOCOL.md S4.4) ----------------------------------------- */

#define ESPS_TELEMETRY_HEADER_SIZE 6u
#define ESPS_TELEMETRY_MAX_COUNT   64u
#define ESPS_TELEMETRY_SAMPLE_HEADER_SIZE 3u /* ch(1) + dt_ms(2); enc+value follow */

#define ESPS_TELEMETRY_FLAG_REPLAY     (1u << 0)
#define ESPS_TELEMETRY_FLAG_GAP_BEFORE (1u << 1)

#define ESPS_ENC_U8   0u
#define ESPS_ENC_I8   1u
#define ESPS_ENC_U16  2u
#define ESPS_ENC_I16  3u
#define ESPS_ENC_U32  4u
#define ESPS_ENC_I32  5u
#define ESPS_ENC_F32  6u
#define ESPS_ENC_BOOL 7u

/* Size in bytes of one `enc` value, or -1 if enc is not one of the 8 known
 * encodings (ESPS_ENC_*). */
int esps_enlp_enc_size(uint8_t enc);

/* Builds a TELEMETRY payload incrementally: init, add() per sample (up to
 * ESPS_TELEMETRY_MAX_COUNT), finish() writes the 6-byte header and returns
 * the total payload length. A rejected add() (batch full, buffer full, or
 * unknown enc) is not sticky — the caller is expected to keep the samples it
 * already has and finish() the batch anyway, which is the normal way a
 * telemetry task fills a frame up to capacity. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;   /* bytes written after the header so far */
    uint8_t count;
} esps_telemetry_builder_t;

void esps_telemetry_builder_init(esps_telemetry_builder_t *b, uint8_t *buf, size_t cap);
bool esps_telemetry_builder_add(esps_telemetry_builder_t *b, uint8_t ch, uint16_t dt_ms,
                                 uint8_t enc, const void *value);
bool esps_telemetry_builder_finish(esps_telemetry_builder_t *b, uint32_t base_ts_ms,
                                    uint8_t flags, size_t *out_len);

/* Walks a received TELEMETRY payload sample-by-sample without copying —
 * `value` in each esps_telemetry_sample_t points into the caller's payload
 * buffer. */
typedef struct {
    uint8_t ch;
    uint16_t dt_ms;
    uint8_t enc;
    const uint8_t *value;
    size_t value_len;
} esps_telemetry_sample_t;

typedef struct {
    const uint8_t *payload;
    size_t len;
    size_t pos;
    uint8_t count;
    uint8_t idx;
} esps_telemetry_reader_t;

/* Validates the 6-byte header (count in [1,64], header fits in `len`) and
 * reports base_ts_ms/flags/count. Does NOT validate sample bodies up front —
 * a malformed sample is caught by esps_telemetry_reader_next() returning
 * false, at the point it would otherwise read past `payload`. */
bool esps_telemetry_reader_init(esps_telemetry_reader_t *r, const uint8_t *payload, size_t len,
                                 uint32_t *base_ts_ms, uint8_t *flags, uint8_t *count);
bool esps_telemetry_reader_next(esps_telemetry_reader_t *r, esps_telemetry_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_ENLP_H */

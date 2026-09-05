/* See esps_log_hook.h. ESP-IDF already serializes calls into the registered
 * vprintf function (esp_log_impl_lock() around esp_log_write), so this file
 * does not add its own mutex for output ordering — only a re-entrancy guard
 * for the pathological case of something inside this file's call chain
 * triggering another log line (e.g. an assert), which a mutex would turn
 * into a deadlock rather than a dropped line.
 *
 * Parsing: by the time esp_log_set_vprintf's callback runs, `fmt`/`args`
 * are ESP-IDF's *fully composed* format for the line — "<L> (<ts>) <tag>: "
 * followed by the caller's own format, followed by "\n" (see
 * LOG_FORMAT/esp_log_write in ESP-IDF). Rendering that with vsnprintf and
 * re-parsing the level/timestamp/tag prefix back out is the standard trick
 * for redirecting ESP-IDF logs to a structured sink; it depends on ANSI
 * color codes being off (sdkconfig.defaults sets CONFIG_LOG_COLORS=n) or
 * the prefix would start with an escape sequence instead of the level
 * letter.
 */
#include "esps_log_hook.h"
#include "esps_enlp.h"
#include "esps_frame.h"
#include "esps_node_id.h"
#include "esps_time.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Generous for a normal log line; vsnprintf truncates safely (never
 * overflows) if a caller's message runs longer, which just means that one
 * line arrives clipped instead of corrupting anything. */
#define ESPS_LOG_LINE_MAX 200

static esps_log_sink_t s_sink = NULL;
static void *s_sink_ctx = NULL;
static volatile bool s_in_hook = false;
static portMUX_TYPE s_reentry_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_dropped = 0;

static uint8_t level_char_to_enlp(char c) {
    switch (c) {
        case 'E':
            return 1;
        case 'W':
            return 2;
        case 'I':
            return 3;
        case 'D':
            return 4;
        case 'V':
            return 5;
        default:
            return 0;
    }
}

/* Parses "<L> (<timestamp>) <tag>: <message>" out of `line`. Falls back to
 * treating the whole line as an untagged message if the prefix doesn't
 * match (e.g. a raw ESP_EARLY_LOG line, or output from something that
 * doesn't use the macro-generated format) — better to deliver an
 * unstructured-but-present line than to drop it. */
static void parse_line(const char *line, size_t line_len, uint8_t *level, const char **tag,
                        size_t *tag_len, const char **msg, size_t *msg_len) {
    *level = 0;
    *tag = "";
    *tag_len = 0;
    *msg = line;
    *msg_len = line_len;

    if (line_len < 4 || line[1] != ' ' || line[2] != '(') {
        return;
    }
    const char *rparen = memchr(line, ')', line_len);
    if (!rparen) {
        return;
    }
    size_t rparen_off = (size_t)(rparen - line);
    if (rparen_off + 2 >= line_len || rparen[1] != ' ') {
        return;
    }
    const char *tag_start = rparen + 2;
    size_t tag_region_len = line_len - (size_t)(tag_start - line);
    const char *colon = memchr(tag_start, ':', tag_region_len);
    if (!colon) {
        return;
    }

    *level = level_char_to_enlp(line[0]);
    *tag = tag_start;
    *tag_len = (size_t)(colon - tag_start);
    const char *m = colon + 1;
    const char *line_end = line + line_len;
    if (m < line_end && *m == ' ') {
        m++;
    }
    *msg = m;
    *msg_len = (size_t)(line_end - m);
}

static int esps_log_hook_vprintf(const char *fmt, va_list args) {
    bool reentered = false;
    portENTER_CRITICAL(&s_reentry_lock);
    if (s_in_hook) {
        reentered = true;
    } else {
        s_in_hook = true;
    }
    portEXIT_CRITICAL(&s_reentry_lock);

    if (reentered) {
        s_dropped++;
        return 0;
    }

    char line[ESPS_LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n < 0) {
        s_dropped++;
        goto out;
    }
    size_t line_len = ((size_t)n >= sizeof(line)) ? sizeof(line) - 1 : (size_t)n;
    while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
        line_len--;
    }

    uint8_t level;
    const char *tag;
    size_t tag_len;
    const char *msg;
    size_t msg_len;
    parse_line(line, line_len, &level, &tag, &tag_len, &msg, &msg_len);
    if (tag_len > 0xFFu) {
        tag_len = 0xFFu; /* wire tag_len is one byte (PROTOCOL.md S4.5) */
    }

    uint8_t payload[ESPS_LOG_HEADER_SIZE + ESPS_LOG_LINE_MAX];
    size_t payload_len = 0;
    if (!esps_enlp_pack_log(esps_time_now_ms(), level, tag, tag_len, msg, msg_len, payload,
                             sizeof(payload), &payload_len)) {
        s_dropped++;
        goto out;
    }

    /* Wire size = COBS(ENLP header + LOG sub-header + line + ENLP crc) + delimiter. */
    uint8_t frame[ESPS_COBS_MAX_ENCODED(ESPS_ENLP_HEADER_SIZE + ESPS_LOG_HEADER_SIZE +
                                         ESPS_LOG_LINE_MAX + ESPS_ENLP_CRC_SIZE) +
                  1];
    size_t frame_len = esps_enlp_encode_cobs(ESPS_ENLP_VERSION, ESPS_MSG_LOG, esps_node_id_get(),
                                              esps_frame_next_seq(), payload, payload_len, frame,
                                              sizeof(frame));
    if (frame_len == 0) {
        s_dropped++;
        goto out;
    }

    if (!s_sink || !s_sink(frame, frame_len, s_sink_ctx)) {
        s_dropped++;
    }

out:
    portENTER_CRITICAL(&s_reentry_lock);
    s_in_hook = false;
    portEXIT_CRITICAL(&s_reentry_lock);
    return n;
}

void esps_log_hook_init(esps_log_sink_t sink, void *ctx) {
    s_sink = sink;
    s_sink_ctx = ctx;
    esp_log_set_vprintf(esps_log_hook_vprintf);
}

uint32_t esps_log_hook_dropped_count(void) {
    return s_dropped;
}

/* TODO(S6): real implementation. This stub only satisfies the link
 * interface shape so other code can be written against it now; open()
 * deliberately fails rather than pretending to succeed and silently
 * dropping every frame (AGENTS.md: "no placeholder code presented as
 * finished" — this is a labelled stub, not a fake success path).
 */
#include "esps_link_tcp.h"

static bool tcp_link_open(esps_link_if_t *self, esps_link_on_frame_cb on_frame,
                           esps_link_on_raw_cb on_raw, void *ctx) {
    (void)self;
    (void)on_frame;
    (void)on_raw;
    (void)ctx;
    return false; /* TODO(S6) */
}

static bool tcp_link_send(esps_link_if_t *self, const uint8_t *frame, size_t len) {
    (void)self;
    (void)frame;
    (void)len;
    return false; /* TODO(S6) */
}

static void tcp_link_poll(esps_link_if_t *self) {
    (void)self; /* TODO(S6) */
}

static void tcp_link_close(esps_link_if_t *self) {
    (void)self; /* TODO(S6) */
}

void esps_link_tcp_init(esps_link_if_t *out, const esps_link_tcp_config_t *config) {
    (void)config;
    out->name = "tcp";
    out->open = tcp_link_open;
    out->send = tcp_link_send;
    out->poll = tcp_link_poll;
    out->close = tcp_link_close;
    out->impl = NULL;
}

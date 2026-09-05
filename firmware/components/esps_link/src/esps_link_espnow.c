/* TODO(S6): real implementation. See esps_link_tcp.c for why open() fails
 * rather than faking success.
 */
#include "esps_link_espnow.h"

static bool espnow_link_open(esps_link_if_t *self, esps_link_on_frame_cb on_frame,
                              esps_link_on_raw_cb on_raw, void *ctx) {
    (void)self;
    (void)on_frame;
    (void)on_raw;
    (void)ctx;
    return false; /* TODO(S6) */
}

static bool espnow_link_send(esps_link_if_t *self, const uint8_t *frame, size_t len) {
    (void)self;
    (void)frame;
    (void)len;
    return false; /* TODO(S6) */
}

static void espnow_link_poll(esps_link_if_t *self) {
    (void)self; /* TODO(S6) */
}

static void espnow_link_close(esps_link_if_t *self) {
    (void)self; /* TODO(S6) */
}

void esps_link_espnow_init(esps_link_if_t *out, const esps_link_espnow_config_t *config) {
    (void)config;
    out->name = "espnow";
    out->open = espnow_link_open;
    out->send = espnow_link_send;
    out->poll = espnow_link_poll;
    out->close = espnow_link_close;
    out->impl = NULL;
}

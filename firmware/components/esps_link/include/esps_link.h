/* Transport abstraction (docs/ARCHITECTURE.md: "One interface (open/send/
 * poll/close), implementations for UART, TCP/WS and ESP-NOW"). One
 * esps_link_if_t per transport instance; main.c wires whichever transport
 * is active to the same frame/CMD handling code, so nothing above this
 * layer needs to know or care which wire it's talking over.
 *
 * The non-blocking-send invariant here is not a style preference: PROTOCOL.md
 * S5.1 says "the node never blocks on the link", because a blocked TX task
 * is a node that stops sampling, and the whole point of the autonomy model
 * is that the experiment keeps running when nobody is listening.
 */
#ifndef ESPS_LINK_H
#define ESPS_LINK_H

#include "esps_enlp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Invoked once per fully decoded, CRC-valid ENLP frame received on this
 * link. `frame` (and the buffer it points into) is only valid for the
 * duration of the callback — copy out anything needed afterward. */
typedef void (*esps_link_on_frame_cb)(const esps_enlp_frame_t *frame, void *ctx);

/* Invoked for bytes that arrived but did not decode into a valid frame
 * (PROTOCOL.md S2.1) — boot-ROM banners and any other non-ENLP noise.
 * Framed transports (TCP/WS length-prefix, ESP-NOW raw) have no equivalent
 * of "noise between delimiters" and may simply never call this. */
typedef void (*esps_link_on_raw_cb)(const uint8_t *data, size_t len, void *ctx);

typedef struct esps_link_if esps_link_if_t;

struct esps_link_if {
    const char *name;

    /* Brings the transport up: installs drivers, starts tasks, registers
     * the callbacks. Returns false on failure (bad config, driver install
     * error) — the caller decides whether that's fatal or retryable. */
    bool (*open)(esps_link_if_t *self, esps_link_on_frame_cb on_frame,
                 esps_link_on_raw_cb on_raw, void *ctx);

    /* Queues one already-built, ready-to-transmit frame (the COBS+delimiter
     * wire form for serial transports; the raw frame body for transports
     * that frame it themselves). Always non-blocking: returns false
     * immediately — never after waiting — if the frame is too large for
     * this transport or the outbound queue is full. A false return is
     * store-and-forward pressure (PROTOCOL.md S5.1), not a transport fault;
     * callers must not spin-retry it, only mark the data as buffered. */
    bool (*send)(esps_link_if_t *self, const uint8_t *frame, size_t len);

    /* Non-blocking, drains whatever is immediately available. Implementations
     * that run RX on their own task (UART does) can make this a no-op —
     * it exists for transports with no spare task to pump from. */
    void (*poll)(esps_link_if_t *self);

    /* Tears the transport down: stops tasks, uninstalls drivers, frees
     * queues. Safe to call on a link that was never successfully opened. */
    void (*close)(esps_link_if_t *self);

    void *impl; /* transport-private state; owned by the esps_link_<transport> module */
};

#ifdef __cplusplus
}
#endif

#endif /* ESPS_LINK_H */

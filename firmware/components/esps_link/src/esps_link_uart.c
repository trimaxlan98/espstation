/* See esps_link_uart.h. RX runs on its own task feeding the esps_proto
 * streaming decoder directly (no intermediate buffering beyond the UART
 * driver's own ring buffer); TX runs on its own task pulling off a bounded
 * FreeRTOS queue, so a slow or wedged consumer downstream never blocks
 * whoever is producing frames (docs/ARCHITECTURE.md's "bounded queue,
 * non-blocking send" invariant).
 *
 * Single static instance: this sprint only ever brings up one UART link, so
 * the impl struct is a module-level singleton rather than heap-allocated —
 * consistent with "no dynamic allocation" and simpler than pretending this
 * supports multiple concurrent UART links when nothing calls it that way.
 * A second instance would need this turned into a small fixed pool.
 */
#include "esps_link_uart.h"
#include "esps_enlp.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

typedef struct {
    uint16_t len;
    uint8_t data[ESPS_LINK_UART_MAX_FRAME];
} esps_link_uart_tx_item_t;

typedef struct {
    esps_link_uart_config_t cfg;
    QueueHandle_t tx_queue;
    TaskHandle_t tx_task;
    TaskHandle_t rx_task;
    esps_link_on_frame_cb on_frame;
    esps_link_on_raw_cb on_raw;
    void *user_ctx;
    esps_enlp_stream_t stream;
    uint32_t dropped;
    bool is_open;
} esps_link_uart_impl_t;

static esps_link_uart_impl_t s_impl;

static void stream_on_frame(const esps_enlp_frame_t *frame, void *ctx) {
    esps_link_uart_impl_t *im = (esps_link_uart_impl_t *)ctx;
    if (im->on_frame) {
        im->on_frame(frame, im->user_ctx);
    }
}

static void stream_on_raw(const uint8_t *data, size_t len, void *ctx) {
    esps_link_uart_impl_t *im = (esps_link_uart_impl_t *)ctx;
    if (im->on_raw) {
        im->on_raw(data, len, im->user_ctx);
    }
}

static void rx_task_fn(void *arg) {
    esps_link_uart_impl_t *im = (esps_link_uart_impl_t *)arg;
    uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(im->cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            esps_enlp_stream_feed(&im->stream, buf, (size_t)n);
        }
    }
}

static void tx_task_fn(void *arg) {
    esps_link_uart_impl_t *im = (esps_link_uart_impl_t *)arg;
    esps_link_uart_tx_item_t item;
    for (;;) {
        if (xQueueReceive(im->tx_queue, &item, portMAX_DELAY) == pdTRUE) {
            uart_write_bytes(im->cfg.uart_num, (const char *)item.data, item.len);
        }
    }
}

static bool uart_link_open(esps_link_if_t *self, esps_link_on_frame_cb on_frame,
                            esps_link_on_raw_cb on_raw, void *ctx) {
    esps_link_uart_impl_t *im = (esps_link_uart_impl_t *)self->impl;
    im->on_frame = on_frame;
    im->on_raw = on_raw;
    im->user_ctx = ctx;
    esps_enlp_stream_init(&im->stream, stream_on_frame, stream_on_raw, im);

    uart_config_t uart_cfg = {
        .baud_rate = im->cfg.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_driver_install(im->cfg.uart_num, (int)im->cfg.rx_buf_size, 0, 0, NULL, 0) != ESP_OK) {
        return false;
    }
    if (uart_param_config(im->cfg.uart_num, &uart_cfg) != ESP_OK) {
        uart_driver_delete(im->cfg.uart_num);
        return false;
    }
    int tx_pin = (im->cfg.tx_pin < 0) ? UART_PIN_NO_CHANGE : im->cfg.tx_pin;
    int rx_pin = (im->cfg.rx_pin < 0) ? UART_PIN_NO_CHANGE : im->cfg.rx_pin;
    if (uart_set_pin(im->cfg.uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) !=
        ESP_OK) {
        uart_driver_delete(im->cfg.uart_num);
        return false;
    }

    im->tx_queue = xQueueCreate(im->cfg.tx_queue_depth, sizeof(esps_link_uart_tx_item_t));
    if (!im->tx_queue) {
        uart_driver_delete(im->cfg.uart_num);
        return false;
    }

    /* rx_task_fn runs the frame/raw callbacks inline (CMD dispatch, the log
     * hook's re-entrancy path, etc.), each of which builds a full
     * ESPS_LINK_UART_MAX_FRAME-sized reply on the stack — sized with that
     * in mind, not just the UART read loop's own small buffer. */
    BaseType_t tx_ok = xTaskCreate(tx_task_fn, "esps_link_tx", 4096, im, 10, &im->tx_task);
    BaseType_t rx_ok = xTaskCreate(rx_task_fn, "esps_link_rx", 4096, im, 10, &im->rx_task);
    if (tx_ok != pdPASS || rx_ok != pdPASS) {
        if (im->tx_task) {
            vTaskDelete(im->tx_task);
            im->tx_task = NULL;
        }
        if (im->rx_task) {
            vTaskDelete(im->rx_task);
            im->rx_task = NULL;
        }
        vQueueDelete(im->tx_queue);
        im->tx_queue = NULL;
        uart_driver_delete(im->cfg.uart_num);
        return false;
    }

    im->is_open = true;
    return true;
}

static bool uart_link_send(esps_link_if_t *self, const uint8_t *frame, size_t len) {
    esps_link_uart_impl_t *im = (esps_link_uart_impl_t *)self->impl;
    if (len == 0 || len > ESPS_LINK_UART_MAX_FRAME) {
        im->dropped++;
        return false;
    }
    esps_link_uart_tx_item_t item;
    item.len = (uint16_t)len;
    memcpy(item.data, frame, len);
    if (xQueueSend(im->tx_queue, &item, 0) != pdTRUE) { /* 0 ticks: never block */
        im->dropped++;
        return false;
    }
    return true;
}

static void uart_link_poll(esps_link_if_t *self) {
    (void)self; /* RX is handled by rx_task_fn; nothing to pump from here */
}

static void uart_link_close(esps_link_if_t *self) {
    esps_link_uart_impl_t *im = (esps_link_uart_impl_t *)self->impl;
    if (!im->is_open) {
        return;
    }
    if (im->tx_task) {
        vTaskDelete(im->tx_task);
        im->tx_task = NULL;
    }
    if (im->rx_task) {
        vTaskDelete(im->rx_task);
        im->rx_task = NULL;
    }
    if (im->tx_queue) {
        vQueueDelete(im->tx_queue);
        im->tx_queue = NULL;
    }
    uart_driver_delete(im->cfg.uart_num);
    im->is_open = false;
}

void esps_link_uart_init(esps_link_if_t *out, const esps_link_uart_config_t *config) {
    memset(&s_impl, 0, sizeof(s_impl));
    s_impl.cfg = *config;

    out->name = "uart";
    out->open = uart_link_open;
    out->send = uart_link_send;
    out->poll = uart_link_poll;
    out->close = uart_link_close;
    out->impl = &s_impl;
}

uint32_t esps_link_uart_dropped_count(const esps_link_if_t *link) {
    const esps_link_uart_impl_t *im = (const esps_link_uart_impl_t *)link->impl;
    return im->dropped;
}

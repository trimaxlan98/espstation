/* GPIO allow-list and fixed assignments for the digital link
 * (SPEC-LINK.md "Pines"), plus the NDB channel ids the node publishes.
 *
 * The allow-list exists so the station's `set_gpio` (and the experiment
 * config) can never drive GPIO6-11 (flash), GPIO12 (strapping/MTDI — a high
 * level at boot selects 1.8 V flash and bricks the boot), the boot-sensitive
 * 0/2/5/15, or the input-only 34-39.
 */
#ifndef ESPS_DIO_PINS_H
#define ESPS_DIO_PINS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPS_DIO_PIN_TX_DATA 26 /* OUTPUT                          */
#define ESPS_DIO_PIN_RX_DATA 25 /* INPUT_PULLDOWN                  */
#define ESPS_DIO_PIN_TX_CLK  27 /* OUTPUT, N3 only                 */
#define ESPS_DIO_PIN_RX_CLK  14 /* INPUT_PULLDOWN, N3 only         */
#define ESPS_DIO_PIN_LED      4 /* OUTPUT, external LED + 330 ohm  */

/* NDB channel ids (16-127 is the node-defined range). */
#define ESPS_DIO_CH_TX         16 /* dio.tx          u8            */
#define ESPS_DIO_CH_RX         17 /* dio.rx          u8            */
#define ESPS_DIO_CH_RTT_US     18 /* link.rtt_us     u32, us       */
#define ESPS_DIO_CH_FRAMES_OK  19 /* link.frames_ok  u32           */
#define ESPS_DIO_CH_FRAMES_ERR 20 /* link.frames_err u32 (crc+len) */
#define ESPS_DIO_CH_BER        21 /* link.ber        f32           */

/* True only for 4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33.
 * Any other value — including negatives and numbers above the SoC's range —
 * is rejected. */
bool esps_dio_pin_is_allowed_output(int gpio);

/* Same set as outputs. 34-39 are input-only but are excluded on purpose: the
 * link relies on the internal pull-down, which those pins do not have. */
bool esps_dio_pin_is_allowed_input(int gpio);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_DIO_PINS_H */

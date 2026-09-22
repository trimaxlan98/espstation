/* Pin assignments and NDB channel ids for the full-duplex Morse link
 * (bench/practicas/morse-duplex/SPEC-DUPLEX.md "Pines" and "Canales NDB").
 *
 * The data pins are deliberately the same 25/26 as the two-wire digital link:
 * on the bench it is the same physical cable. The two practices are never
 * flashed at the same time, so the reuse costs nothing and means one wiring
 * to learn instead of two.
 *
 * GPIO16/17 are free on WROOM modules; on WROVER they belong to the PSRAM and
 * ESPS_MORSE_PIN_LED_RX would have to move. GPIO12 is forbidden everywhere in
 * this bench: a high level at boot selects 1.8 V flash and bricks the boot.
 */
#ifndef ESPS_MORSE_PINS_H
#define ESPS_MORSE_PINS_H

#ifdef __cplusplus
extern "C" {
#endif

#define ESPS_MORSE_PIN_KEY     13 /* INPUT_PULLDOWN, the operator's key     */
#define ESPS_MORSE_PIN_TX_DATA 26 /* OUTPUT, mirrors the debounced key      */
#define ESPS_MORSE_PIN_RX_DATA 25 /* INPUT_PULLDOWN, from the far end's TX  */
#define ESPS_MORSE_PIN_LED_TX   4 /* OUTPUT, own key                        */
#define ESPS_MORSE_PIN_LED_RX  16 /* OUTPUT, incoming level                 */

/* NDB channel ids. 16-127 is the node-defined range (PROTOCOL.md
 * channel_id_ranges) and 16-21 already belong to the digital link, so Morse
 * claims 22-29. These are the same ids the gateway mirror declares in
 * transports/sim/morse_link.py; changing one without the other is the drift
 * the golden vectors exist to catch. */
#define ESPS_MORSE_CH_TX       22 /* morse.tx        u8        */
#define ESPS_MORSE_CH_RX       23 /* morse.rx        u8        */
#define ESPS_MORSE_CH_PULSE_MS 24 /* morse.pulse_ms  u32, ms   */
#define ESPS_MORSE_CH_GAP_MS   25 /* morse.gap_ms    u32, ms   */
#define ESPS_MORSE_CH_SYMBOLS  26 /* morse.symbols   u32       */
#define ESPS_MORSE_CH_LETTERS  27 /* morse.letters   u32       */
#define ESPS_MORSE_CH_UNKNOWN  28 /* morse.unknown   u32       */
#define ESPS_MORSE_CH_BOUNCES  29 /* morse.bounces   u32       */

/* Event codes, exactly as SPEC-DUPLEX.md names them. */
#define ESPS_MORSE_EV_SYMBOL   "morse.symbol"
#define ESPS_MORSE_EV_LETTER   "morse.letter"
#define ESPS_MORSE_EV_WORD     "morse.word"
#define ESPS_MORSE_EV_UNKNOWN  "morse.unknown"
#define ESPS_MORSE_EV_FILTERED "morse.filtered"

#ifdef __cplusplus
}
#endif

#endif /* ESPS_MORSE_PINS_H */

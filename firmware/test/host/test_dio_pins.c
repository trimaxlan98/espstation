/* Pin allow-list: every GPIO 0..48 checked individually against the exact
 * list in SPEC-LINK.md, plus out-of-range values. */
#include "esps_dio_pins.h"
#include "harness.h"

int test_dio_pins_all(void) {
    int fails = 0;

    static const int allowed[] = {4, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
    for (int gpio = 0; gpio <= 48; gpio++) {
        bool want = false;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
            if (allowed[i] == gpio) want = true;
        }
        if (esps_dio_pin_is_allowed_output(gpio) != want) {
            fails++;
            fprintf(stderr, "FAIL %s:%d: output gpio %d expected %d\n", __FILE__, __LINE__,
                    gpio, want);
        }
        if (esps_dio_pin_is_allowed_input(gpio) != want) {
            fails++;
            fprintf(stderr, "FAIL %s:%d: input gpio %d expected %d\n", __FILE__, __LINE__,
                    gpio, want);
        }
    }

    /* The ones that brick or destabilise a board, spelled out. */
    ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_output(12));
    for (int gpio = 6; gpio <= 11; gpio++) {
        ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_output(gpio));
    }
    for (int gpio = 34; gpio <= 39; gpio++) {
        ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_output(gpio));
        ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_input(gpio));
    }
    ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_output(0));
    ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_output(2));
    ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_output(5));
    ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_output(15));

    /* Out of range, including values that would be UB in a naive shift. */
    static const int bad[] = {-1, -100, 49, 63, 64, 65, 100, 1 << 20, -2147483647 - 1, 2147483647};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_output(bad[i]));
        ESPS_CHECK(&fails, !esps_dio_pin_is_allowed_input(bad[i]));
    }

    /* The fixed assignments must themselves be allowed and match the spec. */
    ESPS_CHECK_EQ(&fails, ESPS_DIO_PIN_TX_DATA, 26);
    ESPS_CHECK_EQ(&fails, ESPS_DIO_PIN_RX_DATA, 25);
    ESPS_CHECK_EQ(&fails, ESPS_DIO_PIN_TX_CLK, 27);
    ESPS_CHECK_EQ(&fails, ESPS_DIO_PIN_RX_CLK, 14);
    ESPS_CHECK_EQ(&fails, ESPS_DIO_PIN_LED, 4);
    ESPS_CHECK(&fails, esps_dio_pin_is_allowed_output(ESPS_DIO_PIN_TX_DATA));
    ESPS_CHECK(&fails, esps_dio_pin_is_allowed_input(ESPS_DIO_PIN_RX_DATA));
    ESPS_CHECK(&fails, esps_dio_pin_is_allowed_output(ESPS_DIO_PIN_TX_CLK));
    ESPS_CHECK(&fails, esps_dio_pin_is_allowed_input(ESPS_DIO_PIN_RX_CLK));
    ESPS_CHECK(&fails, esps_dio_pin_is_allowed_output(ESPS_DIO_PIN_LED));

    /* NDB channel ids. */
    ESPS_CHECK_EQ(&fails, ESPS_DIO_CH_TX, 16);
    ESPS_CHECK_EQ(&fails, ESPS_DIO_CH_RX, 17);
    ESPS_CHECK_EQ(&fails, ESPS_DIO_CH_RTT_US, 18);
    ESPS_CHECK_EQ(&fails, ESPS_DIO_CH_FRAMES_OK, 19);
    ESPS_CHECK_EQ(&fails, ESPS_DIO_CH_FRAMES_ERR, 20);
    ESPS_CHECK_EQ(&fails, ESPS_DIO_CH_BER, 21);

    return fails;
}

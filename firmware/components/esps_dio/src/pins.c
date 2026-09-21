#include "esps_dio_pins.h"

#include <stdint.h>

/* Bitmask over GPIO0..63 rather than a switch or array scan: constant time,
 * one place to audit against SPEC-LINK.md, and out-of-range values are
 * rejected before the shift (shifting by >= 64 is undefined). */
#define PIN(n) (UINT64_C(1) << (n))
static const uint64_t ALLOWED_MASK =
    PIN(4) | PIN(13) | PIN(14) | PIN(16) | PIN(17) | PIN(18) | PIN(19) |
    PIN(21) | PIN(22) | PIN(23) | PIN(25) | PIN(26) | PIN(27) | PIN(32) | PIN(33);

bool esps_dio_pin_is_allowed_output(int gpio) {
    if (gpio < 0 || gpio > 63) {
        return false;
    }
    return (ALLOWED_MASK & (UINT64_C(1) << gpio)) != 0u;
}

bool esps_dio_pin_is_allowed_input(int gpio) {
    return esps_dio_pin_is_allowed_output(gpio);
}

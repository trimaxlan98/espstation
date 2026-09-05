/* See esps_frame.h. A spinlock (not a mutex) because increments are O(1) and
 * this can be called from the log hook, which must never block.
 */
#include "esps_frame.h"

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_seq_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_seq = 0;

uint16_t esps_frame_next_seq(void) {
    portENTER_CRITICAL(&s_seq_lock);
    uint16_t v = s_seq++;
    portEXIT_CRITICAL(&s_seq_lock);
    return v;
}

/* Deterministic test payload generator and receiver resync rule
 * (SPEC-LINK.md "Payload de prueba"). Both ends of the link, the Arduino
 * sketches and the gateway simulator run the same generator, so the receiver
 * can regenerate what the sender should have sent and count bit errors
 * without any side channel.
 */
#ifndef ESPS_DIO_TESTGEN_H
#define ESPS_DIO_TESTGEN_H

#include "esps_dio_frame.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPS_DIO_TEST_SEED 0xC0FFEEu

/* One xorshift32 step (13, 17, 5), uint32 arithmetic. x == 0 is a fixed
 * point; testframe_payload() guards against it. */
uint32_t esps_dio_xorshift32(uint32_t x);

/* Writes the payload of test frame number `seq` into out[0..len) and returns
 * len (1..32). out[len..32) is zero-filled so the whole buffer is defined
 * (see esps_dio_stats_account()). payload[0] is seq & 0xFF. Returns 0 and
 * writes nothing if out is NULL. */
size_t esps_dio_testframe_payload(uint32_t seq, uint8_t out[ESPS_DIO_MAX_PAYLOAD]);

/* Receiver resync: given the currently expected index k and byte 0 of a
 * CRC-valid frame, returns k + ((byte0 - k) & 0xFF) — the smallest index >= k
 * whose low byte is byte0. Wraps in uint32. */
uint32_t esps_dio_resync_index(uint32_t expected_k, uint8_t byte0);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_DIO_TESTGEN_H */

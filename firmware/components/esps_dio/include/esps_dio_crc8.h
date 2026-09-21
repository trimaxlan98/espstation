/* CRC-8/SMBUS — the checksum of the two-wire digital link frame
 * (bench/practicas/enlace-digital/SPEC-LINK.md). Parameters: poly 0x07, init
 * 0x00, no input/output reflection, xorout 0x00. Check value:
 * esps_dio_crc8("123456789", 9, 0) == 0xF4. This is NOT part of ENLP; it only
 * protects bytes travelling between two nodes over a cable.
 *
 * Pure C11, no ESP-IDF dependency — must build with plain gcc for the host
 * unit tests (test/host/) as well as inside the firmware.
 */
#ifndef ESPS_DIO_CRC8_H
#define ESPS_DIO_CRC8_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initial value mandated by the spec; also the CRC of an empty input. */
#define ESPS_DIO_CRC8_INIT 0x00u

/* Feeds data[0..len) through an in-progress CRC and returns the new value.
 * Chain calls by passing the previous result as init (LEN, then PAYLOAD, or
 * one byte at a time as the receiver does); start a computation with
 * ESPS_DIO_CRC8_INIT. */
uint8_t esps_dio_crc8(const uint8_t *data, size_t len, uint8_t init);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_DIO_CRC8_H */

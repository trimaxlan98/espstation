/* CRC-16/CCITT-FALSE — the checksum guarding the ENLP frame body on the
 * serial path (PROTOCOL.md S3). Parameters: poly 0x1021, init 0xFFFF, no
 * input/output reflection, no final XOR. Fixed by the protocol, not a
 * tunable: any other CRC-16 variant (XMODEM, MODBUS, ...) produces a
 * different result from the same bytes and will desync with the gateway.
 *
 * Pure C11, no ESP-IDF dependency — this file must build with plain gcc for
 * the host unit tests (test/host/) as well as inside the firmware.
 */
#ifndef ESPS_CRC16_H
#define ESPS_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initial value mandated by CRC-16/CCITT-FALSE; also the correct result for
 * a zero-length input, which the host tests check directly. */
#define ESPS_CRC16_INIT 0xFFFFu

/* Incremental update: feed a buffer through an in-progress CRC. Start a new
 * computation with crc = ESPS_CRC16_INIT. Lets a frame be CRC'd in pieces
 * (header, then payload) without concatenating them first. */
uint16_t esps_crc16_update(uint16_t crc, const void *buf, size_t len);

/* One-shot convenience: esps_crc16(buf, len) == esps_crc16_update(ESPS_CRC16_INIT, buf, len). */
uint16_t esps_crc16(const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ESPS_CRC16_H */

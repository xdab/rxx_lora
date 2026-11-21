/**
 * @file crc.h
 * @brief CRC-16/XMODEM.
 */

#ifndef CRC_H
#define CRC_H

#include <stdint.h>

/**
 * @brief Frame check sequence of the LoRa PHY.
 *
 * The scheme is CRC-16/XMODEM: polynomial 0x1021 (x^16 + x^12 + x^5 + 1),
 * initial value 0x0000, no reflection, no final XOR.
 *
 * @param[in] data bytes to cover
 * @param[in] len byte count
 * @return the 16-bit CRC
 */
uint16_t crc16_compute(const uint8_t *data, uint32_t len);

#endif // CRC_H

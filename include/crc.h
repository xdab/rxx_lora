/**
 * @file crc.h
 * @brief CRC-16/XMODEM.
 */

#ifndef CRC_H
#define CRC_H

#include <stdint.h>

/**
 * @brief CRC-16/XMODEM: poly 0x1021, init 0, MSB first
 */
uint16_t crc16_compute(const uint8_t *data, uint32_t len);

#endif // CRC_H

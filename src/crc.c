#include "crc.h"
#include <stdint.h>

uint16_t crc16_compute(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0x0000;

    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }

    return crc;
}

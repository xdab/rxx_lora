#include "header.h"
#include <stdint.h>

// bit k of header nibble v
static uint8_t nib_bit(uint8_t v, int k) { return (uint8_t)((v >> k) & 1u); }

int decode_header(const uint8_t *nibs, struct lora_header *h)
{
    uint8_t chk, c4, c3, c2, c1, c0;

    h->pay_len = (uint8_t)((nibs[0] << 4) + nibs[1]);
    h->has_crc = nibs[2] & 1;
    h->cr = (uint8_t)(nibs[2] >> 1);

    // deviation: cr indexes fixed CR 1..4 arrays here, so an out-of-range cr
    // is rejected as a header error
    if (h->cr < 1 || h->cr > 4)
        return -1;

    chk = (uint8_t)(((nibs[3] & 1) << 4) + nibs[4]);

    // header checksum
    c4 = (uint8_t)(nib_bit(nibs[0], 3) ^ nib_bit(nibs[0], 2) ^ nib_bit(nibs[0], 1) ^
                   nib_bit(nibs[0], 0));
    c3 = (uint8_t)(nib_bit(nibs[0], 3) ^ nib_bit(nibs[1], 3) ^ nib_bit(nibs[1], 2) ^
                   nib_bit(nibs[1], 1) ^ nib_bit(nibs[2], 0));
    c2 = (uint8_t)(nib_bit(nibs[0], 2) ^ nib_bit(nibs[1], 3) ^ nib_bit(nibs[1], 0) ^
                   nib_bit(nibs[2], 3) ^ nib_bit(nibs[2], 1));
    c1 = (uint8_t)(nib_bit(nibs[0], 1) ^ nib_bit(nibs[1], 2) ^ nib_bit(nibs[1], 0) ^
                   nib_bit(nibs[2], 2) ^ nib_bit(nibs[2], 1) ^ nib_bit(nibs[2], 0));
    c0 = (uint8_t)(nib_bit(nibs[0], 0) ^ nib_bit(nibs[1], 1) ^ nib_bit(nibs[2], 3) ^
                   nib_bit(nibs[2], 2) ^ nib_bit(nibs[2], 1) ^ nib_bit(nibs[2], 0));

    // checksum mismatch or empty frame is a header error
    if (chk != (uint8_t)((c4 << 4) + (c3 << 3) + (c2 << 2) + (c1 << 1) + c0) || h->pay_len == 0)
        return -1;

    return 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // mirrors the reference's symb_numb formula
uint32_t decode_symbol_budget(uint8_t pay_len, uint8_t cr, int has_crc, uint8_t sf)
{
    // the -(sf-2): the first payload nibbles ride in the header block's slots
    uint32_t nibs = 2u * pay_len + 5u + (has_crc ? 4u : 0u) - (uint32_t)(sf - 2);
    uint32_t blocks = (nibs + (uint32_t)sf - 1u) / (uint32_t)sf;

    return 8u + blocks * (4u + cr);
}

/*
 * Explicit-header decode of the coding chain; line references point at
 * [gr-lora_sdr] header_decoder_impl.cc.
 */

#include "header.h"
#include <stdint.h>

// Reference: [gr-lora_sdr] header_decoder_impl.cc:71,133-162
int decode_header(const uint8_t *nibs, struct lora_header *h)
{
    uint8_t chk, c4, c3, c2, c1, c0;

    h->pay_len = (uint8_t)((nibs[0] << 4) + nibs[1]);
    h->has_crc = nibs[2] & 1;
    h->cr = (uint8_t)(nibs[2] >> 1);
    chk = (uint8_t)(((nibs[3] & 1) << 4) + nibs[4]);

    // header checksum
    c4 = (uint8_t)(((nibs[0] & 8) >> 3) ^ ((nibs[0] & 4) >> 2) ^ ((nibs[0] & 2) >> 1) ^
                   (nibs[0] & 1));
    c3 = (uint8_t)(((nibs[0] & 8) >> 3) ^ ((nibs[1] & 8) >> 3) ^ ((nibs[1] & 4) >> 2) ^
                   ((nibs[1] & 2) >> 1) ^ (nibs[2] & 1));
    c2 = (uint8_t)(((nibs[0] & 4) >> 2) ^ ((nibs[1] & 8) >> 3) ^ (nibs[1] & 1) ^
                   ((nibs[2] & 8) >> 3) ^ ((nibs[2] & 2) >> 1));
    c1 = (uint8_t)(((nibs[0] & 2) >> 1) ^ ((nibs[1] & 4) >> 2) ^ (nibs[1] & 1) ^
                   ((nibs[2] & 4) >> 2) ^ ((nibs[2] & 2) >> 1) ^ (nibs[2] & 1));
    c0 = (uint8_t)((nibs[0] & 1) ^ ((nibs[1] & 2) >> 1) ^ ((nibs[2] & 8) >> 3) ^
                   ((nibs[2] & 4) >> 2) ^ ((nibs[2] & 2) >> 1) ^ (nibs[2] & 1));

    // checksum mismatch or empty frame is a header error
    if (chk != (uint8_t)((c4 << 4) + (c3 << 3) + (c2 << 2) + (c1 << 1) + c0) || h->pay_len == 0)
        return -1;

    // deviation: cr indexes fixed CR 1..4 arrays here, so an out-of-range cr
    // is rejected as a header error
    if (h->cr < 1 || h->cr > 4)
        return -1;
    return 0;
}

// Reference: [gr-lora_sdr] frame_sync_impl.cc:419
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // mirrors the reference's symb_numb formula
uint32_t decode_symbol_budget(uint8_t pay_len, uint8_t cr, int has_crc, uint8_t sf)
{
    /* symb_numb = 8 + ceil((2*pay_len - sf + 2 + 5 + 4*crc) / sf) * (4+cr);
     * the -(sf-2) inside the ceil: the first payload nibbles ride in the
     * header block's sf-2 nibble slots */
    uint32_t nibs = 2u * pay_len + 5u + (has_crc ? 4u : 0u) - (uint32_t)(sf - 2);
    uint32_t blocks = (nibs + (uint32_t)sf - 1u) / (uint32_t)sf;
    return 8u + blocks * (4u + cr);
}

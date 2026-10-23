/**
 * @file header.h
 * @brief Explicit-header decode: header fields, checksum, and the
 *        payload symbol budget it unlocks.
 */

#ifndef HEADER_H
#define HEADER_H

#include <stdint.h>

// Decoded PHY header fields (header_decoder_impl.cc:133-136).
struct lora_header
{
    uint8_t pay_len; // payload length in bytes
    uint8_t cr;      // coding rate: 4/(4+cr), 1..4
    int has_crc;     // CRC presence flag
};

/**
 * @brief Decode the 5 header nibbles into the header fields and check the
 *        header checksum.
 *
 * @param[in] nibs hamming-decoded header nibbles (at least 5)
 * @param[out] h decoded fields
 * @return 0 on a valid header, -1 on checksum mismatch, empty frame or
 *         coding rate outside 1..4
 *
 * Reference: [gr-lora_sdr] header_decoder_impl.cc:71,133-162.
 */
int decode_header(const uint8_t *nibs, struct lora_header *h);

/**
 * @brief Total payload-area symbol budget: 8 header symbols plus one
 *        (cr+4)-symbol block per sf payload nibbles (LDRO off, explicit
 *        header). Reference: [gr-lora_sdr] frame_sync_impl.cc:419.
 */
uint32_t decode_symbol_budget(uint8_t pay_len, uint8_t cr, int has_crc, uint8_t sf);

#endif // HEADER_H

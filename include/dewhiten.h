/**
 * @file dewhiten.h
 * @brief LoRa coding chain, byte level: dewhitening.
 */

#ifndef DEWHITEN_H
#define DEWHITEN_H

#include <stdint.h>

/**
 * @brief Dewhitening: XOR the input nibbles against the fixed whitening
 *        sequence, two nibbles per output byte.
 *
 * Byte i of @p out combines input nibbles 2i (low) and 2i+1 (high), each
 * XORed with the matching nibble of whitening sequence byte i.
 *
 * @param[in] nibs 2*n input nibbles, low nibble first
 * @param[in] n bytes to produce, at most 255 (the sequence length)
 * @param[out] out the n dewhitened bytes
 *
 * Reference: [gr-lora_sdr] dewhitening_impl.cc:96-98.
 */
void dewhiten(const uint8_t *nibs, uint8_t n, uint8_t *out);

#endif // DEWHITEN_H

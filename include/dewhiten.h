/**
 * @file dewhiten.h
 * @brief LoRa coding chain, byte level: dewhitening.
 */

#ifndef DEWHITEN_H
#define DEWHITEN_H

#include <stdint.h>

/**
 * @brief Dewhiten one frame's payload: byte i assembles payload nibble
 *        pair 2i (low) / 2i+1 (high), each XORed against the matching
 *        nibble of the whitening sequence's i-th byte — low nibble first.
 *
 * @param[in] nibs 2*n payload nibbles, low nibble first
 * @param[in] n payload bytes, at most 255 (the sequence length)
 * @param[out] out the n dewhitened bytes
 */
void dewhiten(const uint8_t *nibs, uint8_t n, uint8_t *out);

#endif // DEWHITEN_H

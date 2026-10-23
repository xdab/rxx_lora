/**
 * @file hamming.h
 * @brief Soft-decision Hamming decode of one interleaved codeword.
 */

#ifndef HAMMING_H
#define HAMMING_H

#include <stdint.h>

/**
 * @brief Soft-decision hamming decode of one codeword to its data nibble.
 *
 * The (4+cr, 4) block code has 16 codewords, so the APP rule is an
 * exhaustive correlation: every candidate nibble's codeword is scored by
 * the sum of the chip LLRs it asserts (sign = the bit), max wins.
 *
 * @param[in] llr cw_len = cr+4 chip LLRs, MSB-first, packing the bits the
 *            candidate encoder asserts (llr[k] > 0 means bit k is more likely 1)
 * @param[in] cr coding rate: 1..4
 * @return the 4-bit data nibble
 */
uint8_t hamming_decode_soft(const float *llr, uint8_t cr);

#endif // HAMMING_H

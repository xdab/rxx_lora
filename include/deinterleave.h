/**
 * @file deinterleave.h
 * @brief LoRa coding chain, symbol level: deinterleaving.
 */

#ifndef DEINTERLEAVE_H
#define DEINTERLEAVE_H

#include <stdint.h>

/**
 * @brief Reorder one block's bit reliabilities into codeword order.
 *
 * The interleaver scatters each Hamming codeword's bits across symbols —
 * one bit per symbol, at a position that rotates with the codeword index
 * r — and this undoes that permutation on soft inputs. Inputs and outputs
 * are log-likelihood ratios (LLRs): positive means the bit is more likely 1.
 *
 * @param[in] sym_llr cw_len symbol rows of 12 LLRs each; [i][j] is bit j
 *                   (MSB first) of symbol i
 * @param[in] cw_len symbols per block: 8 for the header, cr+4 for payload
 * @param[in] sf_app codewords out: sf-2 for the header, sf for payload
 * @param[out] cw_out sf_app codeword rows of cw_len LLRs, [r][i] MSB first
 */
void deinterleave_soft(
    const float (*sym_llr)[12], uint8_t cw_len, uint8_t sf_app, float (*cw_out)[12]);

#endif // DEINTERLEAVE_H

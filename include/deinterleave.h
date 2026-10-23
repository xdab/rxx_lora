/**
 * @file deinterleave.h
 * @brief LoRa coding chain, symbol level: deinterleaving.
 */

#ifndef DEINTERLEAVE_H
#define DEINTERLEAVE_H

#include <stdint.h>

/**
 * @brief Deinterleave one block's bit LLRs: codeword r's chip i is the
 *        LLR of bit (i + sf_app - r - 1) mod sf_app (MSB-first) of
 *        gray-mapped symbol i.
 *
 * @param[in] sym_llr cw_len symbols' bit LLRs, [i][j] = LLR that bit j
 *                   (MSB first) of gray-mapped symbol i is 1 (from
 *                   demod_soft_llrs); inner stride 12 (max SF)
 * @param[in] cw_len symbols per block: 8 (header) or cr+4 (payload)
 * @param[in] sf_app codewords out: sf-2 (header) or sf (payload)
 * @param[out] cw_out sf_app codewords' chip LLRs, [r][i] MSB-first
 */
void deinterleave_soft(
    const float (*sym_llr)[12], uint8_t cw_len, uint8_t sf_app, float (*cw_out)[12]);

#endif // DEINTERLEAVE_H

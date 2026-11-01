/*
 * Hamming decode of the coding chain; line references point at
 * [gr-lora_sdr] hamming_dec_impl.cc.
 */

#include "hamming.h"
#include <stdint.h>

// bit k (MSB first) of a cw_len-bit codeword
#define CODEWORD_BIT(v, k, cw_len) (((v) >> ((cw_len) - 1 - (k))) & 1u)

// Reference: [gr-lora_sdr] hamming_enc_impl.cc:96-115
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // mirrors [gr-lora_sdr]
static uint8_t hamming_encode_nibble(uint8_t nib, uint8_t cr)
{
    // b0 = data MSB .. b3 = data LSB
    uint8_t b0 = (uint8_t)((nib >> 3) & 1u);
    uint8_t b1 = (uint8_t)((nib >> 2) & 1u);
    uint8_t b2 = (uint8_t)((nib >> 1) & 1u);
    uint8_t b3 = (uint8_t)(nib & 1u);

    if (cr == 1)
    {
        // 4/5: single total-parity bit
        uint8_t p4 = (uint8_t)(b0 ^ b1 ^ b2 ^ b3);
        return (uint8_t)((b3 << 4) | (b2 << 3) | (b1 << 2) | (b0 << 1) | p4);
    }

    uint8_t p0 = (uint8_t)(b3 ^ b2 ^ b1);
    uint8_t p1 = (uint8_t)(b2 ^ b1 ^ b0);
    uint8_t p2 = (uint8_t)(b3 ^ b2 ^ b0);
    uint8_t p3 = (uint8_t)(b3 ^ b1 ^ b0);
    return (uint8_t)(((b3 << 7) | (b2 << 6) | (b1 << 5) | (b0 << 4) | (p0 << 3) | (p1 << 2) |
                      (p2 << 1) | p3) >>
                     (4 - cr));
}

/* Decode-side LUT: per coding rate and candidate, the codeword bits as ±1
 * weights (MSB-first, 1 = bit set). Built once from the encoder, the score is
 * then a plain dot product — no per-call re-encoding, no per-bit branch. */
static int8_t hamming_signs[5][16][8];

static void hamming_signs_init(void)
{
    for (uint8_t cr = 1; cr <= 4; cr++)
    {
        uint8_t cw_len = (uint8_t)(cr + 4);
        for (uint8_t nib = 0; nib < 16; nib++)
        {
            uint8_t cw = hamming_encode_nibble(nib, cr);
            for (uint8_t k = 0; k < cw_len; k++)
                hamming_signs[cr][nib][k] = CODEWORD_BIT(cw, k, cw_len) ? 1 : -1;
        }
    }
}

uint8_t hamming_decode_soft(const float *llr, uint8_t cr, uint8_t *alt, float *margin)
{
    static int initialized = 0;
    if (!initialized)
    {
        hamming_signs_init();
        initialized = 1;
    }

    uint8_t cw_len = (uint8_t)(cr + 4);
    uint8_t best_nib = 0, alt_nib = 0;
    float best_score = -1e30f, alt_score = -1e30f; // |score| <= 2*sum|llr|, far below

    const int8_t (*rows)[8] = hamming_signs[cr];
    for (uint8_t nib = 0; nib < 16; nib++)
    {
        const int8_t *sg = rows[nib];
        float score = 0.0f;
        for (uint8_t k = 0; k < cw_len; k++)
            score += (float)sg[k] * llr[k];
        if (score > best_score)
        {
            alt_score = best_score;
            alt_nib = best_nib;
            best_score = score;
            best_nib = nib;
        }
        else if (score > alt_score)
        {
            alt_score = score;
            alt_nib = nib;
        }
    }

    if (alt)
        *alt = alt_nib;
    if (margin)
        *margin = best_score - alt_score;
    return best_nib;
}

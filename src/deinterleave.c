#include "deinterleave.h"
#include <stdint.h>

void deinterleave_soft(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // mirrors [gr-lora_sdr]
    const float (*sym_llr)[12],
    uint8_t cw_len,
    uint8_t sf_app,
    float (*cw_out)[12])
{
    for (uint8_t r = 0; r < sf_app; r++)
        for (uint8_t i = 0; i < cw_len; i++)
            cw_out[r][i] = sym_llr[i][(uint8_t)((i + sf_app - r - 1) % sf_app)];
}

/*
 * LoRa helpers; line references point at [gr-lora_sdr] utilities.h.
 */

#include "utilities.h"

#include <complex.h>
#include <math.h>
#include <stdint.h>

// Reference: [gr-lora_sdr] utilities.h:50
long util_mod(long a, long b) { return (a % b + b) % b; }

float util_modf(float v, uint32_t n)
{
    v = fmodf(v, (float)n);
    if (v < 0.0f)
        v += (float)n;
    return v;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // (value, modulus)
float util_wrap(float v, uint32_t n)
{
    float m = (float)n;
    v = fmodf(v + m / 2.0f, m);
    if (v < 0.0f)
        v += m;
    return v - m / 2.0f;
}

// Reference: [gr-lora_sdr] frame_sync_impl.cc:114
int util_round(float number) { return (int)(number > 0 ? number + 0.5f : ceilf(number - 0.5f)); }

// Reference: [gr-lora_sdr] utilities.h:95; [gr-lora] demod_impl.cc:96-101
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // mirrors [gr-lora_sdr]
void util_build_upchirp(float complex *chirp, uint32_t id, uint8_t sf)
{
    double n = 1u << sf;
    double n_fold = n - id;
    for (uint32_t i = 0; i < (uint32_t)n; i++)
    {
        double phase;
        // the (double)i casts are load-bearing: uncast, `i * i` is an integer
        // multiply that wraps past i = 65535 and corrupts the chirp
        if ((double)i < n_fold)
            phase = 2.0 * M_PI * ((double)i * i / (2.0 * n) + ((double)id / n - 0.5) * i);
        else
            phase = 2.0 * M_PI * ((double)i * i / (2.0 * n) + ((double)id / n - 1.5) * i);
        chirp[i] = cexp(I * phase);
    }
}

// Reference: [gr-lora_sdr] utilities.h:117; [gr-lora] demod_impl.cc:96-101
void util_build_ref_chirps(float complex *upchirp, float complex *downchirp, uint8_t sf)
{
    util_build_upchirp(upchirp, 0, sf);
    for (uint32_t k = 0; k < (1u << sf); k++)
        downchirp[k] = conjf(upchirp[k]);
}

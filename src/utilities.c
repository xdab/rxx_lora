/*
 * LoRa helpers; line references point at [gr-lora_sdr] utilities.h.
 */

#include "utilities.h"

#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

// Reference: [gr-lora_sdr] utilities.h:50
long util_mod(long a, long b) { return (a % b + b) % b; }

// Reference: [gr-lora_sdr] frame_sync_impl.cc:114
int util_round(float number) { return (int)(number > 0 ? number + 0.5f : ceilf(number - 0.5f)); }

// Reference: [gr-lora_sdr] utilities.h:95; [gr-lora] demod_impl.cc:96-101
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // mirrors [gr-lora_sdr]
void util_build_upchirp(float complex *chirp, uint32_t id, uint8_t sf, uint8_t os_factor)
{
    double n = 1u << sf;
    double n_fold = n * os_factor - id * os_factor;
    for (uint32_t i = 0; i < (uint32_t)n * os_factor; i++)
    {
        double phase;
        // the (double)i casts are load-bearing: uncast, `i * i` is an integer
        // multiply that wraps past i = 65535 and corrupts the chirp
        if ((double)i < n_fold)
            phase = 2.0 * M_PI *
                    ((double)i * i / (2.0 * n) / (os_factor * os_factor) +
                     ((double)id / n - 0.5) * i / os_factor);
        else
            phase = 2.0 * M_PI *
                    ((double)i * i / (2.0 * n) / (os_factor * os_factor) +
                     ((double)id / n - 1.5) * i / os_factor);
        chirp[i] = cexp(I * phase);
    }
}

// Reference: [gr-lora_sdr] utilities.h:117; [gr-lora] demod_impl.cc:96-101
void util_build_ref_chirps(
    float complex *upchirp, float complex *downchirp, uint8_t sf, uint8_t os_factor)
{
    util_build_upchirp(upchirp, 0, sf, os_factor);
    for (uint32_t n = 0; n < (1u << sf) * os_factor; n++)
        downchirp[n] = conjf(upchirp[n]);
}

// Reference: [gr-lora_sdr] utilities.h:129
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // n counts entries, bins sizes the histogram
int util_most_frequent(const int *arr, int n, uint32_t number_of_bins)
{
    int *hash = calloc(number_of_bins, sizeof(*hash));
    if (!hash)
        return -1;

    for (int i = 0; i < n; i++)
        hash[arr[i]]++;

    int max_count = 0, res = -1;
    for (uint32_t i = 0; i < number_of_bins; i++)
    {
        if (max_count < hash[i])
        {
            res = (int)i;
            max_count = hash[i];
        }
    }

    free(hash);
    return res;
}

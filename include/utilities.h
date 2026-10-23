/**
 * @file utilities.h
 * @brief Shared helpers.
 */

#ifndef UTILITIES_H
#define UTILITIES_H

#include <complex.h>
#include <stdint.h>

/**
 * @brief Modulus that is always non-negative: result in [0, b).
 * Reference: [gr-lora_sdr] utilities.h:50.
 */
long util_mod(long a, long b);

/**
 * @brief Round half away from zero, as the reference's my_roundf.
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:114.
 */
int util_round(float number);

/**
 * @brief Fill chirp with the oversampled upchirp of symbol value id
 *        (phase-continuous closed-form quadratic phase).
 *
 * @param[out] chirp buffer of (1 << sf) * os_factor samples
 *
 * Reference: [gr-lora_sdr] utilities.h:95; [gr-lora] demod_impl.cc:96-101.
 */
void util_build_upchirp(float complex *chirp, uint32_t id, uint8_t sf, uint8_t os_factor);

/**
 * @brief Build the reference chirp pair: base upchirp plus its conjugate
 *        (the demodulation downchirp). Buffers of (1 << sf) * os_factor.
 *
 * Reference: [gr-lora_sdr] utilities.h:117; [gr-lora] demod_impl.cc:96-101.
 */
void util_build_ref_chirps(
    float complex *upchirp, float complex *downchirp, uint8_t sf, uint8_t os_factor);

/**
 * @brief Most frequent value in arr (values in [0, number_of_bins)).
 *
 * @return the mode, or -1 on allocation failure
 */
int util_most_frequent(const int *arr, int n, uint32_t number_of_bins);

#endif // UTILITIES_H

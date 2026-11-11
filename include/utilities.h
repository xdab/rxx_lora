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
 * @brief util_mod() on floats: result in [0, n).
 */
float util_modf(float v, uint32_t n);

/**
 * @brief util_modf() centered on zero: result in (-n/2, n/2].
 */
float util_wrap(float v, uint32_t n);

/**
 * @brief Round half away from zero, as the reference's my_roundf.
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:114.
 */
int util_round(float number);

/**
 * @brief Fill chirp with the upchirp of symbol value id
 *        (phase-continuous closed-form quadratic phase).
 *
 * @param[out] chirp buffer of (1 << sf) samples
 *
 * Reference: [gr-lora_sdr] utilities.h:95; [gr-lora] demod_impl.cc:96-101.
 */
void util_build_upchirp(float complex *chirp, uint32_t id, uint8_t sf);

/**
 * @brief Build the reference chirp pair: base upchirp plus its conjugate
 *        (the demodulation downchirp). Buffers of (1 << sf).
 *
 * Reference: [gr-lora_sdr] utilities.h:117; [gr-lora] demod_impl.cc:96-101.
 */
void util_build_ref_chirps(float complex *upchirp, float complex *downchirp, uint8_t sf);

#endif // UTILITIES_H

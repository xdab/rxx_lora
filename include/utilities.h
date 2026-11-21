/**
 * @file utilities.h
 * @brief Shared helpers.
 */

#ifndef UTILITIES_H
#define UTILITIES_H

#include <complex.h>
#include <stdint.h>
#include <stdlib.h>

#include "logging.h"

/**
 * @brief Allocate @p count elements into @p ptr; on failure log what and
 *        which allocation it was, then jump to the enclosing function's
 *        `out` label — the shared cleanup convention: everything allocated
 *        so far is freed there, so all owned pointers must start NULL.
 */
#define SAFE_ALLOC(ptr, count, what)                                                               \
    do                                                                                             \
    {                                                                                              \
        (ptr) = malloc((size_t)(count) * sizeof(*(ptr)));                                          \
        if (!(ptr))                                                                                \
        {                                                                                          \
            log_error("%s: out of memory (%s)\n", (what), #ptr);                                   \
            goto out;                                                                              \
        }                                                                                          \
    } while (0)

/** @brief SAFE_ALLOC() with zeroed memory (calloc). */
#define SAFE_CALLOC(ptr, count, what)                                                              \
    do                                                                                             \
    {                                                                                              \
        (ptr) = calloc((size_t)(count), sizeof(*(ptr)));                                           \
        if (!(ptr))                                                                                \
        {                                                                                          \
            log_error("%s: out of memory (%s)\n", (what), #ptr);                                   \
            goto out;                                                                              \
        }                                                                                          \
    } while (0)

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

/** @file resample.h
 * @brief Streaming polyphase arbitrary-rate resampler.
 */

#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <complex.h>
#include <stddef.h>

/**
 * @brief Streaming polyphase arbitrary-rate resampler: feed it input chunks
 *        of any size, it emits completed output samples. State (filter
 *        memory, timing phase) carries across calls, so chunk boundaries do
 *        not affect the output. A fixed group delay of m input samples is
 *        dropped internally, keeping the output time-aligned with the input
 *        stream.
 */
struct resampler;

/**
 * @brief Create a resampler for rate_in -> rate_out (Hz), any positive
 *        ratio.
 *
 * @p bandwidth_hz is the signal bandwidth the filter must preserve: the
 * anti-alias cutoff is placed at half of it, so out-of-band noise is
 * rejected instead of handed to the chain.
 *
 * @param[in] rate_in input sample rate, Hz
 * @param[in] rate_out output sample rate, Hz
 * @param[in] bandwidth_hz signal bandwidth to preserve, Hz
 * @return the resampler, or NULL on allocation failure
 */
struct resampler *resampler_create(double rate_in, double rate_out, double bandwidth_hz);

/**
 * @brief Release the resampler.
 */
void resampler_destroy(struct resampler *rs);

/**
 * @brief Push n_in input samples; completed output samples go to out.
 *
 * Input is buffered internally, so n_in need not align with anything.
 *
 * @param[in] rs the resampler
 * @param[in] in input samples
 * @param[in] n_in input sample count
 * @param[out] out output buffer
 * @param[in] out_cap output capacity, sized once via resampler_push_space()
 * @return the number of samples written to out
 */
size_t resampler_push(
    struct resampler *rs, const float complex *in, size_t n_in, float complex *out, size_t out_cap);

/**
 * @brief Output space enough for resampler_push to always drain its input:
 *        ceil(rate * (n_in + 1)).
 *
 * @param[in] rs the resampler
 * @param[in] n_in the input sample count the buffer must absorb
 * @return the required output capacity
 */
size_t resampler_push_space(const struct resampler *rs, size_t n_in);

#endif

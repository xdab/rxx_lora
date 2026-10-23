#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <complex.h>
#include <stddef.h>

/**
 * Streaming polyphase arbitrary-rate resampler: feed it input chunks of any
 * size, it emits completed output samples. State (filter memory, timing
 * phase) carries across calls, so chunk boundaries do not affect the output.
 * A fixed group delay of m input samples is dropped internally, keeping the
 * output time-aligned with the input stream.
 */
struct resampler;

struct resampler;

/**
 * Resampler for rate_in -> rate_out (Hz), any positive ratio. `bandwidth_hz`
 * is the signal bandwidth the filter must preserve: the anti-alias cutoff is
 * placed at half of it, so out-of-band noise is rejected instead of handed to
 * the chain. Returns NULL on allocation failure.
 */
struct resampler *resampler_create(double rate_in, double rate_out, double bandwidth_hz);

void resampler_destroy(struct resampler *rs);

/**
 * Push n_in input samples; completed output samples go to out (capacity
 * out_cap, see resampler_push_space) and the count is returned. Input is
 * buffered internally, so n_in need not align with anything.
 */
size_t resampler_push(
    struct resampler *rs, const float complex *in, size_t n_in, float complex *out, size_t out_cap);

/**
 * Output space enough for resampler_push to always drain its input:
 * ceil(rate * (n_in + 1)). Exposed so callers can size buffers once.
 */
size_t resampler_push_space(const struct resampler *rs, size_t n_in);

#endif

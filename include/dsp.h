#ifndef DSP_H
#define DSP_H

#include <complex.h>
#include <stddef.h>

/**
 * Complex sample pair of the FFT buffers.
 */
typedef struct
{
    float r; // real part
    float i; // imaginary part
} dsp_cpx;

/** Opaque FFT plan (FFTW plan + planner arrays), one per transform size. */
typedef struct dsp_fft_state *dsp_fft_cfg;

/**
 * @brief Plan an N-point forward FFT. @p nfft must be a power of two
 *        (>= 2); returns NULL otherwise or on allocation failure.
 *
 * Reference: FFTW fftwf_plan_dft_1d().
 */
dsp_fft_cfg dsp_fft_alloc(int nfft);

/**
 * @brief Run one forward FFT: fout[k] = sum_j fin[j] * e^{-2*pi*i*j*k/n}.
 *        fin and fout must not alias.
 *
 * Reference: FFTW fftwf_execute_dft().
 */
void dsp_fft(dsp_fft_cfg cfg, const dsp_cpx *fin, dsp_cpx *fout);

/**
 * @brief Release a plan and its scratch buffers.
 */
void dsp_fft_free(dsp_fft_cfg cfg);

/**
 * Streaming first-order IIR DC blocker (one-pole high-pass):
 *
 *     y[n] = x[n] - x[n-1] + r*y[n-1],   r = 1 - 2*pi*corner/rate
 *
 * State carries across calls, so arbitrarily chunked input filters the same as
 * one block. Used at the head of the receive chain to strip the zero-IF tuner's
 * DC offset and 1/f junk before it reaches DETECT.
 */
struct dc_block
{
    float r;              // pole, 1 - 2*pi*corner/rate
    float complex x1, y1; // one-sample input and output memory
};

/**
 * @brief Initialise a DC blocker for samples at @p rate_hz with the -3 dB
 *        corner at @p corner_hz (both > 0, corner well below rate/2). The pole
 *        is clamped to [0, 1) so a bad corner can never destabilise the state.
 */
void dc_block_init(struct dc_block *d, double rate_hz, double corner_hz);

/**
 * @brief Filter @p n samples in place, advancing the blocker's state.
 */
void dc_block_apply(struct dc_block *d, float complex *x, size_t n);

/**
 * @brief Design an @p n-tap Kaiser-windowed sinc low-pass prototype, writing
 *        @p n coefficients to @p h (unnormalized; the caller applies any gain).
 *
 * h[i] = sinc(2*fc*(i-(n-1)/2)) * kaiser(i, n, beta(as)); @p fc is the cutoff
 * normalized to the sample rate, in (0, 0.5), and @p as the target stop-band
 * attenuation in dB.
 *
 * Reference: Kaiser window [Kaiser:1980]; taper parameter from [Kaiser:1974].
 */
void dsp_kaiser_lowpass(unsigned n, float fc, float as, float *h);

#endif // DSP_H

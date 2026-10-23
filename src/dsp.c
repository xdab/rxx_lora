/*
 * Complex one-dimensional FFT, a thin wrapper over FFTW3f; the streaming DC
 * blocker; and a Kaiser-windowed sinc low-pass prototype designer.
 *
 * Plans are created once per transform size on private planner arrays and
 * driven through fftwf_execute_dft(), so the caller's own buffers are
 * transformed directly. FFTW requires the arrays it is executed on to be
 * aligned like the planner's, hence the plain malloc() blocks on both sides;
 * they must outlive the plan and are released only in dsp_fft_free().
 */

#include "dsp.h"

#include <fftw3.h>
#include <math.h>
#include <stdlib.h>

// dsp_cpx and fftwf_complex are both two contiguous floats.
_Static_assert(sizeof(dsp_cpx) == sizeof(fftwf_complex), "dsp_cpx must match fftwf_complex layout");

struct dsp_fft_state
{
    unsigned int nfft;
    fftwf_plan plan;
    dsp_cpx *x; // planner input, referenced by the plan for its lifetime
    dsp_cpx *y; // planner output, referenced by the plan for its lifetime
};

dsp_fft_cfg dsp_fft_alloc(int nfft)
{
    if (nfft < 2 || (nfft & (nfft - 1)))
        return NULL; // power-of-two sizes only

    struct dsp_fft_state *st = calloc(1, sizeof(*st));
    if (!st)
        return NULL;
    st->nfft = (unsigned int)nfft;

    st->x = malloc((size_t)nfft * sizeof(*st->x));
    st->y = malloc((size_t)nfft * sizeof(*st->y));
    if (!st->x || !st->y)
    {
        dsp_fft_free(st);
        return NULL;
    }

    st->plan = fftwf_plan_dft_1d(
        (int)st->nfft, (fftwf_complex *)st->x, (fftwf_complex *)st->y, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!st->plan)
    {
        dsp_fft_free(st);
        return NULL;
    }
    return st;
}

void dsp_fft(dsp_fft_cfg cfg, const dsp_cpx *fin, dsp_cpx *fout)
{
    fftwf_execute_dft(cfg->plan, (fftwf_complex *)(void *)fin, (fftwf_complex *)fout);
}

void dsp_fft_free(dsp_fft_cfg cfg)
{
    if (!cfg)
        return;
    if (cfg->plan)
        fftwf_destroy_plan(cfg->plan);
    free(cfg->x);
    free(cfg->y);
    free(cfg);
}

/* Modified Bessel function of the first kind, order 0, via the Abramowitz &
 * Stegun 9.8.1/9.8.2 rational approximation (|error| < 1e-7 over the range the
 * Kaiser window uses). */
static float besseli0(float x)
{
    float ax = fabsf(x);

    if (ax < 3.75f)
    {
        float t = x / 3.75f;
        t *= t;
        return 1.0f +
               t * (3.5156229f +
                    t * (3.0899424f +
                         t * (1.2067492f + t * (0.2659732f + t * (0.0360768f + t * 0.0045813f)))));
    }

    float y = 3.75f / ax;
    return (expf(ax) / sqrtf(ax)) *
           (0.39894228f +
            y * (0.01328592f +
                 y * (0.00225319f +
                      y * (-0.00157565f +
                           y * (0.00916281f +
                                y * (-0.02057706f +
                                     y * (0.02635537f + y * (-0.01647633f + y * 0.00392377f))))))));
}

// sinc(x) = sin(pi*x)/(pi*x), with the small-argument product form for accuracy
static float sinc_pi(float x)
{
    if (fabsf(x) < 0.01f)
        return cosf(M_PI * x / 2.0f) * cosf(M_PI * x / 4.0f) * cosf(M_PI * x / 8.0f);
    return sinf(M_PI * x) / (M_PI * x);
}

// Kaiser taper parameter for a target stop-band attenuation (Kaiser:1974)
static float kaiser_beta(float as)
{
    as = fabsf(as);

    float beta;
    if (as > 50.0f)
        beta = 0.1102f * (as - 8.7f);
    else if (as > 21.0f)
        beta = 0.5842f * powf(as - 21.0f, 0.4f) + 0.07886f * (as - 21.0f);
    else
        beta = 0.0f;

    if (as > 110.0f) // adjustment for high stop-band designs
        beta *= 1.02f;

    return beta;
}

// Kaiser window sample i of n, taper beta (cf. Kaiser:1980)
static float kaiser_win(unsigned i, unsigned n, float beta)
{
    float t = (float)i - (float)(n - 1) / 2.0f;
    float r = 2.0f * t / (float)(n - 1);
    return besseli0(beta * sqrtf(1.0f - r * r)) / besseli0(beta);
}

void dsp_kaiser_lowpass(unsigned n, float fc, float as, float *h)
{
    float beta = kaiser_beta(as);

    for (unsigned i = 0; i < n; i++)
    {
        float t = (float)i - (float)(n - 1) / 2.0f;
        h[i] = sinc_pi(2.0f * fc * t) * kaiser_win(i, n, beta);
    }
}

void dc_block_init(struct dc_block *d, double rate_hz, double corner_hz)
{
    d->x1 = 0.0f;
    d->y1 = 0.0f;
    double r = 1.0 - 2.0 * M_PI * corner_hz / rate_hz;
    if (r < 0.0)
        r = 0.0;
    if (r > 0.999999)
        r = 0.999999;
    d->r = (float)r;
}

void dc_block_apply(struct dc_block *d, float complex *x, size_t n)
{
    float r = d->r;
    float complex x1 = d->x1, y1 = d->y1;
    for (size_t i = 0; i < n; i++)
    {
        float complex xn = x[i];
        y1 = xn - x1 + r * y1;
        x1 = xn;
        x[i] = y1;
    }
    d->x1 = x1;
    d->y1 = y1;
}

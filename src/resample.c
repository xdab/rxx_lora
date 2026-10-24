#include <complex.h>
#include <math.h>
#include <stdlib.h>

#include "dsp.h"
#include "logging.h"
#include "resample.h"

struct resampler
{
    unsigned m;           // prototype semi-length: taps per branch = 2m
    unsigned npfb;        // number of polyphase branches
    unsigned h_sub;       // taps per branch = 2m
    float *coef;          // npfb*h_sub branch taps, row i oldest-tap-first
    float complex *w;     // delay line, w[0] oldest .. w[h_sub-1] newest
    float rate;           // output/input sample rate
    float del;            // timing step per output = 1/rate
    float tau;            // accumulated timing phase
    float bf;             // soft branch index = tau*npfb
    int b;                // base branch index floor(bf)
    float mu;             // fractional branch index bf-b
    int state;            // RESAMP_BOUNDARY or RESAMP_INTERP
    float complex y0, y1; // branch outputs bracketing the current phase
    size_t delay;         // leading output samples dropped (the group delay)
    size_t drained;       // output samples emitted so far, delay included
    float complex *block; // one input sample's outputs
};

enum
{
    RESAMP_BOUNDARY, // boundary between input samples
    RESAMP_INTERP,   // regular interpolation
};

// One polyphase branch: FIR of the decimated prototype against the delay line
static float complex branch_out(const struct resampler *rs, int i)
{
    const float *h = rs->coef + (size_t)i * rs->h_sub;
    float complex acc = 0;

    for (unsigned k = 0; k < rs->h_sub; k++)
        acc += h[k] * rs->w[k];

    return acc;
}

static void push_sample(struct resampler *rs, float complex x)
{
    for (unsigned k = 0; k + 1 < rs->h_sub; k++)
        rs->w[k] = rs->w[k + 1];
    rs->w[rs->h_sub - 1] = x;
}

// Advance the fractional timing phase by one output sample
static void update_timing(struct resampler *rs)
{
    rs->tau += rs->del;
    rs->bf = rs->tau * (float)rs->npfb;
    rs->b = (int)floorf(rs->bf);
    rs->mu = rs->bf - (float)rs->b;
}

/* Run the resampler on one input sample, writing the completed outputs (at
 * most ceil(rate)+1) to y and returning their count. */
static unsigned execute(struct resampler *rs, float complex x, float complex *y)
{
    push_sample(rs, x);
    unsigned n = 0;

    while (rs->b < (int)rs->npfb)
    {
        if (rs->state == RESAMP_BOUNDARY)
        {
            rs->y1 = branch_out(rs, 0);
            y[n++] = (1.0f - rs->mu) * rs->y0 + rs->mu * rs->y1;
            update_timing(rs);
            rs->state = RESAMP_INTERP;
        }
        else
        {
            rs->y0 = branch_out(rs, rs->b);

            if (rs->b == (int)rs->npfb - 1)
            {
                // last branch: the next sample must arrive to interpolate
                rs->state = RESAMP_BOUNDARY;
                rs->b = (int)rs->npfb;
            }
            else
            {
                rs->y1 = branch_out(rs, rs->b + 1);
                y[n++] = (1.0f - rs->mu) * rs->y0 + rs->mu * rs->y1;
                update_timing(rs);
            }
        }
    }

    rs->tau -= 1.0f;
    rs->bf -= (float)rs->npfb;
    rs->b -= (int)rs->npfb;

    return n;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // directional (rate_in -> rate_out) pair
struct resampler *resampler_create(double rate_in, double rate_out, double bandwidth_hz)
{
    if (!(rate_in > 0.0) || !(rate_out > 0.0))
    {
        log_error("resampler: invalid rates %.6f -> %.6f Hz\n", rate_in, rate_out);
        return NULL;
    }

    struct resampler *rs = calloc(1, sizeof(*rs));
    if (!rs)
        return NULL;

    /* Anti-alias cutoff at half the signal bandwidth, relative to the input
     * rate: the resampler is the only filter before detect, so out-of-band
     * noise is rejected here. It must stay below the decimation Nyquist. */
    float rate = (float)(rate_out / rate_in);
    float bw_rel = (float)(bandwidth_hz / rate_in);

    float fc_max = 0.5f * (rate < 1.0f ? rate : 1.0f);
    if (fc_max > 0.499f)
        fc_max = 0.499f;

    unsigned m = 4;
    unsigned as = 60;
    unsigned npfb = 64;

#if RXX_EXPERIMENTAL // env-var overrides for filter experiments
    {
        const char *e = getenv("RESAMP_M");
        if (e)
            m = (unsigned)atoi(e);
        e = getenv("RESAMP_AS");
        if (e)
            as = (unsigned)atof(e);
        e = getenv("RESAMP_NPFB");
        if (e)
            npfb = (unsigned)atoi(e);
        e = getenv("RESAMP_BW");
        if (e)
            bw_rel = (float)atof(e);
    }
#endif

    float fc = 0.5f * bw_rel;
    if (!(fc > 0.0f) || fc > fc_max)
        fc = fc_max;

    if (m < 1 || npfb < 1)
    {
        log_error("resampler: m and npfb must be positive (m %u, npfb %u)\n", m, npfb);
        free(rs);
        return NULL;
    }

    /* Kaiser-windowed sinc prototype, npfb times oversampled: the cutoff is
     * fc/npfb at the interpolated rate. Branch i's taps are the prototype
     * decimated by npfb, oldest tap first. */
    rs->m = m;
    rs->npfb = npfb;
    rs->h_sub = 2 * m;
    rs->rate = rate;
    rs->del = 1.0f / rate;
    rs->state = RESAMP_INTERP; // start mid-bank, like a fresh arbitrary resampler

    unsigned n = 2 * m * npfb + 1;
    float *hf = malloc(n * sizeof(*hf));
    if (!hf)
    {
        free(rs);
        return NULL;
    }

    dsp_kaiser_lowpass(n, fc / (float)npfb, (float)as, hf);

    float sum = 0.0f;
    for (unsigned j = 0; j < n; j++)
        sum += hf[j];

    float gain = (float)npfb / sum; // normalize the branch bank to unit DC gain

    rs->coef = malloc((size_t)npfb * rs->h_sub * sizeof(*rs->coef));
    rs->w = calloc(rs->h_sub, sizeof(*rs->w));
    rs->block = malloc((size_t)((unsigned)ceil(rate) + 2) * sizeof(*rs->block));
    if (!rs->coef || !rs->w || !rs->block)
    {
        free(hf);
        resampler_destroy(rs);
        return NULL;
    }

    for (unsigned i = 0; i < npfb; i++)
        for (unsigned k = 0; k < rs->h_sub; k++)
            rs->coef[(size_t)i * rs->h_sub + k] = hf[i + (rs->h_sub - 1 - k) * npfb] * gain;

    free(hf);

    rs->delay = (size_t)(0.5 + (double)rs->m * (double)rate);

    log_info(
        "resampler: %.0f -> %.0f Hz (rate %.6f, m %u, cutoff %.4f, delay %zu)\n",
        rate_in,
        rate_out,
        rate,
        rs->m,
        fc,
        rs->delay);

    return rs;
}

void resampler_destroy(struct resampler *rs)
{
    if (!rs)
        return;
    free(rs->coef);
    free(rs->w);
    free(rs->block);
    free(rs);
}

size_t resampler_push_space(const struct resampler *rs, size_t n_in)
{
    return (size_t)ceil((double)rs->rate * (double)(n_in + 1));
}

size_t resampler_push(
    struct resampler *rs, const float complex *in, size_t n_in, float complex *out, size_t out_cap)
{
    size_t n = 0;

    for (size_t i = 0; i < n_in; i++)
    {
        unsigned nw = execute(rs, in[i], rs->block);

        for (unsigned k = 0; k < nw; k++)
        {
            if (rs->drained++ < rs->delay) // drop the group delay once
                continue;

            if (n < out_cap) // the caller sizes out via resampler_push_space
                out[n++] = rs->block[k];
        }
    }

    return n;
}

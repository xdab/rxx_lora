#include <complex.h>
#include <math.h>
#include <stdlib.h>

#include "dsp.h"
#include "logging.h"
#include "resample.h"

struct resampler
{
    unsigned m;       // prototype semi-length: taps per branch = 2m
    unsigned npfb;    // number of polyphase branches
    unsigned h_sub;   // taps per branch = 2m
    float *coef;      // npfb*h_sub branch taps, row i oldest-tap-first
    float complex *w; // circular delay line, mirrored to 2*h_sub slots: every
                      // push writes both w[pos] and w[pos+h_sub], so a window
                      // (w_pos .. w_pos+h_sub) never wraps and reads linearly
    unsigned w_pos;   // delay-line write position; slot w_pos is the oldest
    float rate;       // output/input sample rate
    float del;        // timing step per output = 1/rate
    float tau;        // accumulated timing phase
    float bf;         // soft branch index = tau*npfb
    int b;            // base branch index floor(bf)
    float mu;         // fractional branch index bf-b
    int state;        // RESAMP_BOUNDARY or RESAMP_INTERP
    float complex (*f_out)(const struct resampler *, int);        // branch kernel pair,
    float complex (*f_mix)(const struct resampler *, int, float); // h_sub-specialized
    float complex y0, y1; // branch outputs bracketing the current phase
    size_t delay;         // leading output samples dropped (the group delay)
    size_t drained;       // leading outputs seen; only read during the
                          // group-delay drop, frozen once it drains
    float complex *block; // one input sample's outputs
};

enum
{
    RESAMP_BOUNDARY, // boundary between input samples
    RESAMP_INTERP,   // regular interpolation
};

/* Fixed-8-tap kernels: constant bound, contiguous window (the mirrored delay
 * line below makes (p + k) wrap-free); the generic loops cover every other
 * h_sub. */
static float complex branch_out_8(const struct resampler *rs, int i)
{
    const float *h = rs->coef + (size_t)i * 8;
    const float complex *x = rs->w + rs->w_pos;
    float complex acc = 0;
    for (unsigned k = 0; k < 8; k++)
        acc += h[k] * x[k];
    return acc;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // (branch, mix) distinct types at call sites
static float complex branch_mix_8(const struct resampler *rs, int i, float mu)
{
    const float *h0 = rs->coef + (size_t)i * 8;
    const float *h1 = h0 + 8;
    const float complex *x = rs->w + rs->w_pos;
    float complex acc = 0;
    for (unsigned k = 0; k < 8; k++)
    {
        float h = h0[k] + mu * (h1[k] - h0[k]);
        acc += h * x[k];
    }
    return acc;
}

// One polyphase branch: FIR of the decimated prototype against the delay line
static float complex branch_out(const struct resampler *rs, int i)
{
    const float *h = rs->coef + (size_t)i * rs->h_sub;
    const float complex *x = rs->w + rs->w_pos;
    float complex acc = 0;

    for (unsigned k = 0; k < rs->h_sub; k++)
        acc += h[k] * x[k];

    return acc;
}

/* Branch pair (i, i+1) mixed at mu, then one FIR pass: the linear output
 * interpolation of the arbitrary phase is algebraically the same MAC over
 * the interpolated taps — half the complex work, exact (no mu quantize).
 * Only valid for i + 1 < npfb (the INTERP path; the BOUNDARY pair spans
 * executes and keeps the two-branch form). */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // (branch, mix) distinct types at call sites
static float complex branch_mix(const struct resampler *rs, int i, float mu)
{
    const float *h0 = rs->coef + (size_t)i * rs->h_sub;
    const float *h1 = h0 + rs->h_sub;
    const float complex *x = rs->w + rs->w_pos;
    float complex acc = 0;

    for (unsigned k = 0; k < rs->h_sub; k++)
    {
        float h = h0[k] + mu * (h1[k] - h0[k]);
        acc += h * x[k];
    }

    return acc;
}

static void push_sample(struct resampler *rs, float complex x)
{
    rs->w[rs->w_pos] = x;             // overwrite the oldest slot; it becomes the newest
    rs->w[rs->w_pos + rs->h_sub] = x; // mirror: keeps (w_pos .. +h_sub) linear
    if (++rs->w_pos == rs->h_sub)
        rs->w_pos = 0;
}

// Advance the fractional timing phase by one output sample
static inline void update_timing(struct resampler *rs, float *tau, float *bf, int *b, float *mu)
{
    *tau += rs->del;
    *bf = *tau * (float)rs->npfb;
    // bf >= 0 on every path here, so a truncating cast is floor
    *b = (int)*bf;
    *mu = *bf - (float)*b;
}

/* Run the resampler on one input sample, writing the completed outputs (at
 * most ceil(rate)+1) to y and returning their count. */
static unsigned execute(struct resampler *rs, float complex x, float complex *y)
{
    push_sample(rs, x);
    unsigned n = 0;
    float tau = rs->tau, bf = rs->bf, mu = rs->mu;
    int b = rs->b;

    while (b < (int)rs->npfb)
    {
        if (rs->state == RESAMP_BOUNDARY)
        {
            rs->y1 = rs->f_out(rs, 0);
            y[n++] = (1.0f - mu) * rs->y0 + mu * rs->y1;
            update_timing(rs, &tau, &bf, &b, &mu);
            rs->state = RESAMP_INTERP;
        }
        else
        {
            if (b == (int)rs->npfb - 1)
            {
                // last branch: the next sample must arrive to interpolate
                rs->y0 = rs->f_out(rs, b);
                rs->state = RESAMP_BOUNDARY;
                b = (int)rs->npfb;
            }
            else
            {
                y[n++] = rs->f_mix(rs, b, mu);
                update_timing(rs, &tau, &bf, &b, &mu);
            }
        }
    }

    tau -= 1.0f;
    bf -= (float)rs->npfb;
    b -= (int)rs->npfb;
    rs->tau = tau;
    rs->bf = bf;
    rs->b = b;
    rs->mu = mu;

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
    if (rs->h_sub == 8)
    {
        rs->f_out = branch_out_8;
        rs->f_mix = branch_mix_8;
    }
    else
    {
        rs->f_out = branch_out;
        rs->f_mix = branch_mix;
    }

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
    rs->w = calloc((size_t)2 * rs->h_sub, sizeof(*rs->w)); // mirrored delay line
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

        if (rs->drained >= rs->delay)
        { // steady state: no per-output bookkeeping
            for (unsigned k = 0; k < nw; k++)
                if (n < out_cap) // the caller sizes out via resampler_push_space
                    out[n++] = rs->block[k];
        }
        else
        { // leading group delay, dropped once
            for (unsigned k = 0; k < nw; k++)
            {
                if (rs->drained++ < rs->delay)
                    continue;
                if (n < out_cap)
                    out[n++] = rs->block[k];
            }
        }
    }

    return n;
}

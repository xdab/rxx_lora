/*
 * Hard symbol demodulator; line references point at [gr-lora_sdr] fft_demod_impl.cc.
 */

#include "demod.h"

#include "dsp.h"
#include "logging.h"
#include "utilities.h"
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "volk.h"

/* Reference: [gr-lora_sdr] fft_demod_impl.cc:26,283-289; [gr-lora] demod_impl.cc:54,96-101. */
int demod_init(struct demod_state *s, uint8_t sf)
{
    memset(s, 0, sizeof(*s));
    s->spreading_factor = sf;
    s->number_of_bins = 1u << sf;

    s->upchirp = malloc(s->number_of_bins * sizeof(*s->upchirp));
    s->downchirp = malloc(s->number_of_bins * sizeof(*s->downchirp));
    s->cx_in = malloc(s->number_of_bins * sizeof(*s->cx_in));
    s->cx_out = malloc(s->number_of_bins * sizeof(*s->cx_out));
    s->fft_mag = malloc(s->number_of_bins * sizeof(*s->fft_mag));
    s->llr_mask = malloc((size_t)sf * s->number_of_bins * sizeof(*s->llr_mask));
    s->llr_w = malloc(s->number_of_bins * sizeof(*s->llr_w));
    if (!s->upchirp || !s->downchirp || !s->cx_in || !s->cx_out || !s->fft_mag || !s->llr_mask ||
        !s->llr_w)
    {
        log_error("demod_init: out of memory\n");
        demod_cleanup(s);
        return -1;
    }

    /* Soft-LLR bin masks: for each bit j (MSB first), 1.0 on the bins whose
     * gray-mapped symbol value carries that bit. The half-space sums become
     * dot products of the weight vector against these masks; the mapping is
     * fixed, so they are built once here. */
    for (uint8_t j = 0; j < sf; j++)
    {
        uint32_t shift = sf - 1 - j;
        float *row = s->llr_mask + (size_t)j * s->number_of_bins;
        for (uint32_t b = 0; b < s->number_of_bins; b++)
        {
            uint32_t g = (b + s->number_of_bins - 1) & (s->number_of_bins - 1);
            uint32_t v = g ^ (g >> 1);
            row[b] = ((v >> shift) & 1u) ? 1.0f : 0.0f;
        }
    }

    s->fft_cfg = dsp_fft_alloc((int)s->number_of_bins);
    if (!s->fft_cfg)
    {
        log_error("demod_init: dsp_fft_alloc failed\n");
        demod_cleanup(s);
        return -1;
    }

    demod_reset(s, 0.0f);
    return 0;
}

/* Reference: [gr-lora_sdr] fft_demod_impl.cc:26,283-289; [gr-lora] demod_impl.cc:54,96-101.
 *
 * cfo arrives as one value in bins and is split here: the upchirp rotation is
 * a whole-sample cyclic shift, so it takes the rounded part, and the leftover
 * fraction becomes a per-sample phase twist. */
void demod_reset(struct demod_state *s, float cfo)
{
    int cfo_bins = util_round(cfo);
    float cfo_rest = cfo - (float)cfo_bins;

    util_build_upchirp(
        s->upchirp, util_mod(cfo_bins, (long)s->number_of_bins), s->spreading_factor);
    for (uint32_t n = 0; n < s->number_of_bins; n++)
        s->downchirp[n] = conjf(s->upchirp[n]);

    for (uint32_t n = 0; n < s->number_of_bins; n++)
        s->downchirp[n] *=
            cexpf(I * (-2.0f * (float)M_PI * cfo_rest / (float)s->number_of_bins * (float)n));

    log_trace(
        "demod LO: cfo=%+.4f bins -> rotate(%d) + twist(%+.4f)\n",
        (double)cfo,
        cfo_bins,
        (double)cfo_rest);
}

// Reference: [gr-lora_sdr] fft_demod_impl.cc:62; [gr-lora] demod_impl.cc:109
void demod_cleanup(struct demod_state *s)
{
    free(s->upchirp);
    free(s->downchirp);
    free(s->cx_in);
    free(s->cx_out);
    free(s->fft_mag);
    free(s->llr_mask);
    free(s->llr_w);
    if (s->fft_cfg)
        dsp_fft_free(s->fft_cfg);
    memset(s, 0, sizeof(*s));
}

#define DEMOD_SNR_BLANK 2 // bins blanked each side of the tone (CFO/STO skirt)

/* On-channel SNR of the last window; see demod.h. Not in the reference.
 * The dechirp folds the spreading gain into the tone bin (10*log10(N) dB
 * above the on-channel SNR); dividing by N undoes it. */
float demod_snr(const struct demod_state *s)
{
    float noise = s->noise_floor, sig;

    if (noise <= 0)
        return 0;

    sig = s->peak_mag - noise; // tone bin minus its own noise floor

    return sig > 0 ? sig / ((float)s->number_of_bins * noise) : 0;
}

// Reference: [gr-lora_sdr] fft_demod_impl.cc:262,301; [gr-lora] demod_impl.cc:174-206
int demod_step(struct demod_state *s, const float complex *in)
{
    // deviation: the reference dechirps into a separate buffer (volk) and
    // converts it to the FFT input in a second pass — same values, one pass;
    // dsp_cpx is layout-identical to lv_32fc_t (two contiguous floats)
    volk_32fc_x2_multiply_32fc((lv_32fc_t *)(void *)s->cx_in, in, s->downchirp, s->number_of_bins);

    dsp_fft(s->fft_cfg, s->cx_in, s->cx_out);

    /* Two passes, no modulo: magnitude + peak + runner-up, then the noise
     * sum walking n-5 bins from peak+3 (wrapping) — the tone and its skirt
     * must not inflate the estimate, and summing the tone in to subtract it
     * later would cancel catastrophically on strong clean tones. The floor
     * is cached; demod_snr() and demod_soft_llrs() never rescan. */
    volk_32fc_magnitude_squared_32f(s->fft_mag, (lv_32fc_t *)(void *)s->cx_out, s->number_of_bins);

    float mmax = 0, m2 = -1.0f;
    int max = 0;
    for (uint32_t i = 0; i < s->number_of_bins; i++)
    {
        float m = s->fft_mag[i];
        if (m > mmax)
        {
            m2 = mmax;
            mmax = m;
            max = (int)i;
        }
        else if (m > m2)
        {
            m2 = m;
        }
    }
    uint32_t n = s->number_of_bins;
    /* The blanked skirt (2*BLANK+1 bins centered on the tone) splits the
     * non-blanked bins into at most two contiguous runs; accumulate the
     * runs directly — the tone bin must never enter the sum, or float
     * rounding of the huge tone absorbs the noise floor entirely. */
    uint32_t start = (uint32_t)((max + DEMOD_SNR_BLANK + 1) % (int)n);
    uint32_t cnt = n - (2 * DEMOD_SNR_BLANK + 1);
    float nsum = 0.0f, part;
    if (start + cnt <= n)
    {
        volk_32f_accumulator_s32f(&nsum, s->fft_mag + start, cnt);
    }
    else
    {
        volk_32f_accumulator_s32f(&part, s->fft_mag + start, n - start);
        volk_32f_accumulator_s32f(&nsum, s->fft_mag, start + cnt - n);
        nsum += part;
    }
    s->symbol_val = (int)util_mod(max - 1, s->number_of_bins);
    s->peak_mag = mmax;
    s->noise_floor = nsum / (float)(n - (2 * DEMOD_SNR_BLANK + 1));

    return s->symbol_val;
}

#define DEMOD_SOFT_EPS 1e-12f

/* Soft-output twin of the argmax above; see demod.h. Not in the reference.
 *
 * The bin magnitudes are sufficient statistics for the symbol value. The
 * noncoherent metric uses a = (m_max - floor)/(m_max*floor), floor = the
 * blanked noise-floor mean cached by demod_step() (the tone and its skirt
 * must not inflate it) — the per-window tone SNR's λ/(1+λ), so it
 * self-calibrates.
 * Bit marginals sum the bins whose gray-mapped value carries that bit:
 *   llr[j] = log( (sum_{v: bit j = 1} e^{a(m-m_max)} + eps) /
 *                 (sum_{v: bit j = 0} e^{a(m-m_max)} + eps) )
 * eps (1e-12, capping |llr| at ~28) keeps a clean window finite. Valid only
 * right after demod_step() (fft_mag is overwritten by the next call). */
void demod_soft_llrs(struct demod_state *s, float *llr)
{
    float mmax = s->peak_mag, noise = s->noise_floor;
    uint32_t n = s->number_of_bins;
    const float *mask;
    uint8_t j;
    float a, wtot, s1;

    // a = lambda/(1+lambda) in floor units; 0 (no information) when the
    // spectrum is flat or silent
    a = (mmax > noise && noise > 0.0f) ? (mmax - noise) / (mmax * noise) : 0.0f;

    for (uint32_t b = 0; b < n; b++)
        s->llr_w[b] = expf(a * (s->fft_mag[b] - mmax));

    /* The bin->bit masks are prebuilt per bit (see demod_init), so each
     * half-space sum is a dot of the weights against a 0/1 vector; the
     * opposite half is the total minus that dot (the weights are >= 0, and
     * the eps below bounds the path where the subtraction loses the last
     * ulps of a vanishing half-sum). */
    volk_32f_accumulator_s32f(&wtot, s->llr_w, n);
    for (j = 0; j < s->spreading_factor; j++)
    {
        mask = s->llr_mask + (size_t)j * n;
        volk_32f_x2_dot_prod_32f(&s1, s->llr_w, mask, n);
        llr[j] = logf((s1 + DEMOD_SOFT_EPS) / (wtot - s1 + DEMOD_SOFT_EPS));
    }
}

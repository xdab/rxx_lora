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

/* Reference: [gr-lora_sdr] fft_demod_impl.cc:26,283-289; [gr-lora] demod_impl.cc:54,96-101. */
int demod_init(struct demod_state *s, uint8_t sf)
{
    memset(s, 0, sizeof(*s));
    s->spreading_factor = sf;
    s->number_of_bins = 1u << sf;
    s->samples_per_symbol = s->number_of_bins;

    s->upchirp = malloc(s->number_of_bins * sizeof(*s->upchirp));
    s->downchirp = malloc(s->number_of_bins * sizeof(*s->downchirp));
    s->cx_in = malloc(s->number_of_bins * sizeof(*s->cx_in));
    s->cx_out = malloc(s->number_of_bins * sizeof(*s->cx_out));
    s->fft_mag = malloc(s->number_of_bins * sizeof(*s->fft_mag));
    if (!s->upchirp || !s->downchirp || !s->cx_in || !s->cx_out || !s->fft_mag)
    {
        log_error("demod_init: out of memory\n");
        demod_cleanup(s);
        return -1;
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
        s->upchirp, util_mod(cfo_bins, (long)s->number_of_bins), s->spreading_factor, 1);
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
    if (s->fft_cfg)
        dsp_fft_free(s->fft_cfg);
    memset(s, 0, sizeof(*s));
}

#define DEMOD_SNR_BLANK 2 // bins blanked each side of the tone (CFO/STO skirt)

/* On-channel SNR of the last window; see demod.h. Not in the reference.
 * The dechirp folds the spreading gain into the tone bin (10*log10(N) dB
 * above the on-channel SNR); dividing by N undoes it. The floor is the mean
 * of the bins outside a small blank around the tone. */
float demod_snr(const struct demod_state *s)
{
    uint32_t n = s->number_of_bins, b, peak = 0;
    float mmax = 0, floor_sum = 0, noise, sig;

    for (b = 0; b < n; b++)
        if (s->fft_mag[b] > mmax)
        {
            mmax = s->fft_mag[b];
            peak = b;
        }

    for (b = 0; b < n; b++)
    {
        uint32_t d = (b + n - peak) % n;
        if (d <= DEMOD_SNR_BLANK || d >= n - DEMOD_SNR_BLANK)
            continue;
        floor_sum += s->fft_mag[b];
    }

    noise = floor_sum / (float)(n - (2 * DEMOD_SNR_BLANK + 1));
    if (noise <= 0)
        return 0;

    sig = mmax - noise; // tone bin minus its own noise floor

    return sig > 0 ? sig / ((float)n * noise) : 0;
}

// Reference: [gr-lora_sdr] fft_demod_impl.cc:262,301; [gr-lora] demod_impl.cc:174-206
int demod_step(struct demod_state *s, const float complex *in)
{
    // deviation: the reference dechirps into a separate buffer (volk) and
    // converts it to the FFT input in a second pass — same values, one pass
    for (uint32_t i = 0; i < s->number_of_bins; i++)
    {
        float complex v = in[i] * s->downchirp[i];
        s->cx_in[i].r = crealf(v);
        s->cx_in[i].i = cimagf(v);
    }

    dsp_fft(s->fft_cfg, s->cx_in, s->cx_out);
    for (uint32_t i = 0; i < s->number_of_bins; i++)
        s->fft_mag[i] = s->cx_out[i].r * s->cx_out[i].r + s->cx_out[i].i * s->cx_out[i].i;

    int max = 0;
    for (uint32_t i = 1; i < s->number_of_bins; i++)
        if (s->fft_mag[i] > s->fft_mag[max])
            max = (int)i;
    s->bin_idx = max;
    s->symbol_val = (int)util_mod(max - 1, s->number_of_bins);

    { // TRACE: argmax bin, runner-up, margin (small ratio = noise-decided pick)
        static int call = 0;

        int second = (max == 0) ? 1 : 0;
        for (uint32_t i = 0; i < s->number_of_bins; i++)
            if ((int)i != max && s->fft_mag[i] > s->fft_mag[second])
                second = (int)i;

        log_trace(
            "demod[%d]: peak=%d mag=%.3e  2nd=%d mag=%.3e  ratio=%.4f"
            "  -> symbol=%d\n",
            call,
            max,
            sqrt(s->fft_mag[max]),
            second,
            sqrt(s->fft_mag[second]),
            sqrt(s->fft_mag[second] / s->fft_mag[max]),
            s->symbol_val);

        call++;
    }

    return s->symbol_val;
}

#define DEMOD_SOFT_EPS 1e-12f

/* Soft-output twin of the argmax above; see demod.h. Not in the reference.
 *
 * The bin magnitudes are sufficient statistics for the symbol value. The
 * noncoherent metric uses a = (m_max - floor)/(m_max*floor), floor = mean
 * bin energy — the per-window tone SNR's λ/(1+λ), so it self-calibrates.
 * Bit marginals sum the bins whose gray-mapped value carries that bit:
 *   llr[j] = log( (sum_{v: bit j = 1} e^{a(m-m_max)} + eps) /
 *                 (sum_{v: bit j = 0} e^{a(m-m_max)} + eps) )
 * eps (1e-12, capping |llr| at ~28) keeps a clean window finite. Valid only
 * right after demod_step() (fft_mag is overwritten by the next call). */
void demod_soft_llrs(struct demod_state *s, float *llr)
{
    float sum[12][2] = {{0}}; // per-bit half-space sums, [j][bit]
    float floor = 0.0f, mmax = 0.0f, a;
    uint32_t b;
    uint8_t j;

    for (b = 0; b < s->number_of_bins; b++)
    {
        floor += s->fft_mag[b];
        if (s->fft_mag[b] > mmax)
            mmax = s->fft_mag[b];
    }
    floor /= (float)s->number_of_bins;

    // a = lambda/(1+lambda) in floor units; 0 (no information) when the
    // spectrum is flat or silent
    a = (mmax > floor && floor > 0.0f) ? (mmax - floor) / (mmax * floor) : 0.0f;

    for (b = 0; b < s->number_of_bins; b++)
    {
        // tone at bin b means symbol value g = mod(b - 1, N); the bit the
        // deinterleaver reads belongs to the gray-mapped v = g ^ (g>>1)
        uint32_t g = (b + s->number_of_bins - 1) & (s->number_of_bins - 1);
        uint32_t v = g ^ (g >> 1);
        float w = expf(a * (s->fft_mag[b] - mmax));

        for (j = 0; j < s->spreading_factor; j++)
            sum[j][(v >> (s->spreading_factor - 1 - j)) & 1u] += w;
    }

    for (j = 0; j < s->spreading_factor; j++)
        llr[j] = logf((sum[j][1] + DEMOD_SOFT_EPS) / (sum[j][0] + DEMOD_SOFT_EPS));
}

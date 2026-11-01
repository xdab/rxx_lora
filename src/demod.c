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
    // converts it to the FFT input in a second pass — same values, one pass
    for (uint32_t i = 0; i < s->number_of_bins; i++)
    {
        float complex v = in[i] * s->downchirp[i];
        s->cx_in[i].r = crealf(v);
        s->cx_in[i].i = cimagf(v);
    }

    dsp_fft(s->fft_cfg, s->cx_in, s->cx_out);

    /* Two passes, no modulo: magnitude + peak + runner-up, then the noise
     * sum walking n-5 bins from peak+3 (wrapping) — the tone and its skirt
     * must not inflate the estimate, and summing the tone in to subtract it
     * later would cancel catastrophically on strong clean tones. The floor
     * is cached; demod_snr() and demod_soft_llrs() never rescan. */
    float mmax = 0, m2 = -1.0f;
    int max = 0, second = 0;
    for (uint32_t i = 0; i < s->number_of_bins; i++)
    {
        float m = s->cx_out[i].r * s->cx_out[i].r + s->cx_out[i].i * s->cx_out[i].i;
        s->fft_mag[i] = m;
        if (m > mmax)
        {
            m2 = mmax;
            second = max;
            mmax = m;
            max = (int)i;
        }
        else if (m > m2)
        {
            m2 = m;
            second = (int)i;
        }
    }
    uint32_t n = s->number_of_bins;
    uint32_t b = (uint32_t)((max + DEMOD_SNR_BLANK + 1) % (int)n);
    float nsum = 0;
    for (uint32_t k = 0; k < n - (2 * DEMOD_SNR_BLANK + 1); k++)
    {
        nsum += s->fft_mag[b];
        if (++b == n)
            b = 0;
    }
    s->bin_idx = max;
    s->symbol_val = (int)util_mod(max - 1, s->number_of_bins);
    s->peak_mag = mmax;
    s->noise_floor = nsum / (float)(n - (2 * DEMOD_SNR_BLANK + 1));
    s->second_idx = second;

    { // TRACE: argmax bin, runner-up, margin (small ratio = noise-decided pick)
        static int call = 0;

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
    float sum[12][2] = {{0}}; // per-bit half-space sums, [j][bit]
    float mmax = s->peak_mag, noise = s->noise_floor, a;
    uint32_t b;
    uint8_t j;

    // a = lambda/(1+lambda) in floor units; 0 (no information) when the
    // spectrum is flat or silent
    a = (mmax > noise && noise > 0.0f) ? (mmax - noise) / (mmax * noise) : 0.0f;

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

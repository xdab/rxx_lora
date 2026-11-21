#include "frame_sync.h"
#include "dsp.h"
#include "frame_sync_internal.h"

#include "logging.h"
#include "utilities.h"
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "volk.h"

static void frame_sync_cleanup(struct frame_sync *s);

struct frame_sync *frame_sync_create(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // mirrors the reference's constructor
    uint8_t spreading_factor,
    uint8_t oversampling_factor,
    uint16_t preamble_len,
    uint32_t sync_word,
    uint32_t center_freq,
    uint32_t bandwidth,
    uint8_t detect_avg)
{
    struct frame_sync *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->spreading_factor = spreading_factor;
    s->oversampling_factor = oversampling_factor;
    s->preamble_len = preamble_len;
    s->sync_words[0] = (uint16_t)((sync_word >> 4) << 3);
    s->sync_words[1] = (uint16_t)((sync_word & 0xf) << 3);
    s->center_freq = center_freq;
    s->bandwidth = bandwidth;
    s->number_of_bins = 1u << spreading_factor;
    s->samples_per_symbol = s->number_of_bins * oversampling_factor;
    s->n_up_req = preamble_len - 3;
    s->detect_avg = detect_avg < 1 ? 1 : detect_avg;
    s->phase = FS_DETECT;
    s->bin_idx = 0;
    s->symbol_cnt = 1;
    s->sto_frac = 0.0f;

    int ret = -1;

    SAFE_ALLOC(s->preamb_up_vals, s->n_up_req, "frame_sync_create");
    SAFE_ALLOC(s->mode_hash, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->upchirp, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->downchirp, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->in_down, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->out_win, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->preamble_raw, preamble_len * s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->net_id_samp, 2.5 * s->samples_per_symbol, "frame_sync_create");
    SAFE_ALLOC(s->cfo_correc_long, (s->n_up_req + 3) * s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->sfo_corr_vect, (s->n_up_req + 3) * s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->net_ids_samp_dec, 2 * s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->rot_tmp, s->samples_per_symbol, "frame_sync_create");
    SAFE_ALLOC(s->preamble_upchirps, preamble_len * s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->cfo_correc, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->symb_corr, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->est_dechirped, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->est_cx_in, 2 * s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->est_cx_out, 2 * s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->est_fft_mag, 2 * s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->est_fft_val, s->n_up_req * s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->est_k0, s->n_up_req, "frame_sync_create");
    SAFE_ALLOC(s->est_k0_mag, s->n_up_req, "frame_sync_create");
    SAFE_ALLOC(s->cx_in, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->cx_out, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->fft_mag, s->number_of_bins, "frame_sync_create");
    SAFE_ALLOC(s->detect_score, 2 * s->number_of_bins + 2, "frame_sync_create");
    SAFE_ALLOC(s->netid_mask, s->number_of_bins, "frame_sync_create");
    if (s->detect_avg > 1)
    {
        SAFE_ALLOC(s->detect_mag_hist, s->detect_avg * s->number_of_bins, "frame_sync_create");
        SAFE_CALLOC(s->detect_mag_sum, s->number_of_bins, "frame_sync_create");
    }

    memset(s->preamb_up_vals, 0, s->n_up_req * sizeof(*s->preamb_up_vals));
    util_build_ref_chirps(s->upchirp, s->downchirp, spreading_factor);

    s->fft_cfg = dsp_fft_alloc((int)s->number_of_bins);
    if (!s->fft_cfg)
    {
        log_error("frame_sync_create: dsp_fft_alloc failed\n");
        goto out;
    }
    s->est_fft_cfg = dsp_fft_alloc(2 * (int)s->number_of_bins);
    if (!s->est_fft_cfg)
    {
        log_error("frame_sync_create: 2N dsp_fft_alloc failed\n");
        goto out;
    }
    if (sfd_alloc(s) != 0)
    {
        log_error("frame_sync_create: sfd_alloc failed\n");
        goto out;
    }
    ret = 0;
out:
    if (ret < 0)
    {
        frame_sync_cleanup(s);
        free(s);
        s = NULL;
    }
    return s;
}

static void frame_sync_cleanup(struct frame_sync *s)
{
    free(s->preamb_up_vals);
    free(s->mode_hash);
    free(s->upchirp);
    free(s->downchirp);
    free(s->in_down);
    free(s->out_win);
    free(s->preamble_raw);
    free(s->net_id_samp);
    free(s->cfo_correc_long);
    free(s->sfo_corr_vect);
    free(s->net_ids_samp_dec);
    free(s->rot_tmp);
    free(s->preamble_upchirps);
    free(s->cfo_correc);
    free(s->symb_corr);
    free(s->est_dechirped);
    free(s->est_cx_in);
    free(s->est_cx_out);
    free(s->est_fft_mag);
    free(s->est_fft_val);
    free(s->est_k0);
    free(s->est_k0_mag);
    free(s->cx_in);
    free(s->cx_out);
    free(s->fft_mag);
    free(s->detect_score);
    free(s->netid_mask);
    free(s->detect_mag_hist);
    free(s->detect_mag_sum);
    sfd_free(s);
    if (s->fft_cfg)
        dsp_fft_free(s->fft_cfg);
    if (s->est_fft_cfg)
        dsp_fft_free(s->est_fft_cfg);
    memset(s, 0, sizeof(*s));
}

void frame_sync_destroy(struct frame_sync *s)
{
    if (!s)
        return;
    frame_sync_cleanup(s);
    free(s);
}

/* Deviation: the reference re-arms its DETECT fields alone; this port also
 * clears the one-shot cfo_est_done and the DETECT averaging ring, kept
 * across frames — left set, the next frame's SYNC would reuse the previous
 * frame's CFO, and its first detect windows would average in the finished
 * frame's spectra. */
void frame_sync_reset_detect(struct frame_sync *s)
{
    s->phase = FS_DETECT;
    s->symbol_cnt = 1;
    s->k_hat = 0;
    s->sto_frac = 0.0f;
    // re-run the CFO/STO estimator on the next frame's preamble
    s->cfo_est_done = 0;
    s->cfo_applied = 0.0f;
    // the caller's symbol-budget handshake is per-frame
    s->symb_numb = 0;
    s->received_head = 0;
    s->consume = 0;
    // drop the finished frame's spectra: they must not seed the next hunt
    s->detect_hist_fill = 0;
    s->detect_hist_pos = 0;
    s->detect_win_cnt = 0;
    if (s->detect_mag_sum)
        memset(s->detect_mag_sum, 0, s->number_of_bins * sizeof(*s->detect_mag_sum));
}

void frame_sync_set_symbol_budget(struct frame_sync *s, uint32_t symb_numb)
{
    s->symb_numb = symb_numb;
    s->received_head = 1;
}

/* Gather kernel half-length: 2*FRAC_M taps, the house resampler's m=4 (60 dB). */
#define FRAC_M 4

// a gather is directional (dst <- in); the geometry quadruple is inherent
void frame_sync_frac_gather(
    float complex *dst,
    const float complex *in,
    float base, // NOLINT(bugprone-easily-swappable-parameters)
    int stride,
    uint32_t count,
    uint32_t len)
{
    unsigned m = FRAC_M;
    int i0 = (int)floorf(base);
    float f = base - (float)i0;

    if (f < 1e-4f)
    { /* integer position: the kernel is a unit impulse, plain stride (the
       * pre-payload callers always land here — no estimate exists yet) */
        for (uint32_t i = 0; i < count; i++)
        {
            int t = i0 + stride * (int)i;
            dst[i] = in[t < 0 ? 0 : (t > (int)len - 1 ? (int)len - 1 : t)];
        }
        return;
    }

    /* Weights once per call: the fractional offset is the same for every
     * output (stride is an integer), only the integer part advances. */
    float w[16];
    float beta = 5.6536f; // 60 dB stopband, the house resampler's as=60
    float ib = dsp_bessel_i0(beta);
    float wsum = 0.0f;
    for (unsigned j = 0; j < 2 * m; j++)
    {
        float d = (float)((int)j - (int)m + 1) - f; // tap - base
        float sinc = fabsf(d) < 1e-7f ? 1.0f : sinf((float)M_PI * d) / ((float)M_PI * d);
        float x = 2.0f * (float)j / (float)(2 * m - 1) - 1.0f;
        w[j] = sinc * dsp_bessel_i0(beta * sqrtf(1.0f - x * x)) / ib;
        wsum += w[j];
    }
    for (unsigned j = 0; j < 2 * m; j++)
        w[j] /= wsum; // unity DC gain

    for (uint32_t i = 0; i < count; i++)
    {
        float complex acc = 0;
        for (unsigned j = 0; j < 2 * m; j++)
        {
            int t = i0 + stride * (int)i + (int)j - (int)m + 1;
            t = t < 0 ? 0 : (t > (int)len - 1 ? (int)len - 1 : t);
            acc += w[j] * in[t];
        }
        dst[i] = acc;
    }
}

void frame_sync_status(const struct frame_sync *s, struct frame_sync_status *st)
{
    *st = (struct frame_sync_status){
        .sf = s->spreading_factor,
        .samples_per_symbol = s->samples_per_symbol,
        .number_of_bins = s->number_of_bins,
        .bandwidth = s->bandwidth,
        .k_hat = s->k_hat,
        .n_up_req = s->n_up_req,
        .cfo = s->cfo,
        .sfo_hat = s->sfo_hat,
        .sto_frac = s->sto_frac,
        .netid_dec = {s->netid_dec[0], s->netid_dec[1]},
        .sync_words = {s->sync_words[0], s->sync_words[1]},
    };
}

void frame_sync_downsample_prologue(
    struct frame_sync *s, const float complex *in, float complex *dst)
{
    /* Continuous timing application: the STO estimate at sub-chip precision,
     * plus — in the payload phase — the SFO walk not yet folded into a
     * whole-sample slip. The stream grid (consume) stays integer; the gather
     * carries the fraction the round() used to discard, which is what made
     * low os lose (at os=1 it discarded the estimate outright). Pre-payload
     * callers (DETECT windows, SYNC walk) have no estimate yet and take the
     * fast integer path. */
    float sto_samp = s->sto_frac * (float)s->oversampling_factor;
    if (s->phase == FS_SFO_COMP)
        sto_samp += s->sfo_cum * (float)s->oversampling_factor;
    frame_sync_frac_gather(
        dst,
        in,
        // os / 2 truncation is by design
        (float)(s->oversampling_factor / 2) - sto_samp, // NOLINT(bugprone-integer-division)
        (int)s->oversampling_factor,
        s->number_of_bins,
        s->samples_per_symbol);
}

int frame_sync_get_symbol_val(
    struct frame_sync *s, const float complex *samples, const float complex *ref_chirp)
{
    double sig_en = 0;

    // deviation: the reference multiplies into a separate dechirp buffer and
    // converts it to the FFT input in a second pass — same values, one pass
    volk_32fc_x2_multiply_32fc(
        (lv_32fc_t *)(void *)s->cx_in, samples, ref_chirp, s->number_of_bins);

    dsp_fft(s->fft_cfg, s->cx_in, s->cx_out);

    volk_32fc_magnitude_squared_32f(s->fft_mag, (lv_32fc_t *)(void *)s->cx_out, s->number_of_bins);
    for (uint32_t i = 0; i < s->number_of_bins; i++)
        sig_en += s->fft_mag[i];

    if (sig_en == 0)
        return -1;

    int max = 0;
    for (uint32_t i = 1; i < s->number_of_bins; i++)
        if (s->fft_mag[i] > s->fft_mag[max])
            max = (int)i;

    return max;
}

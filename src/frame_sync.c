/*
 * Frame-sync state lifecycle and the shared dechirp-FFT kernel; line
 * references point at [gr-lora_sdr] frame_sync_impl.cc. The
 * DETECT state lives in detect.c, SYNC/SFO_COMPENSATION in sync.c.
 */

#include "frame_sync.h"
#include "dsp.h"
#include "frame_sync_internal.h"

#include "logging.h"
#include "utilities.h"
#include <complex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void frame_sync_cleanup(struct frame_sync *s);

// Reference: [gr-lora_sdr] frame_sync_impl.cc:25,45,66-73,77-78,128-137,198-205;
// [gr-lora] demod_impl.cc:54
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
    s->preamb_up_vals = malloc(s->n_up_req * sizeof(*s->preamb_up_vals));
    // port-only: per-slot copies of the agreeing windows for the lazy
    // preamble materialization (detect.c); the caller's buffer may be
    // trimmed between steps
    s->detect_run_srcs =
        malloc((size_t)s->n_up_req * s->samples_per_symbol * sizeof(*s->detect_run_srcs));
    s->upchirp = malloc(s->number_of_bins * sizeof(*s->upchirp));
    s->downchirp = malloc(s->number_of_bins * sizeof(*s->downchirp));
    s->in_down = malloc(s->number_of_bins * sizeof(*s->in_down));
    s->out_win = malloc(s->number_of_bins * sizeof(*s->out_win));
    s->preamble_raw = malloc((size_t)preamble_len * s->number_of_bins * sizeof(*s->preamble_raw));
    s->net_id_samp = malloc((size_t)(2.5 * s->samples_per_symbol) * sizeof(*s->net_id_samp));
    s->cfo_correc_long =
        malloc((size_t)(s->n_up_req + 3) * s->number_of_bins * sizeof(*s->cfo_correc_long));
    s->sfo_corr_vect =
        malloc((size_t)(s->n_up_req + 3) * s->number_of_bins * sizeof(*s->sfo_corr_vect));
    s->net_ids_samp_dec = malloc(2 * (size_t)s->number_of_bins * sizeof(*s->net_ids_samp_dec));
    s->rot_tmp = malloc(s->samples_per_symbol * sizeof(*s->rot_tmp));
    s->preamble_upchirps =
        malloc((size_t)preamble_len * s->number_of_bins * sizeof(*s->preamble_upchirps));
    s->cfo_correc = malloc(s->number_of_bins * sizeof(*s->cfo_correc));
    s->cfo_correc_aug =
        malloc((size_t)s->n_up_req * s->number_of_bins * sizeof(*s->cfo_correc_aug));
    s->symb_corr = malloc(s->number_of_bins * sizeof(*s->symb_corr));
    s->est_dechirped = malloc(s->number_of_bins * sizeof(*s->est_dechirped));
    s->est_cx_in = malloc(2 * (size_t)s->number_of_bins * sizeof(*s->est_cx_in));
    s->est_cx_out = malloc(2 * (size_t)s->number_of_bins * sizeof(*s->est_cx_out));
    s->est_fft_mag = malloc(2 * (size_t)s->number_of_bins * sizeof(*s->est_fft_mag));
    s->est_fft_val = malloc((size_t)s->n_up_req * s->number_of_bins * sizeof(*s->est_fft_val));
    s->est_k0 = malloc(s->n_up_req * sizeof(*s->est_k0));
    s->est_k0_mag = malloc(s->n_up_req * sizeof(*s->est_k0_mag));
    s->cx_in = malloc(s->number_of_bins * sizeof(*s->cx_in));
    s->cx_out = malloc(s->number_of_bins * sizeof(*s->cx_out));
    s->fft_mag = malloc(s->number_of_bins * sizeof(*s->fft_mag));
    if (s->detect_avg > 1)
    {
        s->detect_mag_hist =
            malloc((size_t)s->detect_avg * s->number_of_bins * sizeof(*s->detect_mag_hist));
        s->detect_mag_avg = malloc(s->number_of_bins * sizeof(*s->detect_mag_avg));
        s->detect_mag_sum = calloc(s->number_of_bins, sizeof(*s->detect_mag_sum));
    }
    if (!s->preamb_up_vals || !s->detect_run_srcs || !s->upchirp || !s->downchirp || !s->in_down ||
        !s->out_win || !s->preamble_raw || !s->net_id_samp || !s->cfo_correc_long ||
        !s->sfo_corr_vect || !s->net_ids_samp_dec || !s->rot_tmp || !s->preamble_upchirps ||
        !s->cfo_correc || !s->cfo_correc_aug || !s->symb_corr || !s->est_dechirped ||
        !s->est_cx_in || !s->est_cx_out || !s->est_fft_mag || !s->est_fft_val || !s->est_k0 ||
        !s->est_k0_mag || !s->cx_in || !s->cx_out || !s->fft_mag ||
        (s->detect_avg > 1 && (!s->detect_mag_hist || !s->detect_mag_avg || !s->detect_mag_sum)))
    {
        log_error("frame_sync_create: out of memory\n");
        goto out;
    }
    memset(s->preamb_up_vals, 0, s->n_up_req * sizeof(*s->preamb_up_vals));
    util_build_ref_chirps(s->upchirp, s->downchirp, spreading_factor, 1);

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

// Reference: [gr-lora_sdr] frame_sync_impl.cc:108; [gr-lora] demod_impl.cc:109
static void frame_sync_cleanup(struct frame_sync *s)
{
    free(s->preamb_up_vals);
    free(s->detect_run_srcs);
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
    free(s->cfo_correc_aug);
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
    free(s->detect_mag_hist);
    free(s->detect_mag_avg);
    free(s->detect_mag_sum);
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

/* Reference: [gr-lora_sdr] frame_sync_impl.cc:874-880,404-411. The reference
 * re-arms its DETECT fields alone; this port also clears the one-shot
 * cfo_est_done and the DETECT averaging ring, kept across frames — left set,
 * the next frame's SYNC would reuse the previous frame's CFO, and its first
 * detect windows would average in the finished frame's spectra. */
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

void frame_sync_status(const struct frame_sync *s, struct frame_sync_status *st)
{
    *st = (struct frame_sync_status){
        .sf = s->spreading_factor,
        .os = s->oversampling_factor,
        .samples_per_symbol = s->samples_per_symbol,
        .number_of_bins = s->number_of_bins,
        .bandwidth = s->bandwidth,
        .k_hat = s->k_hat,
        .n_up_req = s->n_up_req,
        .down_val = s->down_val,
        .cfo = s->cfo,
        .sfo_hat = s->sfo_hat,
        .sto_frac = s->sto_frac,
        .netid_dec = {s->netid_dec[0], s->netid_dec[1]},
        .sync_words = {s->sync_words[0], s->sync_words[1]},
        .sync_word_rx = s->sync_word_rx,
        .symbol_cnt = s->symbol_cnt,
        .bin_idx = s->bin_idx,
    };
}

// Reference: [gr-lora_sdr] frame_sync_impl.cc:499-501
void frame_sync_downsample_prologue(
    struct frame_sync *s, const float complex *in, float complex *dst)
{
    int sto_samp = util_round(s->sto_frac * (float)s->oversampling_factor);
    for (uint32_t ii = 0; ii < s->number_of_bins; ii++)
    {
        /* clamp at 0: for odd os_factor the first index can go negative
         * (util_round rounds half away from zero) and read before the window */
        int tap = (int)(s->oversampling_factor / 2) + (int)(s->oversampling_factor * ii) - sto_samp;
        dst[ii] = in[tap < 0 ? 0 : tap];
    }
}

// Reference: [gr-lora_sdr] frame_sync_impl.cc:322; [gr-lora] demod_impl.cc:199-206
int frame_sync_get_symbol_val(
    struct frame_sync *s, const float complex *samples, const float complex *ref_chirp)
{
    double sig_en = 0;

    // deviation: the reference multiplies into a separate dechirp buffer and
    // converts it to the FFT input in a second pass — same values, one pass
    for (uint32_t i = 0; i < s->number_of_bins; i++)
    {
        float complex v = samples[i] * ref_chirp[i];
        s->cx_in[i].r = crealf(v);
        s->cx_in[i].i = cimagf(v);
    }

    dsp_fft(s->fft_cfg, s->cx_in, s->cx_out);

    for (uint32_t i = 0; i < s->number_of_bins; i++)
    {
        s->fft_mag[i] = s->cx_out[i].r * s->cx_out[i].r + s->cx_out[i].i * s->cx_out[i].i;
        sig_en += s->fft_mag[i];
    }

    if (sig_en == 0)
        return -1;

    int max = 0;
    for (uint32_t i = 1; i < s->number_of_bins; i++)
        if (s->fft_mag[i] > s->fft_mag[max])
            max = (int)i;

    return max;
}

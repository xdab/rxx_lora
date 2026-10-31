/*
 * DETECT state of the frame-sync stage: preamble hunting and the coarse
 * sync's grid decision. State and the shared dechirp-FFT kernel live in
 * frame_sync.c, SYNC/SFO_COMPENSATION in sync.c.
 */

#include "frame_sync.h"
#include "frame_sync_internal.h"

#include "logging.h"
#include "utilities.h"
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// port-side: exact resync period for the sliding detect-mean sum, in windows
#define DETECT_SUM_RESYNC 1024

// deviation; baseline: [gr-lora_sdr] frame_sync_impl.cc:348 (raw argmax)
static int get_symbol_val_detect(
    struct frame_sync *s, const float complex *samples, const float complex *ref_chirp)
{
    int raw = frame_sync_get_symbol_val(s, samples, ref_chirp);
    if (raw < 0)
        return raw;

    uint32_t n = s->number_of_bins;
    const float *mag = s->fft_mag;
    uint8_t fill = 1;

    if (s->detect_avg > 1)
    {
        // deviation: sliding sum instead of a ring recompute; the exact
        // resync bounds fp drift far below any margin
        float *slot = &s->detect_mag_hist[(size_t)s->detect_hist_pos * n];
        int full = s->detect_hist_fill == s->detect_avg;
        for (uint32_t b = 0; b < n; b++)
        {
            float sum = s->detect_mag_sum[b];
            if (full)
                sum -= slot[b];
            slot[b] = s->fft_mag[b];
            s->detect_mag_sum[b] = sum + slot[b];
        }

        s->detect_hist_pos = (uint8_t)((s->detect_hist_pos + 1) % s->detect_avg);
        if (s->detect_hist_fill < s->detect_avg)
            s->detect_hist_fill++;

        fill = s->detect_hist_fill;
        if (++s->detect_win_cnt >= DETECT_SUM_RESYNC)
        {
            s->detect_win_cnt = 0;
            for (uint32_t b = 0; b < n; b++)
            {
                float acc = 0.0f;
                for (uint8_t k = 0; k < fill; k++)
                    acc += s->detect_mag_hist[(size_t)k * n + b];
                s->detect_mag_sum[b] = acc;
            }
        }

        for (uint32_t b = 0; b < n; b++)
            s->detect_mag_avg[b] = s->detect_mag_sum[b] / (float)fill;
        mag = s->detect_mag_avg;
    }

    int best = 0, second = -1;
    float best_score = -1.0f, second_score = -1.0f;

    for (uint32_t b = 0; b < n; b++)
    {
        float score = mag[b] + mag[(b + 1) % n] + mag[(b + n - 1) % n];
        if (score > best_score)
        {
            second = best;
            second_score = best_score;
            best_score = score;
            best = (int)b;
        }
        else if (score > second_score)
        {
            second_score = score;
            second = (int)b;
        }
    }

    /* Deviation: half-bin self-tie break. Near +-1/2 bin offsets the 3-bin
     * scores tie and flap 2 bins apart (the 3-bin sum of a single sharp peak
     * maximizes one bin OFF the peak); trust the raw argmax there, which can
     * only turn such a step into a tolerated 1-bin one. */
    float raw_score = mag[raw] + mag[(raw + 1) % n] + mag[(raw + n - 1) % n];
    int decision = best;
    if (raw_score >= best_score - 0.10f * best_score &&
        util_mod(labs(raw - best) + 1, (int)n) - 1 <= 1)
        decision = raw;

    const char *tb = decision != best ? "  tiebreak->raw" : "";
    if (s->detect_avg > 1)
        log_trace(
            "detect: raw %3d (mag %.3e)  %u-win avg %3d (%.3e) vs %3d "
            "(%.3e)%s\n",
            raw,
            s->fft_mag[raw],
            fill,
            best,
            best_score,
            second,
            second_score,
            tb);
    else
        log_trace(
            "detect: raw %3d (mag %.3e)  3-bin: %3d (%.3e) vs %3d (%.3e)%s\n",
            raw,
            s->fft_mag[raw],
            best,
            best_score,
            second,
            second_score,
            tb);

    return decision;
}

/* DETECT-path deviations from the reference's raw argmax, which flaps +-2 bins
 * on a two-tone window: 3-bin energy sums, optional mean over detect_avg
 * spectra, half-bin tie-break. */

// Reference: [gr-lora_sdr] frame_sync_impl.cc:505-539; [gr-lora] demod_impl.cc:247.
int frame_sync_detect_step(
    struct frame_sync *s, const float complex *in, int64_t *advance, struct frame_sync_status *st)
{
    *advance = 0;

    if (s->phase != FS_DETECT)
    {
        if (st)
            frame_sync_status(s, st);
        return 1;
    }

    frame_sync_downsample_prologue(s, in, s->in_down);

    int bin_idx_new = get_symbol_val_detect(s, s->in_down, s->downchirp);

    // consecutive reference upchirps, +-1 margin
    int agree = bin_idx_new != -1 &&
                labs(util_mod(labs(bin_idx_new - s->bin_idx) + 1, s->number_of_bins) - 1) <= 1;
    log_trace(
        "detect: bin %3d -> %s, run %d/%d\n",
        bin_idx_new,
        agree ? "agree" : "RESET",
        agree ? s->symbol_cnt + 1 : 1,
        s->n_up_req);

    if (agree)
    {
        if (s->symbol_cnt == 1 && s->bin_idx != -1)
            s->preamb_up_vals[0] = s->bin_idx;
        s->preamb_up_vals[s->symbol_cnt] = bin_idx_new;
        memcpy(
            s->preamble_raw + (size_t)s->symbol_cnt * s->number_of_bins,
            s->in_down,
            s->number_of_bins * sizeof(*s->in_down));
        s->symbol_cnt++;
    }
    else
    {
        memcpy(s->preamble_raw, s->in_down, s->number_of_bins * sizeof(*s->in_down));
        s->symbol_cnt = 1;
    }
    s->bin_idx = bin_idx_new;

    if (s->symbol_cnt == s->n_up_req)
    {
        // preamble_raw: the run's decimated windows (the DETECT prologue is
        // state-deterministic, re-decimation reproduces them); the slots
        // past n_up_req are SYNC's
        s->symbol_cnt = 0;
        s->additional_upchirps = 0;
        s->k_hat =
            util_most_frequent(s->preamb_up_vals, s->n_up_req, s->number_of_bins, s->mode_hash);
        if (s->k_hat < 0)
        {
            if (st)
                frame_sync_status(s, st);
            return -1;
        }

        struct sfd_result sfd;
        if (!sfd_locate(s, in, &sfd))
        {
            // no SFD behind the fire; 0 = the receiver consumes one window
            memcpy(s->preamble_raw, s->in_down, s->number_of_bins * sizeof(*s->in_down));
            s->symbol_cnt = 1;
            if (st)
                frame_sync_status(s, st);
            return 0;
        }

        s->phase = FS_SYNC;

        // netid1 on the reference's window lattice (fire + os*(N - k_hat)
        // mod sps): the downstream estimators read whole-bin tones; the SFD
        // edge only picks the lattice window
        int64_t os64 = (int64_t)s->oversampling_factor;
        int64_t sps64 = (int64_t)s->samples_per_symbol;
        int64_t lattice = os64 * ((int64_t)s->number_of_bins - s->k_hat) % sps64;
        double netid1 = (double)sfd.t_edge + (double)sfd.t_frac - 2.0 * (double)sps64;
        s->coarse_consume =
            lattice + (int64_t)llround((netid1 - (double)lattice) / (double)sps64) * sps64;
        memcpy(
            s->net_id_samp,
            &in[s->coarse_consume - sps64 / 4],
            (size_t)(s->samples_per_symbol / 4) * sizeof(*s->net_id_samp));
        *advance = s->coarse_consume;
        if (st)
            frame_sync_status(s, st);
        return 1;
    }

    if (st)
        frame_sync_status(s, st);
    return 0;
}

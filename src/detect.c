#include "detect.h"

#include "logging.h"
#include "sync.h"
#include "utilities.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// port-side: exact resync period for the sliding detect-mean sum, in windows
#define DETECT_SUM_RESYNC 1024

// argmax of the 3-bin energy sum (b-1, b, b+1); tolerates a tone spread over
// neighbors by residual fractional STO
static int argmax_3bin(const float *mag, uint32_t n)
{
    int best = 0;
    float best_score = -1.0f;

    for (uint32_t b = 0; b < n; b++)
    {
        float score = mag[b] + mag[(b + 1) % n] + mag[(b + n - 1) % n];
        if (score > best_score)
        {
            best_score = score;
            best = (int)b;
        }
    }

    return best;
}

/* Fused 3-bin energy argmax over two dechirped magnitude rows: row b rotated
 * by `shift` bins so its tone lands on row a's expected pair bin, then summed.
 * The noncoherent fusion of the (w0, w1) sync pair carries ~3 dB more energy
 * than either single-window vote; the argmax sits at V + w0. */
static int argmax_3bin_fused(const float *a, const float *b, int shift, uint32_t n)
{
    int best = 0;
    float best_score = -1.0f;

    for (uint32_t k = 0; k < n; k++)
    {
        long kp = util_mod((long)k + 1, (long)n);
        long km = util_mod((long)k - 1, (long)n);
        long sp = util_mod((long)k + 1 + shift, (long)n);
        long sm = util_mod((long)k - 1 + shift, (long)n);
        long s0 = util_mod((long)k + shift, (long)n);

        float score = a[km] + a[k] + a[kp] + b[sm] + b[s0] + b[sp];
        if (score > best_score)
        {
            best_score = score;
            best = (int)k;
        }
    }

    return best;
}

// deviation; baseline: [gr-lora_sdr] frame_sync_impl.cc:348 (raw argmax)
static int get_symbol_val_detect(
    struct frame_sync_state *s, const float complex *samples, const float complex *ref_chirp)
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
            s->detect_mag_avg[b] = s->detect_mag_sum[b] / fill;
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
int frame_sync_detect_step(struct frame_sync_state *s, const float complex *in)
{
    if (s->phase != FS_DETECT)
        return 1;

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
            s->detect_run_srcs + (size_t)s->symbol_cnt * s->samples_per_symbol,
            in,
            (size_t)s->samples_per_symbol * sizeof(*in));
        s->symbol_cnt++;
    }
    else
    {
        memcpy(s->detect_run_srcs, in, (size_t)s->samples_per_symbol * sizeof(*in));
        s->symbol_cnt = 1;
    }
    s->bin_idx = bin_idx_new;

    if (s->symbol_cnt == s->n_up_req)
    {
        /* Deviation: record one source pointer per slot (detect_run_srcs) and
         * materialize every window here, once the run proves real. */
        for (int k = 0; k < s->n_up_req; k++)
        {
            const float complex *src = s->detect_run_srcs + (size_t)k * s->samples_per_symbol;
            frame_sync_downsample_prologue(
                s, src, &s->preamble_raw[s->number_of_bins * (uint32_t)k]);
        }
        s->phase = FS_SYNC;
        s->symbol_cnt = 0;
        s->additional_upchirps = 0;
        s->k_hat = util_most_frequent(s->preamb_up_vals, s->n_up_req, s->number_of_bins);
        if (s->k_hat < 0)
            return -1;

        /* Deviation (this port): settle the symbol grid from the sync word
         * itself instead of guessing how many preamble upchirps follow. Every
         * preamble upchirp dechirps to one value V; the two sync symbols to
         * V + sync_words[0] and V + sync_words[1] — a signature independent of
         * the unknown carrier offset. The fractional CFO is estimated first
         * (Bernier) and rotated out of every scanned window, V is the argmax of
         * the noncoherent sum of the corrected preamble spectra, and each
         * scanned value is the 3-bin energy argmax. When the scan cannot
         * confirm the pair it leaves `lead` at 1, i.e. the reference's own
         * coarse sync. */
        int lead = 1; // windows from here to the first sync symbol
        {
            const uint32_t n = s->number_of_bins;
            const int ust = s->n_up_req - 1;

            float cfo_frac =
                frame_sync_estimate_cfo_frac_bernier(s, &s->preamble_raw[n - s->k_hat]);
            for (uint32_t j = 0; j < n; j++)
                s->cfo_correc[j] =
                    cexpf(I * (-2.0f * (float)M_PI * cfo_frac / (float)n * (float)j));

            int V = -1;
            float best_e = 0.0f;
            for (uint32_t b = 0; b < n; b++)
                s->est_fft_mag[b] = 0.0f;

            /* V must come off the same detect grid the scanned windows are
             * on: preamble_raw's unshifted windows, rotated by cfo_correc so
             * each tone sits unsplit on k_hat. */
            for (int i = 0; i < ust; i++)
            {
                for (uint32_t j = 0; j < n; j++)
                    s->in_down[j] = s->preamble_raw[(uint32_t)i * n + j] * s->cfo_correc[j];
                frame_sync_get_symbol_val(s, s->in_down, s->downchirp);
                for (uint32_t b = 0; b < n; b++)
                    s->est_fft_mag[b] += s->fft_mag[b];
            }
            for (uint32_t b = 0; b < n; b++)
                if (s->est_fft_mag[b] > best_e)
                {
                    best_e = s->est_fft_mag[b];
                    V = (int)b;
                }

            /* four_cum Move A (this port): fuse the (w0, w1) sync pair into
             * one noncoherent decision instead of ANDing two independent
             * argmax votes — keep the previous window's magnitude row
             * (est_fft_mag, free after the V estimate above), sum it with the
             * current one at the known pair shift dw = w1 - w0, and accept
             * when the fused 3-bin argmax lands on V + w0. The fused pair
             * carries ~3 dB more energy than either single-window vote. */
            int vals[SYNC_LOOKAHEAD + 1];
            int want0 = (int)s->sync_words[0];
            int want1 = s->sync_words[1] ? (int)s->sync_words[1] : want0;
            int dw = want1 - want0;
            float *prev_mag = s->est_fft_mag;
            int found = 0;
            int m = 0;

            /* one window behind the fire: with detect_avg > 1 the averaged
             * decision lags far enough that DETECT can fire on netid2's own
             * window, putting the pair one window in the PAST */
            for (; m <= SYNC_LOOKAHEAD; m++)
            {
                frame_sync_downsample_prologue(
                    s, in + (size_t)(m - 1) * s->samples_per_symbol, s->in_down);
                for (uint32_t j = 0; j < n; j++)
                    s->in_down[j] *= s->cfo_correc[j];
                if (frame_sync_get_symbol_val(s, s->in_down, s->downchirp) < 0 && m > 0)
                    break;
                vals[m] = argmax_3bin(s->fft_mag, n);
                if (m > 0 && V >= 0)
                {
                    int fb = argmax_3bin_fused(prev_mag, s->fft_mag, dw, n);
                    int danch = (int)util_mod((long)fb - (V + want0), (long)n);
                    if (danch > (int)n / 2)
                        danch -= (int)n;
                    if (abs(danch) <= 2)
                    {
                        lead = (m - 1) - 1;
                        found = 1;
                        break;
                    }
                }
                memcpy(prev_mag, s->fft_mag, n * sizeof(*prev_mag));
            }

            log_trace(
                "detect: look-ahead V=%d lead=%d found=%d over %d windows"
                " (cfo_frac=%+.3f)\n",
                V,
                lead,
                found,
                m,
                (double)cfo_frac);

            char vals_line[128]; // SYNC_LOOKAHEAD+2 short ints fit easily
            int off = snprintf(vals_line, sizeof(vals_line), "detect: vals:");
            for (int i = 0; i < m && off < (int)sizeof(vals_line) - 1; i++)
                off += snprintf(vals_line + off, sizeof(vals_line) - (size_t)off, " %d", vals[i]);
            log_trace("%s\n", vals_line);
        }

        memcpy(
            s->net_id_samp,
            &in[(int)(0.75 * s->samples_per_symbol - s->k_hat * s->oversampling_factor)],
            (size_t)(0.25 * s->samples_per_symbol) * sizeof(*s->net_id_samp));

        s->coarse_consume = (int64_t)s->oversampling_factor * (s->number_of_bins - s->k_hat) +
                            (int64_t)(lead - 1) * s->samples_per_symbol;
        return 1;
    }
    return 0;
}

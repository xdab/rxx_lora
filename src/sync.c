/*
 * SYNC and SFO_COMPENSATION states of the frame-sync stage; state and the
 * shared dechirp-FFT kernel live in frame_sync.c, preamble hunting in
 * detect.c.
 */

#include "sync.h"

#include "logging.h"
#include "utilities.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// port-side: exact-refresh period (power of two) for the float phasor
// recurrences below; bounds the running-multiply |z|=1 drift to ~3e-5.
#define PHASOR_RESYNC 512

/* Shared with detect.c (see sync.h); returns the offset's fractional part,
 * in bins, in (-0.5, +0.5]. The integer part comes from the SFD downchirp;
 * see frame_sync_sync_step(). */
float frame_sync_estimate_cfo_frac_bernier(struct frame_sync_state *s, const float complex *samples)
{
    const uint32_t n = s->number_of_bins;
    const int ust = s->n_up_req - 1; // up_symb_to_use

    for (int i = 0; i < ust; i++)
    {
        // volk_32fc_x2_multiply_32fc
        for (uint32_t j = 0; j < n; j++)
            s->est_dechirped[j] = samples[(uint32_t)n * i + j] * s->downchirp[j];

        for (uint32_t j = 0; j < n; j++)
        {
            s->est_cx_in[j].r = crealf(s->est_dechirped[j]);
            s->est_cx_in[j].i = cimagf(s->est_dechirped[j]);
        }

        dsp_fft(s->fft_cfg, s->est_cx_in, s->est_cx_out);

        for (uint32_t j = 0; j < n; j++)
        {
            s->est_fft_mag[j] =
                s->est_cx_out[j].r * s->est_cx_out[j].r + s->est_cx_out[j].i * s->est_cx_out[j].i;
            s->est_fft_val[j + (uint32_t)n * i] =
                (float complex)s->est_cx_out[j].r + I * s->est_cx_out[j].i;
        }

        int max = 0;
        for (uint32_t j = 1; j < n; j++)
            if (s->est_fft_mag[j] > s->est_fft_mag[max])
                max = (int)j;

        s->est_k0[i] = max;
        s->est_k0_mag[i] = s->est_fft_mag[max];
    }

    int idx_max = 0;
    for (int i = 1; i < ust; i++)
        if (s->est_k0_mag[i] > s->est_k0_mag[idx_max])
            idx_max = i;
    idx_max = s->est_k0[idx_max];

    float complex four_cum = 0;
    for (int i = 0; i < ust - 1; i++)
        four_cum += s->est_fft_val[idx_max + (uint32_t)n * i] *
                    conjf(s->est_fft_val[idx_max + (uint32_t)n * (i + 1)]);
    float cfo_frac = -cargf(four_cum) / 2.0f / (float)M_PI;

    // deviation: the reference calls cexpf per sample
    float complex step = cexpf(I * (-2.0f * (float)M_PI * cfo_frac / (float)n));
    float complex z = 1.0f;
    for (uint32_t j = 0; j < (uint32_t)ust * n; j++)
    {
        if ((j & (PHASOR_RESYNC - 1)) == 0)
            z = cexpf(I * (-2.0f * (float)M_PI * cfo_frac / (float)n * (float)j));
        s->cfo_correc_aug[j] = z;
        z *= step;
    }

    for (uint32_t j = 0; j < (uint32_t)ust * n; j++)
        s->preamble_upchirps[j] = samples[j] * s->cfo_correc_aug[j];

    return cfo_frac;
}

// Reference: [gr-lora_sdr] frame_sync_impl.cc:254
static float estimate_sto_frac(struct frame_sync_state *s)
{
    const uint32_t n = s->number_of_bins;
    const int ust = s->n_up_req - 1;

    for (uint32_t j = 0; j < 2 * n; j++)
        s->est_fft_mag[j] = 0;

    for (int i = 0; i < ust; i++)
    {
        for (uint32_t j = 0; j < n; j++)
            s->est_dechirped[j] = s->preamble_upchirps[(uint32_t)n * i + j] * s->downchirp[j];

        for (uint32_t j = 0; j < 2 * n; j++)
        {
            if (j < n)
            {
                s->est_cx_in[j].r = crealf(s->est_dechirped[j]);
                s->est_cx_in[j].i = cimagf(s->est_dechirped[j]);
            }
            else
            {
                s->est_cx_in[j].r = 0;
                s->est_cx_in[j].i = 0;
            }
        }

        dsp_fft(s->est_fft_cfg, s->est_cx_in, s->est_cx_out);

        for (uint32_t j = 0; j < 2 * n; j++)
            s->est_fft_mag[j] +=
                s->est_cx_out[j].r * s->est_cx_out[j].r + s->est_cx_out[j].i * s->est_cx_out[j].i;
    }

    int k0 = 0;
    for (uint32_t j = 1; j < 2 * n; j++)
        if (s->est_fft_mag[j] > s->est_fft_mag[k0])
            k0 = (int)j;

    double y_m1 = s->est_fft_mag[util_mod(k0 - 1, 2 * n)];
    double y0 = s->est_fft_mag[k0];
    double y1 = s->est_fft_mag[util_mod(k0 + 1, 2 * n)];
    double u = 64 * n / 406.5506497; // from Cui yang (eq.15)
    double v = u * 2.4674;
    double wa = (y1 - y_m1) / (u * (y1 + y_m1) + v * y0);
    double ka = wa * n / M_PI;
    double k_residual = fmod((k0 + ka) / 2, 1);
    float sto_frac = k_residual - (k_residual > 0.5 ? 1 : 0);

    return sto_frac;
}

// Reference: [gr-lora_sdr] frame_sync_impl.cc:573,625,667
static void rotate_left(struct frame_sync_state *s, float complex *v, uint32_t len, uint32_t r)
{
    if (r == 0 || r >= len)
        return;
    memcpy(s->rot_tmp, v, r * sizeof(*v));
    memmove(v, v + r, (len - r) * sizeof(*v));
    memcpy(v + len - r, s->rot_tmp, r * sizeof(*v));
}

// Reference: [gr-lora_sdr] frame_sync_impl.cc:545-841; [gr-lora] demod_impl.cc:279
int frame_sync_sync_step(struct frame_sync_state *s, const float complex *in)
{
    if (s->phase != FS_SYNC)
        return 1;

    frame_sync_downsample_prologue(s, in, s->in_down);

    if (!s->cfo_est_done)
    {
        /* The preamble yields the offset's fractional part; the whole part
         * needs the SFD downchirp, not yet seen. Remember how much of cfo
         * is already applied so FS_QUARTER_DOWN applies only the remainder. */
        s->cfo =
            frame_sync_estimate_cfo_frac_bernier(s, &s->preamble_raw[s->number_of_bins - s->k_hat]);
        s->cfo_applied = s->cfo;
        s->sto_frac = estimate_sto_frac(s);

        for (uint32_t j = 0; j < s->number_of_bins; j++)
            s->cfo_correc[j] =
                cexpf(I * (-2.0f * (float)M_PI * s->cfo / (float)s->number_of_bins * (float)j));
        s->cfo_est_done = 1;
    }

    for (uint32_t j = 0; j < s->number_of_bins; j++)
        s->symb_corr[j] = s->in_down[j] * s->cfo_correc[j];

    s->bin_idx = frame_sync_get_symbol_val(s, s->symb_corr, s->downchirp);
    s->consume = s->samples_per_symbol;

    switch (s->symbol_cnt)
    {
    case FS_NET_ID1:
        if (s->bin_idx == 0 || s->bin_idx == 1 || (uint32_t)s->bin_idx == s->number_of_bins - 1)
        { // additional upchirps (net id 1 of 0, 1 or N-1 reads as one);
            // the reference indexes in[0] here, kept as written.
            memcpy(
                &s->net_id_samp[0],
                &in[0],
                (size_t)(0.25 * s->samples_per_symbol) * sizeof(*s->net_id_samp));
            // saturates at 3; sets the corrected preamble's length below
            if (s->additional_upchirps < 3)
                s->additional_upchirps++;
        }
        else
        {
            s->symbol_cnt = FS_NET_ID2;
            memcpy(
                &s->net_id_samp[(size_t)(0.25 * s->samples_per_symbol)],
                &in[0],
                s->samples_per_symbol * sizeof(*s->net_id_samp));
        }
        break;

    case FS_NET_ID2:
        s->symbol_cnt = FS_DOWNCHIRP1;
        memcpy(
            &s->net_id_samp[(size_t)(1.25 * s->samples_per_symbol)],
            &in[0],
            (size_t)(s->number_of_bins + 1) * s->oversampling_factor * sizeof(*s->net_id_samp));
        break;

    case FS_DOWNCHIRP1:
        /* Deviation (this port): the whole-bin CFO decision integrates both
         * SFD downchirps, not just the second one — their dechirp tones sit
         * at the same bin, so summing the magnitude spectra gains ~a symbol
         * on the single integer that shifts the netid decode by a whole chip.
         * est_fft_mag is free here; the estimators are done. */
        frame_sync_get_symbol_val(s, s->symb_corr, s->upchirp);
        memcpy(s->est_fft_mag, s->fft_mag, s->number_of_bins * sizeof(*s->est_fft_mag));
        memcpy(
            &s->net_id_samp[(size_t)(2.25 * s->samples_per_symbol)],
            &in[0],
            (size_t)(0.25 * s->samples_per_symbol) * sizeof(*s->net_id_samp));
        s->symbol_cnt = FS_DOWNCHIRP2;
        break;

    case FS_DOWNCHIRP2:
        frame_sync_get_symbol_val(s, s->symb_corr, s->upchirp);
        s->down_val = 0;
        for (uint32_t j = 0; j < s->number_of_bins; j++)
        {
            s->est_fft_mag[j] += s->fft_mag[j];
            if (s->est_fft_mag[j] > s->est_fft_mag[s->down_val])
                s->down_val = (int)j;
        }
        s->symbol_cnt = FS_QUARTER_DOWN;
        break;

    case FS_QUARTER_DOWN:
    {
        const uint32_t n = s->number_of_bins;
        int down_signed = ((uint32_t)s->down_val < n / 2) ? s->down_val : s->down_val - (int)n;
        float cfo_coarse = util_round((float)down_signed / 2.0f - s->cfo);
        s->cfo = cfo_coarse + s->cfo; // the estimate is now complete

        { // EXPERIMENT: nudge the whole estimate (bins)
            const char *e = getenv("RTL_CFO_ADD");
            if (e)
                s->cfo += (float)atof(e);
        }

        log_trace(
            "cfo: SFD down_val=%d (signed %+d, half-bin grid -> whole bin "
            "%+.0f) + preamble fraction -> cfo=%+.4f bins (%.1f Hz)\n",
            s->down_val,
            down_signed,
            (double)cfo_coarse,
            (double)s->cfo,
            (double)s->cfo * s->bandwidth / s->number_of_bins);

        float lim = (float)n / 4.0f; // +-BW/4, in bins
        float ratio = fabsf(s->cfo) / lim;
        if (ratio > 0.75f)
            log_debug(
                "estimate %.1f Hz is %.0f%% of the +-BW/4 limit (+-%.1f Hz); "
                "if this is a real frame (not a noise mistrigger) the tuner "
                "crystal may be off - frames past the limit alias by N/2 and "
                "drop silently\n",
                (double)s->cfo * s->bandwidth / s->number_of_bins,
                100.0 * ratio,
                (double)s->bandwidth / 4.0);

        /* The preamble was already corrected by s->cfo_applied; correct it by
         * whatever has been added since. */
        float cfo_increment = s->cfo - s->cfo_applied;
        s->cfo_applied = s->cfo;

        // deviation: geometric progression instead of per-sample cexpf
        int cfo_int = (int)cfo_coarse; // whole bins; the fractional part is twisted below
        rotate_left(s, s->preamble_upchirps, s->preamble_len * n, util_mod(cfo_int, n));
        uint32_t len1 = (uint32_t)(s->n_up_req + s->additional_upchirps) * n;
        {
            float complex step = cexpf(I * (-2.0f * (float)M_PI * cfo_increment / (float)n));
            float complex z = 1.0f;
            for (uint32_t j = 0; j < len1; j++)
            {
                if ((j & (PHASOR_RESYNC - 1)) == 0)
                    z = cexpf(I * (-2.0f * (float)M_PI * cfo_increment / (float)n * (float)j));
                s->cfo_correc_long[j] = z;
                z *= step;
            }
        }
        for (uint32_t j = 0; j < (uint32_t)(s->n_up_req - 1) * n; j++)
            s->preamble_upchirps[j] *= s->cfo_correc_long[j];

        /* SFO estimate from the CFO: one crystal drives both the tuner LO and
         * the ADC, so a fractional carrier error eps implies a fractional
         * clock error eps, and sfo_hat = cfo*BW/f_center is that clock error
         * in chips/symbol. gr-lora_sdr infers it unconditionally
         * (frame_sync_impl.cc:638); the default-off guard is a deliberate
         * deviation, the reference assumes the coupling even where it cannot
         * hold. */
        s->sfo_hat = (s->center_freq != 0) ? (s->cfo * s->bandwidth) / s->center_freq : 0.0f;

        { // EXPERIMENT: force the SFO estimate (chips/symbol)
            const char *e = getenv("RTL_SFO_SET");
            if (e)
                s->sfo_hat = (float)atof(e);
        }

        double clk_off = s->sfo_hat / n;
        double fs = s->bandwidth;
        double fs_p = s->bandwidth * (1 - clk_off);
        double bwpow_p = s->bandwidth / fs_p * s->bandwidth / fs_p;
        double bwpow = s->bandwidth / fs * s->bandwidth / fs;
        // deviation: chirp recurrence per block instead of a per-sample
        // double cexp of a phase quadratic in m = j mod n
        {
            double A = -2.0 * M_PI * (bwpow_p - bwpow) / 2.0 / n;
            double Bblk = -2.0 * M_PI * (bwpow_p - s->bandwidth / fs_p); // per-block phase step
            double B0 = -2.0 * M_PI * (s->bandwidth / 2.0 * (1.0 / fs - 1.0 / fs_p));
            double complex step2 = cexp(I * 2.0 * A);
            double complex blk_z = 1.0, blk_step = 1.0;
            for (uint32_t j = 0; j < len1; j++)
            {
                uint32_t m = j % n;
                if (m == 0)
                {
                    blk_z = 1.0;
                    blk_step = cexp(I * (A + B0 + Bblk * (double)(j / n)));
                }
                s->sfo_corr_vect[j] = (float complex)blk_z;
                blk_z *= blk_step;
                blk_step *= step2;
            }
        }

        for (uint32_t j = 0; j < (uint32_t)(s->n_up_req - 1) * n; j++)
            s->preamble_upchirps[j] *= s->sfo_corr_vect[j];

        float tmp_sto_frac = estimate_sto_frac(s);
        float diff_sto_frac = s->sto_frac - tmp_sto_frac;
        if (fabsf(diff_sto_frac) <= (float)(s->oversampling_factor - 1) / s->oversampling_factor)
            s->sto_frac = tmp_sto_frac;

        // SNR estimate not ported (log only)

        s->sto_frac += s->sfo_hat * s->preamble_len;
        if (fabsf(s->sto_frac) > 0.5f)
            s->sto_frac += s->sto_frac > 0 ? -1 : 1;

        int start_off = (int)s->oversampling_factor / 2 -
                        util_round(s->sto_frac * s->oversampling_factor) +
                        s->oversampling_factor * (0.25 * n + cfo_int);
        for (uint32_t i = 0; i < 2 * n; i++)
            s->net_ids_samp_dec[i] = s->net_id_samp[start_off + i * s->oversampling_factor];
        for (uint32_t i = 0; i < 2 * n; i++)
            s->net_ids_samp_dec[i] *= s->cfo_correc_long[i];
        for (uint32_t i = 0; i < n; i++)
        {
            s->net_ids_samp_dec[i] *= s->cfo_correc[i];
            s->net_ids_samp_dec[n + i] *= s->cfo_correc[i];
        }

        s->netid_dec[0] = frame_sync_get_symbol_val(s, &s->net_ids_samp_dec[0], s->downchirp);
        s->netid_dec[1] = frame_sync_get_symbol_val(s, &s->net_ids_samp_dec[n], s->downchirp);

        { /* decode health: top-2 bins of each netid dechirp spectrum
           * (fft_mag holds netid2's after the second call, netid1's are
           * recomputed from the first call's window below) */
            float m[2][2] = {{0}};
            int b[2][2] = {{-1, -1}, {-1, -1}};
            for (int sym = 0; sym < 2; sym++)
            {
                frame_sync_get_symbol_val(s, &s->net_ids_samp_dec[sym * n], s->downchirp);
                for (uint32_t i = 0; i < s->number_of_bins; i++)
                {
                    if (s->fft_mag[i] > m[sym][0])
                    {
                        m[sym][1] = m[sym][0];
                        b[sym][1] = b[sym][0];
                        m[sym][0] = s->fft_mag[i];
                        b[sym][0] = (int)i;
                    }
                    else if (s->fft_mag[i] > m[sym][1])
                    {
                        m[sym][1] = s->fft_mag[i];
                        b[sym][1] = (int)i;
                    }
                }
            }
            log_debug(
                "netid spec1: %d (%.0f), %d (%.0f) | spec2: %d (%.0f), %d (%.0f)\n",
                b[0][0],
                m[0][0],
                b[0][1],
                m[0][1],
                b[1][0],
                m[1][0],
                b[1][1],
                m[1][1]);
        }

        int reset = 0;
        int early_walk = 0; // the pair legitimately read one symbol low
        if (s->sync_words[0] == 0)
        { /* accept-any: snap netid1 to the nearest multiple of 8 (the map
           * id -> symbols puts every netid there, and the coarse sync's
           * whole-chip slip is at most 2 bins, never enough to reach the
           * neighbour 8 bins away) and correct the grid exactly as -w does. */
            int32_t off = (int32_t)s->netid_dec[0];
            int32_t snap = 8 * ((off + 4) / 8);
            if (snap >= (int32_t)s->number_of_bins)
                snap = 0; // N is a multiple of 8: the wrap lands on 0
            off -= snap;
            if (off > (int32_t)s->number_of_bins / 2)
                off -= (int32_t)s->number_of_bins;
            if (off < -(int32_t)s->number_of_bins / 2)
                off += (int32_t)s->number_of_bins;
            if (off > 2 || off < -2)
                off = 0; // not near a multiple of 8: no slip estimate, keep the grid
            s->net_id_off = off;
            s->consume = -(int32_t)s->oversampling_factor * off;
            {
                char extra[64] = "";
                if (off)
                    snprintf(
                        extra,
                        sizeof(extra),
                        "; grid snapped %+d bin -> netid1 %d",
                        off,
                        s->netid_dec[0] - off);
                log_info(
                    "netid1 is %d, netid2 is %d, check skipped%s\n",
                    s->netid_dec[0],
                    s->netid_dec[1],
                    extra);
            }
        }
        else if (abs(s->netid_dec[0] - (int32_t)s->sync_words[0]) > 2)
        { /* early-walk recovery: one symbol early, netid1's window catches the
           * last preamble upchirp and netid2's catches netid1, so the pair
           * reads (w0 - 8, w1 - 8) with its SPACING intact. That spacing is
           * the evidence both symbols are real; the fix is a whole symbol of
           * grid advance (the caller applies `pos += consume`), not the -8
           * bins, which only reflect the wrong window. The reference's
           * one-symbol-off path (frame_sync_impl.cc:718) is the late mirror. */
            int32_t d1 = (int32_t)util_mod(s->netid_dec[0] - (int32_t)s->sync_words[0], n);
            int32_t dsp = (int32_t)util_mod(
                s->netid_dec[1] - s->netid_dec[0] -
                    ((int32_t)s->sync_words[1] - (int32_t)s->sync_words[0]),
                n);
            if (d1 > (int32_t)n / 2)
                d1 -= (int32_t)n;
            if (dsp > (int32_t)n / 2)
                dsp -= (int32_t)n;
            if (s->sync_words[1] != 0 && abs(d1 + 8) <= 2 && abs(dsp) <= 2)
            {
                s->net_id_off = 0; // the -8 is the wrong symbol, not a slip
                s->consume = s->samples_per_symbol;
                early_walk = 1;
                log_debug(
                    "netid early walk: dec %d,%d -> grid advanced one symbol\n",
                    s->netid_dec[0],
                    s->netid_dec[1]);
            }
            else
                reset = 1;
        }
        else
        {
            s->net_id_off = s->netid_dec[0] - (int32_t)s->sync_words[0];
            /* deviation (this port): netid2 is compared modulo n/4 bins —
             * the low SF-2 symbol bits only. Real 0x2b traffic arrives as
             * both (16, 88) and (16, 472): transmitter families differing
             * only in the two top symbol bits, which Semtech sync comparison
             * does not fully resolve (SX1262 receives when at least one
             * nibble matches, SX1276 sometimes with none — Classy Code,
             * "LoRa Sync Word Compatibility between SX127x and SX126x"). An
             * exact compare dropped frames stock nodes accept. nibble<<3
             * values are < n/4, so words from our own map compare exactly. */
            if (s->sync_words[1] != 0 && // match netid2 only if requested
                util_mod(s->netid_dec[1] - s->net_id_off, n / 4) !=
                    util_mod((int32_t)s->sync_words[1], n / 4))
                reset = 1;
            else
            {
                if (s->sync_words[1] == 0)
                    log_info("netid2 is %d\n", s->netid_dec[1]);
                s->consume = -s->oversampling_factor * s->net_id_off;
            }
        }

        if (reset)
        {
            log_debug(
                "netid reset: dec %d,%d expect %d,%d | cfo=%+.3f "
                "sto_frac=%+.3f down_val=%d k_hat=%d start_off=%d add_up=%d\n",
                s->netid_dec[0],
                s->netid_dec[1],
                (int)s->sync_words[0],
                (int)s->sync_words[1],
                (double)s->cfo,
                s->sto_frac,
                s->down_val,
                s->k_hat,
                start_off,
                s->additional_upchirps);

            frame_sync_reset_detect(s);
            return -1;
        }

        // the sync word actually received, rebuilt from the decoded pair
        // (nibble<<3 bins per symbol, grid slip removed, early walk shifted)
        {
            long n0 = util_mod(s->netid_dec[0] - s->net_id_off, n) / 8;
            long n1 = util_mod(s->netid_dec[1] - s->net_id_off, n) / 8;
            if (early_walk) // the windows sat one symbol early
            {
                n0++;
                n1++;
            }
            s->sync_word_rx = (uint8_t)(((n0 & 0xF) << 4) | (n1 & 0xF));
        }

        s->phase = FS_SFO_COMP;

        s->sto_frac += s->sfo_hat * 4.25;
        s->sfo_cum = ((s->sto_frac * s->oversampling_factor) -
                      util_round(s->sto_frac * s->oversampling_factor)) /
                     s->oversampling_factor;
        // frame_info tag not ported
        // One chip per bin, driven by the whole-bin part as in the reference.
        s->consume += s->samples_per_symbol / 4 + s->oversampling_factor * cfo_int;
        s->symbol_cnt = 0;
        break;
    }

    default:
        log_error("sync: no sync sub-state, shouldn't happen\n");
        break;
    }
    return s->phase != FS_SYNC; // 1 once the verdict moved us out of SYNC
}

// Reference: [gr-lora_sdr] frame_sync_impl.cc:845-871
int frame_sync_output_step(struct frame_sync_state *s, const float complex *in)
{
    if (s->phase != FS_SFO_COMP)
        return 1;

    frame_sync_downsample_prologue(s, in, s->in_down);

    // the 8 first windows are the PHY header; further payload windows follow
    // once the caller decoded the header and set symb_numb/received_head
    if (s->symbol_cnt < 8 || ((uint32_t)s->symbol_cnt < s->symb_numb && s->received_head))
    {
        // no STO correction, CFO left in
        memcpy(s->out_win, s->in_down, s->number_of_bins * sizeof(*s->in_down));
        s->consume = s->samples_per_symbol;

        if (fabsf(s->sfo_cum) > 1.0f / 2 / s->oversampling_factor)
        {
            int adj = signbit(s->sfo_cum) ? -1 : 1;
            s->consume -= adj;
            s->sfo_cum -= adj * 1.0f / s->oversampling_factor;
        }

        s->sfo_cum += s->sfo_hat;
        s->symbol_cnt++;

        return 0;
    }

    return 1;
}

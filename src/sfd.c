/*
 * SFD-first coarse sync: the slot pipeline that decides the frame grid at the
 * DETECT fire. The preamble's upchirps carry no timing phase — any window
 * inside one sees a full tone; the SFD's 2.25 downchirps are the only
 * deterministic time anchor. Stages:
 *
 *   lock          coherent FFT over the certified run's slots -> u = f + tau,
 *                 fractional-bin precision
 *   change point  known-bin preamble power at u, plus the candidate's
 *                 downchirp evidence (downchirp x upchirp = tone at f - tau)
 *   pairing       tau = wrap(u - d)/2, f = wrap(u - tau); N/2 ambiguity
 *                 resolved by |f| <= N/4
 *   two-pass      re-time by tau, re-lock, known-bin tests on the pure grid,
 *                 scored in noise-power units (lam) over slot shifts
 *   gate          lam >= SFD_LAM_GATE in worded mode; accept-any skips the
 *                 gate (no expected sync word -> other scale)
 */

#include "dsp.h"
#include "frame_sync.h"
#include "frame_sync_internal.h"

#include "logging.h"
#include "utilities.h"
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// SFD start, windows past the fire: 6 for a prompt fire (plen + 3 - n_up_req),
// less when averaging or resets delay the run; the +-2 fine shifts extend both
// ends
#define SFD_Q_MIN 2
#define SFD_Q_MAX 8
// fine-pass slot shifts around the re-timed candidate, in windows
#define SFD_SHIFT_MIN (-2)
#define SFD_SHIFT_MAX 2

// slot offsets covered: change-point U slots q-6..q+1 for q in [2, 8] ->
// -4..5; fine-pass down slots (shift + m for m in [-6, -1]) -> -8..1, up
// slots (shift + {0, 1}) -> -2..3
#define SFD_U_SLOTS 10
#define SFD_MAX_CANDIDATES (SFD_MAX_TAU_REPS * (SFD_SHIFT_MAX - SFD_SHIFT_MIN + 1))

// noise-only p_0.001 of the worded score
#define SFD_LAM_GATE 32.5f

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // qsort comparator
static int sfd_cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* (max magnitude)^2 over the 3 bins around a fractional bin on the 4N-gridded
 * spectrum, clamped at the array ends. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // (spectrum, bins, bin)
static float sfd_bin_pow(const float *mag, uint32_t n, float b_frac)
{
    const long n4 = 4L * (long)n;
    long b = lroundf(b_frac * 4.0f) % n4;
    if (b < 0)
        b += n4;
    long lo = b - 1 < 0 ? 0 : b - 1;
    long hi = b + 2 > n4 ? n4 : b + 2;

    float mx = 0.0f;
    for (long i = lo; i < hi; i++)
        if (mag[i] > mx)
            mx = mag[i];
    return mx * mx;
}

/* Raw sample at p relative to the fire window; zero outside the source's
 * retention (SYNC_LOOKAHEAD windows either side, virtual-zero padded). */
static float complex sfd_raw(const struct frame_sync *s, const float complex *in, int64_t p)
{
    int64_t back = -(int64_t)SYNC_LOOKAHEAD * (int64_t)s->samples_per_symbol;
    int64_t fwd = (int64_t)(SYNC_LOOKAHEAD + 1) * (int64_t)s->samples_per_symbol;
    if (p < back || p >= fwd)
        return 0;
    return in[p];
}

/* Anti-alias FIR + os-decimation of the N-chip slot at `off`:
 * dst[j] = filtered x[off + j*os]; the centered FIR keeps the time origin. */
static void
sfd_slot_dec(const struct frame_sync *s, const float complex *in, int64_t off, float complex *dst)
{
    const uint32_t n = s->number_of_bins;

    if (s->oversampling_factor <= 1)
    {
        for (uint32_t j = 0; j < n; j++)
            dst[j] = sfd_raw(s, in, off + (int64_t)j);
        return;
    }

    const unsigned half = SFD_FIR_TAPS / 2;
    for (uint32_t j = 0; j < n; j++)
    {
        int64_t base = off + (int64_t)j * (int64_t)s->oversampling_factor - (int64_t)half;
        float complex acc = 0;
        for (unsigned t = 0; t < SFD_FIR_TAPS; t++)
            acc += s->sfd.fir[t] * sfd_raw(s, in, base + (int64_t)t);
        dst[j] = acc;
    }
}

/* Magnitude spectrum (4N bins, zero-padded) of the slot at `off`, dechirped
 * by ref (downchirp for upchirp slots, upchirp for the SFD's downchirps). */
static void sfd_slot_mag(
    struct frame_sync *s,
    const float complex *in,
    int64_t off,
    const float complex *ref,
    float *mag)
{
    const uint32_t n = s->number_of_bins, n4 = 4u * n;

    sfd_slot_dec(s, in, off, s->sfd.slot);
    for (uint32_t j = 0; j < n; j++)
    {
        float complex v = s->sfd.slot[j] * ref[j];
        s->sfd.cx_in[j].r = crealf(v);
        s->sfd.cx_in[j].i = cimagf(v);
    }
    memset(s->sfd.cx_in + n, 0, (size_t)(n4 - n) * sizeof(*s->sfd.cx_in));
    dsp_fft(s->sfd.slot_fft_cfg, s->sfd.cx_in, s->sfd.cx_out);
    for (uint32_t i = 0; i < n4; i++)
        mag[i] = hypotf(s->sfd.cx_out[i].r, s->sfd.cx_out[i].i);
}

/* Coherent lock over `count` consecutive slots from `off`: dechirp each,
 * concatenate (the tone is phase-continuous across symbols), zero-pad,
 * fractional peak. Returns u = f + tau, in symbol bins. */
// NOLINTBEGIN(bugprone-easily-swappable-parameters) // config-style, one call per pipeline run
static float
sfd_lock(struct frame_sync *s, const float complex *in, int64_t off, int count, float u_fallback)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const uint32_t n = s->number_of_bins;
    uint32_t m = 0;

    for (int i = 0; i < count; i++)
    {
        sfd_slot_dec(s, in, off + (int64_t)i * (int64_t)s->samples_per_symbol, s->sfd.slot);
        for (uint32_t j = 0; j < n; j++)
        {
            float complex v = s->sfd.slot[j] * s->downchirp[j];
            s->sfd.cx_in[m].r = crealf(v);
            s->sfd.cx_in[m].i = cimagf(v);
            m++;
        }
    }
    if (m == 0)
        return u_fallback;

    memset(s->sfd.cx_in + m, 0, (size_t)(s->sfd.lock_len - m) * sizeof(*s->sfd.cx_in));
    dsp_fft(s->sfd.lock_fft_cfg, s->sfd.cx_in, s->sfd.cx_out);
    for (uint32_t i = 0; i < s->sfd.lock_len; i++)
        s->sfd.lockmag[i] = hypotf(s->sfd.cx_out[i].r, s->sfd.cx_out[i].i);

    // tone at u bins -> FFT bin u * L / N; wrap handles the negative half
    return util_wrap(
        dsp_frac_peak(s->sfd.lockmag, s->sfd.lock_len, NULL) * (float)n / (float)s->sfd.lock_len,
        n);
}

int sfd_alloc(struct frame_sync *s)
{
    struct sfd *f = &s->sfd;
    const uint32_t n = s->number_of_bins;

    // zero-pad the concatenation to >= 4x for the fractional-bin peak;
    // >= 4*n_up_req*N also covers the 6-slot re-lock
    f->lock_len = 16u;
    while (f->lock_len < 4u * (uint32_t)s->n_up_req * n)
        f->lock_len <<= 1;

    f->slot = malloc(n * sizeof(*f->slot));
    f->lockmag = malloc(f->lock_len * sizeof(*f->lockmag));
    f->mag = malloc(4 * (size_t)n * sizeof(*f->mag));
    f->dmag0 = malloc(4 * (size_t)n * sizeof(*f->dmag0));
    f->dmag1 = malloc(4 * (size_t)n * sizeof(*f->dmag1));
    f->dsum = malloc(4 * (size_t)n * sizeof(*f->dsum));
    f->med = malloc(4 * (size_t)n * sizeof(*f->med));
    f->fine_dn = malloc((size_t)SFD_FINE_DN * 4 * n * sizeof(*f->fine_dn));
    f->fine_up = malloc((size_t)SFD_FINE_UP * 4 * n * sizeof(*f->fine_up));
    f->cx_in = malloc(f->lock_len * sizeof(*f->cx_in));
    f->cx_out = malloc(f->lock_len * sizeof(*f->cx_out));
    f->slot_fft_cfg = dsp_fft_alloc(4 * (int)n);
    f->lock_fft_cfg = dsp_fft_alloc((int)f->lock_len);
    if (!f->slot || !f->lockmag || !f->mag || !f->dmag0 || !f->dmag1 || !f->dsum || !f->med ||
        !f->fine_dn || !f->fine_up || !f->cx_in || !f->cx_out || !f->slot_fft_cfg ||
        !f->lock_fft_cfg)
    {
        sfd_free(s);
        return -1;
    }

    // firwin(65, 1/os, hamming)-class design; the passband must cover the
    // whole chirp band — tone bins in (-N/2, N/2], up to the decimated
    // Nyquist. Unity DC gain: h = c*sinc(c*(t - center)) windowed, normalized.
    float c = 1.0f / (float)s->oversampling_factor;
    float sum = 0.0f;
    for (unsigned t = 0; t < SFD_FIR_TAPS; t++)
    {
        float x = c * (float)((int)t - (int)(SFD_FIR_TAPS / 2));
        float sinc = fabsf(x) < 1e-9f ? 1.0f : sinf((float)M_PI * x) / ((float)M_PI * x);
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)t / (float)(SFD_FIR_TAPS - 1));
        f->fir[t] = c * sinc * w;
        sum += f->fir[t];
    }
    for (unsigned t = 0; t < SFD_FIR_TAPS; t++)
        f->fir[t] /= sum;

    return 0;
}

void sfd_free(struct frame_sync *s)
{
    struct sfd *f = &s->sfd;
    free(f->slot);
    free(f->lockmag);
    free(f->mag);
    free(f->dmag0);
    free(f->dmag1);
    free(f->dsum);
    free(f->med);
    free(f->fine_dn);
    free(f->fine_up);
    free(f->cx_in);
    free(f->cx_out);
    if (f->slot_fft_cfg)
        dsp_fft_free(f->slot_fft_cfg);
    if (f->lock_fft_cfg)
        dsp_fft_free(f->lock_fft_cfg);
    memset(f, 0, sizeof(*f));
}

/* Pairing identity: (u, d) resolve to (tau, f) with the N/2 ambiguity —
 * (tau, f) and (tau + N/2, f + N/2) give the same (u, d) — so the wrapped
 * representatives are at most two. */
int sfd_tau_reps(float u, float d, uint32_t n, float taus[SFD_MAX_TAU_REPS])
{
    float tau0 = util_wrap(u - d, n) / 2.0f;
    const float raw_taus[SFD_MAX_TAU_REPS] = {tau0, tau0 - (float)n / 2.0f, tau0 + (float)n / 2.0f};
    int ntau = 0;
    for (int i = 0; i < SFD_MAX_TAU_REPS; i++)
    {
        float t = util_wrap(raw_taus[i], n);
        int dup = 0;
        for (int j = 0; j < ntau; j++)
            if (taus[j] == t)
                dup = 1;
        if (!dup)
            taus[ntau++] = t;
    }
    return ntau;
}

/* Median power of a magnitude spectrum; scratch (len floats) is reordered. */
float sfd_median_power(float *scratch, const float *mag, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        scratch[i] = mag[i] * mag[i];
    qsort(scratch, len, sizeof(float), sfd_cmp_float);
    return 0.5f * (scratch[len / 2 - 1] + scratch[len / 2]);
}

/* The fine score of one slot shift: known-bin evidence in noise-power units.
 * dn/up are the representative's reference spectra (SFD_FINE_DN / SFD_FINE_UP
 * rows of 4N), indexed by the shift; sync_words[0] != 0 adds the net-id terms. */
float sfd_shift_lam(
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // (down-ref, up-ref spectra)
    const float *dn,
    const float *up,
    uint32_t n,
    int shift,
    float u_r,
    float sigma2,
    const uint16_t *sync_words)
{
    const uint32_t n4 = 4u * n;

    float lam = 0.0f;
    for (int m = -6; m <= -3; m++) // preamble at u_r
        lam += sfd_bin_pow(dn + (size_t)(shift + m + 8) * n4, n, u_r) / sigma2 - 1.0f;
    float d_known = util_modf(u_r, n); // on the pure grid d = u
    lam += sfd_bin_pow(up + (size_t)(shift + 2) * n4, n, d_known) / sigma2 - 1.0f;
    lam += sfd_bin_pow(up + (size_t)(shift + 3) * n4, n, d_known) / sigma2 - 1.0f;
    if (sync_words[0] != 0)
    { // the pair at f + w0 / f + w1: positive net-id evidence
        lam += sfd_bin_pow(
                   dn + (size_t)(shift - 2 + 8) * n4, n, util_modf(u_r + (float)sync_words[0], n)) /
                   sigma2 -
               1.0f;
        lam += sfd_bin_pow(
                   dn + (size_t)(shift - 1 + 8) * n4, n, util_modf(u_r + (float)sync_words[1], n)) /
                   sigma2 -
               1.0f;
    }
    return lam;
}

/* Best candidate by lam; the margin over the best other through *margin.
 * Returns the winner's index. */
int sfd_select(const struct sfd_cand *c, int n, float *margin)
{
    int best = 0;
    float lam_best = -HUGE_VALF;
    for (int i = 0; i < n; i++)
        if (c[i].lam > lam_best)
        {
            lam_best = c[i].lam;
            best = i;
        }

    float m = lam_best;
    for (int i = 0; i < n; i++)
        if (i != best && lam_best - c[i].lam < m)
            m = lam_best - c[i].lam;
    *margin = m;
    return best;
}

/* The pipeline's statistical half: coherent lock on the certified run, the
 * change-point scan over candidate SFD starts, and the pairing identity.
 * Fills the tau representatives; returns their count. */
static int sfd_coarse(
    struct frame_sync *s,
    const float complex *in,
    float *u_hat,
    float *d_hat,
    int *q_hat,
    float taus[SFD_MAX_TAU_REPS])
{
    const uint32_t n = s->number_of_bins;
    const uint32_t n4 = 4u * n;
    const int64_t sps = (int64_t)s->samples_per_symbol;

    /* Lock on the certified run: the n_up_req agreeing windows ending at the
     * fire are upchirps by construction. */
    *u_hat = sfd_lock(s, in, -(int64_t)(s->n_up_req - 1) * sps, s->n_up_req, 0.0f);

    /* Change point over candidate SFD starts q windows past the fire:
     * known-bin power at u, plus the candidate's two windows dechirped by
     * the upchirp (downchirp evidence). */
    float upow[SFD_U_SLOTS]; // slot offsets -4..5, index o + 4
    for (int o = -4; o <= 5; o++)
    {
        sfd_slot_mag(s, in, (int64_t)o * sps, s->downchirp, s->sfd.mag);
        upow[o + 4] = sfd_bin_pow(s->sfd.mag, n, *u_hat);
    }

    float best_score = -HUGE_VALF;
    *q_hat = SFD_Q_MIN;
    *d_hat = 0.0f;
    for (int q = SFD_Q_MIN; q <= SFD_Q_MAX; q++)
    {
        sfd_slot_mag(s, in, (int64_t)q * sps, s->upchirp, s->sfd.dmag0);
        sfd_slot_mag(s, in, (int64_t)(q + 1) * sps, s->upchirp, s->sfd.dmag1);
        for (uint32_t i = 0; i < n4; i++)
            s->sfd.dsum[i] =
                sqrtf(s->sfd.dmag0[i] * s->sfd.dmag0[i] + s->sfd.dmag1[i] * s->sfd.dmag1[i]);

        float dpk;
        float kd = dsp_frac_peak(s->sfd.dsum, n4, &dpk);
        float score = dpk * dpk;
        for (int j = -6; j <= -3; j++) // U over the four slots before the pair
            score += upow[q + j + 4];
        if (score > best_score)
        {
            best_score = score;
            *q_hat = q;
            *d_hat = util_wrap(kd / 4.0f, n);
        }
    }

    return sfd_tau_reps(*u_hat, *d_hat, n, taus);
}

/* The pipeline's deterministic half: per tau representative, re-time, re-lock,
 * and score the pure grid — every slot a known symbol — over slot shifts.
 * Fills cands; returns the candidate count. */
static int sfd_fine(
    struct frame_sync *s,
    const float complex *in,
    float u_hat,
    int q_hat,
    const float *taus,
    int ntau,
    struct sfd_cand *cands)
{
    const uint32_t n = s->number_of_bins;
    const int os = (int)s->oversampling_factor;
    const int64_t sps = (int64_t)s->samples_per_symbol;
    const uint32_t n4 = 4u * n;
    int ncand = 0;

    for (int it = 0; it < ntau; it++)
    {
        // |f| <= N/4 prior: an impossible CFO is not scored
        if (fabsf(util_wrap(u_hat - taus[it], n)) > (float)n / 4.0f)
            continue;

        int64_t t_cand = (int64_t)q_hat * sps - (int64_t)lroundf(taus[it] * (float)os);
        float u_r = sfd_lock(s, in, t_cand - 8 * sps, 6, u_hat);

        // slot spectra once per representative; the shift scan re-indexes
        for (int j = -8; j <= 1; j++)
            sfd_slot_mag(
                s,
                in,
                t_cand + (int64_t)j * sps,
                s->downchirp,
                s->sfd.fine_dn + (size_t)(j + 8) * n4);
        for (int j = -2; j <= 3; j++)
            sfd_slot_mag(
                s,
                in,
                t_cand + (int64_t)j * sps,
                s->upchirp,
                s->sfd.fine_up + (size_t)(j + 2) * n4);

        // one noise floor per representative, shared by every shift — lam
        // must stay comparable across hypotheses. Zero means the slot sits
        // in the stream's zero padding: nothing to score
        float med = sfd_median_power(s->sfd.med, s->sfd.fine_up + (size_t)2 * n4, n4);
        if (med <= 1e-12f)
            continue;
        float sigma2 = med / 0.6931472f + 1e-12f;

        for (int shift = SFD_SHIFT_MIN; shift <= SFD_SHIFT_MAX; shift++)
        {
            cands[ncand].lam =
                sfd_shift_lam(s->sfd.fine_dn, s->sfd.fine_up, n, shift, u_r, sigma2, s->sync_words);
            cands[ncand].base = t_cand + (int64_t)shift * sps;
            cands[ncand].tau = taus[it];
            cands[ncand].u = u_r;
            ncand++;
        }
    }

    return ncand;
}

int sfd_locate(struct frame_sync *s, const float complex *in, struct sfd_result *r)
{
    const int os = (int)s->oversampling_factor;

    memset(r, 0, sizeof(*r));
    r->t_edge = -1;
    r->lam = -HUGE_VALF;
    r->margin = -HUGE_VALF;

    float u_hat, d_hat;
    int q_hat;
    float taus[SFD_MAX_TAU_REPS];
    int ntau = sfd_coarse(s, in, &u_hat, &d_hat, &q_hat, taus);

    struct sfd_cand cands[SFD_MAX_CANDIDATES];
    int ncand = sfd_fine(s, in, u_hat, q_hat, taus, ntau, cands);
    if (ncand == 0)
    {
        log_debug("sfd: no hypothesis (u=%.2f d=%.2f)\n", u_hat, d_hat);
        return 0;
    }

    float margin;
    int best = sfd_select(cands, ncand, &margin);

    r->t_edge = cands[best].base;
    // tau's rounding residue, for full-precision window placement
    r->t_frac = (float)(int)lroundf(cands[best].tau * (float)os) - cands[best].tau * (float)os;
    r->q = q_hat;
    r->u = u_hat;
    r->d = d_hat;
    r->tau = cands[best].tau;
    r->f = cands[best].u; // on the re-timed grid tau ~= 0: u is f

    if (s->sync_words[0] != 0 && cands[best].lam < SFD_LAM_GATE)
    {
        log_debug(
            "sfd: gate reject lam=%.1f margin=%.1f (u=%.2f d=%.2f tau=%.2f f=%.2f)\n",
            cands[best].lam,
            margin,
            u_hat,
            d_hat,
            r->tau,
            r->f);
        return 0;
    }

    log_trace(
        "sfd: q=%d edge=%lld tau=%.2f u=%.2f d=%.2f f=%.2f lam=%.1f margin=%.1f\n",
        q_hat,
        (long long)r->t_edge,
        r->tau,
        u_hat,
        d_hat,
        r->f,
        cands[best].lam,
        margin);

    r->lam = cands[best].lam;
    r->margin = margin;
    return 1;
}

/**
 * @file frame_sync_internal.h
 * @brief The frame-sync state, private to frame_sync.c, detect.c and sync.c.
 *        The public contract lives in include/frame_sync.h.
 */

#ifndef FRAME_SYNC_INTERNAL_H
#define FRAME_SYNC_INTERNAL_H

#include <complex.h>
#include <stdint.h>

#include "dsp.h"
#include "frame_sync.h"

/** Detector phase, mirroring the reference's m_state enum. */
enum frame_sync_phase
{
    FS_DETECT = 0,   // sliding decision windows, hunting stable bins
    FS_SYNC = 1,     // frame found; estimating and checking the sync word
    FS_SFO_COMP = 2, // locked; producing corrected payload windows
};

/** SYNC sub-states, mirroring the reference's SyncState enum. */
enum frame_sync_subphase
{
    FS_NET_ID1 = 0,
    FS_NET_ID2,
    FS_DOWNCHIRP1,
    FS_DOWNCHIRP2,
    FS_QUARTER_DOWN,
};

/**
 * Frame synchronization state. The configuration fields are constructor
 * arguments; the result fields are mirrored to the caller through
 * frame_sync_status(); the rest is the state machine's private working set.
 */
struct frame_sync
{
    // configuration (constructor arguments in frame_sync_impl.cc)
    uint8_t spreading_factor;    // spreading factor (7..12)
    uint8_t oversampling_factor; // oversampling: samples per chip
    uint16_t preamble_len;       // expected preamble upchirps
    uint16_t sync_words[2];      // expected network-id symbols
    uint32_t center_freq;        // RF center frequency, Hz (SFO estimate only);
                                 // 0 disables the CFO->SFO inference
    uint32_t bandwidth;          // chirp bandwidth, Hz (SFO estimate only)
    uint32_t number_of_bins;     // N = 2^sf (FFT length, bins)
    uint32_t samples_per_symbol; // N * os_factor (window length)
    int n_up_req;                // stable windows required: len - 3
    uint8_t detect_avg;          // DETECT: windows per averaged spectrum (1 = off)

    // result fields (see frame_sync_status())
    enum frame_sync_phase phase; // current state
    int k_hat;                   // mode bin of the stable preamble run
    int64_t coarse_consume;      // samples to advance after detection; negative
                                 // when the sync word starts inside the fire
                                 // window (see detect.c)
    float cfo;                   // carrier-frequency offset, in FFT bins
    float cfo_applied;           // how much of cfo the preamble buffers are already
                                 // corrected by; the remainder applied once the
                                 // whole-bin part arrives
    float sfo_hat;               // sampling-frequency offset estimate
    int netid_dec[2];            // net-id symbols after full correction
    uint8_t sync_word_rx;        // 1-byte sync word rebuilt from the decoded pair
    uint32_t consume;            // samples consumed by the last step
    uint32_t symb_numb;          // total output windows incl. 8 header
    int received_head;           // header decoded; unlocks the payload budget

    // internal (m_* members of frame_sync_impl.cc)
    int bin_idx;                      // previous window's winning bin
    int symbol_cnt;                   // DETECT: stable windows; SYNC: sub-phase
    float sto_frac;                   // fractional STO estimate in bins
    int cfo_est_done;                 // SYNC: the estimators already ran
    int down_val;                     // SFD downchirp demod value
    float sfo_cum;                    // SFO sample-drift accumulator
    int net_id_off;                   // netid1 - sync_words[0], in [-2, 2]
    int *preamb_up_vals;              // bins of the current stable run
    int additional_upchirps;          // extra preamble upchirps seen in SYNC
    float complex *upchirp;           // reference upchirp, N-point
    float complex *downchirp;         // reference downchirp, N-point
    float complex *in_down;           // decimated decision window
    float complex *out_win;           // SFO_COMPENSATION output window
    float complex *preamble_raw;      // stable-run windows, decimated
    float complex *net_id_samp;       // 2.5-symbol scratch around the sync word
    float complex *preamble_upchirps; // CFO-corrected preamble copy
    float complex *cfo_correc;        // N-point CFO correction vector
    float complex *cfo_correc_aug;    // long correction vector (Bernier stage)
    float complex *cfo_correc_long;   // long correction vector (post-SFD stage)
    float complex *sfo_corr_vect;     // SFO correction vector
    float complex *net_ids_samp_dec;  // decimated net-id pair, 2N
    float complex *symb_corr;         // CFO-corrected decision window
    float complex *rot_tmp;           // scratch for buffer rotations
    void *fft_cfg;                    // N-point FFT plan (dsp.h)
    void *est_fft_cfg;                // 2N-point FFT plan (dsp.h)
    // estimator scratch
    float complex *est_dechirped; // N-point dechirp buffer
    dsp_cpx *est_cx_in;           // 2N-point FFT input
    dsp_cpx *est_cx_out;          // 2N-point FFT output
    float *est_fft_mag;           // 2N-point magnitude-squared spectrum
    float complex *est_fft_val;   // per-window FFT rows, up_symb_to_use x N
    int *est_k0;                  // per-window argmax
    double *est_k0_mag;           // per-window argmax magnitude
    // per-step scratch of get_symbol_val
    dsp_cpx *cx_in;  // FFT input
    dsp_cpx *cx_out; // FFT output
    float *fft_mag;  // magnitude-squared spectrum
    // DETECT averaging scratch (this port; unused when detect_avg <= 1)
    float *detect_mag_hist;   // ring of the last detect_avg spectra
    float *detect_mag_avg;    // their running mean, the decision spectrum
    float *detect_mag_sum;    // sliding sum of the ring (resynced exactly)
    uint8_t detect_hist_fill; // windows currently in the ring
    uint8_t detect_hist_pos;  // next ring slot
    uint32_t detect_win_cnt;  // windows since the last exact sum resync
};

/**
 * @brief Shared dechirp + N-point FFT + raw argmax: the bin of the tone
 *        in one decimated window, or -1 on a zero-energy window. Leaves the
 *        magnitude spectrum in s->fft_mag — an implicit out-parameter every
 *        caller in this module relies on.
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:322; [gr-lora] demod_impl.cc:199-206.
 */
int frame_sync_get_symbol_val(
    struct frame_sync *s, const float complex *samples, const float complex *ref_chirp);

/**
 * @brief Band-limited gather at fractional sample positions:
 *        dst[i] = in[base + i*stride], the continuous position interpolated
 *        by a 2*FRAC_M-tap Kaiser-windowed sinc (weights built once — the
 *        fractional offset is constant across the call, only the integer
 *        part advances with the stride). Taps clamp to [0, len-1].
 *
 * Used by the prologue below and the net-id pair read (sync.c) to apply the
 * STO/SFO estimates at sub-chip precision instead of rounding them onto the
 * os-grid (TODOS.md, fractional-delay decimation taps).
 */
void frame_sync_frac_gather(
    float complex *dst,
    const float complex *in,
    float base,
    int stride,
    uint32_t count,
    uint32_t len);

/**
 * @brief Decimate one oversampled input window into dst
 *        (os_factor stride, sto_frac-corrected at fractional precision,
 *        os/2 phase; in the payload phase the SFO walk rides along). The dst
 *        parameter (a deviation) lets DETECT materialize preamble_raw
 *        lazily (detect.c).
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:499-501.
 */
void frame_sync_downsample_prologue(
    struct frame_sync *s, const float complex *in, float complex *dst);

/**
 * @brief Bernier fractional-CFO estimator over the buffered preamble run.
 *
 * Shared with detect.c: the DETECT->SYNC look-ahead rotates its scanned
 * windows by the estimate before deciding the symbol grid. Side effects (the
 * fractional-CFO-corrected copy in s->preamble_upchirps and the long phasor
 * in s->cfo_correc_aug) are exactly what the SYNC walk's own first-step call
 * produces on the same data.
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:194.
 */
float frame_sync_estimate_cfo_frac_bernier(struct frame_sync *s, const float complex *samples);

#endif // FRAME_SYNC_INTERNAL_H

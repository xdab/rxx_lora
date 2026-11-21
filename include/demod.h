/**
 * @file demod.h
 * @brief LoRa symbol demodulator: hard argmax plus soft bit LLRs.
 */

#ifndef DEMOD_H
#define DEMOD_H

#include <complex.h>
#include <stdint.h>

#include "dsp.h"

/**
 * Hard symbol demodulator state, one instance per stream: it dechirps each
 * symbol window with the CFO-adapted reference chirp, takes the N-point FFT
 * and reads the tone off the magnitude spectrum. Carries the reference
 * chirps, the FFT plan and scratch, and the last window's results (symbol
 * value, peak, noise floor) that the SNR and soft-LLR outputs read.
 */
struct demod_state
{
    // configuration
    uint8_t spreading_factor;
    uint32_t number_of_bins; // N = 2^sf

    // result fields, valid after demod_step()
    int symbol_val;    // LoRa symbol value mod(bin_idx - 1, N)
    float peak_mag;    // the peak bin's power: one fused pass computes these
    float noise_floor; // the blanked noise-floor mean; demod_snr/soft_llrs

    // internal
    float complex *upchirp;   // CFO-int-adapted reference upchirp
    float complex *downchirp; // conjugate, CFO-frac-adapted
    void *fft_cfg;            // N-point FFT plan (dsp.h)
    dsp_cpx *cx_in;           // FFT input
    dsp_cpx *cx_out;          // FFT output
    float *fft_mag;           // magnitude-squared spectrum
    // soft-LLR scratch: per bit, an N-point 0/1 mask of the bins carrying
    // that bit (the gray mapping is fixed per SF, built once in init)
    float *llr_mask; // sf rows of N mask floats
    float *llr_w;    // per-bin soft weight of the last window
};

/**
 * @brief Allocate the demodulator's buffers and FFT plan; call once per run.
 *
 *        Leaves the reference chirps CFO-neutral (as if cfo were 0); call
 *        demod_reset() with the frame's CFO before demodulating it.
 *
 * @param[out] s state to initialize
 * @param[in] sf spreading factor 7..12
 * @return 0 on success, -1 on allocation failure
 *
 * Reference: [gr-lora_sdr] fft_demod_impl.cc:26,283-289; [gr-lora] demod_impl.cc:54.
 */
int demod_init(struct demod_state *s, uint8_t sf);

/**
 * @brief Rebuild the CFO-adapted reference chirps for an incoming frame; call
 *        once per frame, after sync produced its CFO estimate. Allocates
 *        nothing, so it cannot fail.
 *
 * @param[in,out] s state from demod_init()
 * @param[in] cfo carrier-frequency offset in FFT bins
 *
 * Reference: [gr-lora] demod_impl.cc:96-101.
 */
void demod_reset(struct demod_state *s, float cfo);

/**
 * @brief Release everything demod_init() allocated.
 *
 * Reference: [gr-lora_sdr] fft_demod_impl.cc:62; [gr-lora] demod_impl.cc:109.
 */
void demod_cleanup(struct demod_state *s);

/**
 * @brief On-channel SNR of the last demodulated window, linear.
 *
 * The dechirp folds the spreading gain into the tone bin — 10*log10(N) dB
 * above the on-channel SNR — and dividing by N undoes it.
 *
 * @param[in] s state after at least one demod_step
 * @return the linear ratio, 0 when the spectrum is all zeros or the tone is
 *         at or below the floor
 */
float demod_snr(const struct demod_state *s);

/**
 * @brief Demodulate one N-point symbol window.
 *
 * Dechirp with the adapted downchirp, N-point FFT, argmax, then the LoRa
 * symbol map mod(idx - 1, N). Also caches the peak's magnitude and the
 * noise-floor mean (all bins except the tone and its 2-bin skirt each
 * side) for demod_snr() and demod_soft_llrs().
 *
 * @param[in,out] s initialized state
 * @param[in] in window start; N samples must be readable
 * @return the demodulated symbol value, also stored in symbol_val
 *
 * Reference: [gr-lora_sdr] fft_demod_impl.cc:262,301; [gr-lora] demod_impl.cc:174-206.
 */
int demod_step(struct demod_state *s, const float complex *in);

/**
 * @brief Per-bit LLRs of the last demod_step() window, from its bin
 *        energies. Valid only right after demod_step() (its spectrum is
 *        overwritten by the next call).
 *
 * The soft twin of demod_step()'s argmax: with m the bin magnitudes, m_max
 * the peak and floor the cached noise-floor mean, the weight of bin v is
 * exp(a*(m - m_max)) with a = (m_max - floor)/(m_max*floor) — the
 * per-window tone SNR's λ/(1+λ), so it self-calibrates. Bit j's LLR is the
 * log ratio of the summed weights over the bins whose gray-mapped symbol
 * value carries bit j set vs clear (MSB first), matching the bit the
 * deinterleaver extracts. A 1e-12 epsilon caps |llr| on clean windows.
 *
 * @param[in] s state with a fresh demod_step() result
 * @param[out] llr sf floats
 */
void demod_soft_llrs(struct demod_state *s, float *llr);

#endif // DEMOD_H

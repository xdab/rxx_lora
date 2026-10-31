/**
 * @file demod.h
 * @brief LoRa symbol demodulator: hard argmax plus soft bit LLRs.
 */

#ifndef DEMOD_H
#define DEMOD_H

#include <complex.h>
#include <stdint.h>

#include "dsp.h"

struct demod_state
{
    // configuration
    uint8_t spreading_factor;
    uint32_t number_of_bins;     // N = 2^sf
    uint32_t samples_per_symbol; // N (the block runs post-decimation)

    // result fields, valid after demod_step()
    int bin_idx;    // raw FFT argmax of the last window
    int symbol_val; // LoRa symbol value mod(bin_idx - 1, N)

    // internal
    float complex *upchirp;   // CFO-int-adapted reference upchirp
    float complex *downchirp; // conjugate, CFO-frac-adapted
    void *fft_cfg;            // N-point FFT plan (dsp.h)
    dsp_cpx *cx_in;           // FFT input
    dsp_cpx *cx_out;          // FFT output
    float *fft_mag;           // magnitude-squared spectrum

    // DEMOD_A_EMA knob: smoothed LLR metric slope, reset per frame
    float a_ema;
    int a_ema_valid;
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
 */
void demod_reset(struct demod_state *s, float cfo);

/**
 * @brief Release everything demod_init() allocated.
 */
void demod_cleanup(struct demod_state *s);

/**
 * @brief On-channel SNR of the last demodulated window, linear.
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
 * symbol map mod(idx - 1, N).
 *
 * @param[in,out] s initialized state
 * @param[in] in window start; N samples must be readable
 * @return the demodulated symbol value, also stored in symbol_val
 *
 * Reference: [gr-lora_sdr] fft_demod_impl.cc:262; [gr-lora] demod_impl.cc:174-206.
 */
int demod_step(struct demod_state *s, const float complex *in);

/**
 * @brief Per-bit LLRs of the last demod_step() window, from its bin
 *        energies; see demod.c for the metric.
 *
 * @param[in] s state with a fresh demod_step() result
 * @param[out] llr sf floats; llr[j] = LLR that bit j (MSB first) of the
 *             gray-mapped symbol value is 1, matching the bit the
 *             deinterleaver (deinterleave_soft) extracts
 */
void demod_soft_llrs(struct demod_state *s, float *llr);

#endif // DEMOD_H

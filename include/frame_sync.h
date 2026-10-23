/**
 * @file frame_sync.h
 * @brief LoRa frame synchronization: DETECT > SYNC > corrected payload
 *        windows. The state is opaque; the caller drives the state machine
 *        with the step functions and reads results through the snapshot.
 */

#ifndef FRAME_SYNC_H
#define FRAME_SYNC_H

#include <complex.h>
#include <stdint.h>

/**
 * DETECT->SYNC look-ahead (this port): when the detector fires it scans this
 * many windows forward for the sync word's signature (see detect.c), so the
 * raw buffer must stay readable that far past the frame.
 */
#define SYNC_LOOKAHEAD 12

/**
 * Opaque frame-sync state, one instance per input stream. The struct lives in
 * src/frame_sync_internal.h, shared by frame_sync.c/detect.c/sync.c.
 */
struct frame_sync;

/**
 * Read-only copy of the configuration-derived and result fields, for the
 * caller's logging and reporting. A snapshot of the live fields at the call;
 * refresh it after a step whose results you want to read.
 */
struct frame_sync_status
{
    // configuration-derived
    int sf;                      // spreading factor
    int os;                      // oversampling factor, samples per chip
    uint32_t samples_per_symbol; // window length, N * os
    uint32_t number_of_bins;     // N = 2^sf
    uint32_t bandwidth;          // chirp bandwidth, Hz

    // detection results, valid once frame_sync_detect_step() fired
    int k_hat;    // mode bin of the stable preamble run
    int n_up_req; // stable windows required: preamble_len - 3

    // sync results, valid once frame_sync_sync_step() accepted
    int down_val;           // SFD downchirp demod value
    float cfo;              // carrier-frequency offset, in FFT bins
    float sfo_hat;          // sampling-frequency offset estimate
    float sto_frac;         // fractional sampling-time offset, in bins
    int netid_dec[2];       // net-id symbols after full correction
    uint16_t sync_words[2]; // expected network-id symbols
    uint8_t sync_word_rx;   // 1-byte sync word rebuilt from the decoded pair

    // state-machine progress, for trace logging
    int symbol_cnt; // DETECT: stable windows; SYNC: sub-phase
    int bin_idx;    // previous window's winning bin
};

/**
 * @brief Create the state and build the reference chirps.
 *
 * A single-byte sync word (e.g. 0x12) is expanded to the two network-id
 * symbols (nibble << 3); pass 0 to skip the check.
 *
 * @param[in] spreading_factor spreading factor (5..12)
 * @param[in] oversampling_factor oversampling factor (samples per chip)
 * @param[in] preamble_len expected preamble upchirps (>= 6)
 * @param[in] sync_word network id to listen to in single-byte form (or 0, to disable)
 * @param[in] center_freq RF center frequency in Hz (SFO estimate only); 0 disables the inference
 * @param[in] bandwidth chirp bandwidth in Hz (SFO estimate only)
 * @param[in] detect_avg average the magnitude spectra of this many consecutive
 *            DETECT windows before the bin decision (1 = off, values < 1 treated as 1)
 * @return the state, or NULL on allocation failure (stderr printed)
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:25; [gr-lora] demod_impl.cc:54.
 */
struct frame_sync *frame_sync_create(
    uint8_t spreading_factor,
    uint8_t oversampling_factor,
    uint16_t preamble_len,
    uint32_t sync_word,
    uint32_t center_freq,
    uint32_t bandwidth,
    uint8_t detect_avg);

/**
 * @brief Release everything frame_sync_create() allocated.
 */
void frame_sync_destroy(struct frame_sync *fs);

/**
 * @brief Run one DETECT step over the next symbol window.
 *
 * @param[in] in window start; samples_per_symbol samples must be readable
 * @param[out] advance on a fire: samples to skip to the first scanned window
 *             (possibly negative; the sync word may start inside the fire
 *             window), untouched otherwise
 * @param[out] st optional: a status snapshot as of this step's return
 * @return 1 when the detector fires, 0 while hunting, -1 on error
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:505-539; [gr-lora] demod_impl.cc:247.
 */
int frame_sync_detect_step(
    struct frame_sync *fs, const float complex *in, int64_t *advance, struct frame_sync_status *st);

/**
 * @brief Run one SYNC step over the next symbol window.
 *
 * The first call runs the CFO/STO estimators on the buffered preamble; the
 * walk then demodulates the two network-id symbols and the SFD downchirps,
 * completes the CFO estimate, and checks the net ids against sync_words
 * (offset +-2 tolerated, mirroring the reference).
 *
 * @param[in] in window start; 2 * samples_per_symbol samples must be readable
 * @param[out] advance once the sync word is accepted: samples to the first
 *             payload window; 0 while walking or on a mismatch
 * @param[out] st optional: a status snapshot as of this step's return
 * @return 1 once the sync word is accepted, 0 while walking, -1 on a
 *         sync-word mismatch (the state resets to DETECT)
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:545; [gr-lora] demod_impl.cc:279.
 */
int frame_sync_sync_step(
    struct frame_sync *fs,
    const float complex *in,
    uint32_t *advance,
    struct frame_sync_status *st);

/**
 * @brief Produce the next SFO-corrected payload window for demodulation.
 *
 * The first 8 windows are the PHY header; further payload windows follow once
 * the caller decoded the header and unlocked the budget with
 * frame_sync_set_symbol_budget().
 *
 * @param[in] in window start; 2 * samples_per_symbol samples must be readable
 * @param[out] advance samples to move past the returned window
 * @param[out] st optional: a status snapshot as of this step's return
 * @return the corrected window (valid until the next call), or NULL when the
 *         symbol budget is spent
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:845.
 */
const float complex *frame_sync_output_step(
    struct frame_sync *fs,
    const float complex *in,
    uint32_t *advance,
    struct frame_sync_status *st);

/**
 * @brief Unlock a payload budget of @p symb_numb windows. Called once the
 *        header block decoded; until then the output step yields only the
 *        8 header windows.
 */
void frame_sync_set_symbol_budget(struct frame_sync *fs, uint32_t symb_numb);

/**
 * @brief Rewind the state machine to DETECT for the next frame.
 *
 * Called once a frame is finished — decoded payload, exhausted budget, or a
 * rejected sync word.
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:874-880,404-411.
 */
void frame_sync_reset_detect(struct frame_sync *fs);

/**
 * @brief Copy the current configuration and result fields into @p st.
 */
void frame_sync_status(const struct frame_sync *fs, struct frame_sync_status *st);

#endif // FRAME_SYNC_H

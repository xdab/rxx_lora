/**
 * @file sync.h
 * @brief SYNC and SFO_COMPENSATION phases: CFO/STO estimation, sync-word
 *        check, corrected payload windows.
 */

#ifndef SYNC_H
#define SYNC_H

#include "frame_sync.h"

/**
 * @brief Run one SYNC step over the next symbol window.
 *
 * The first call runs the CFO/STO estimators on the buffered preamble; the
 * walk then demodulates the two network-id symbols and the SFD downchirps,
 * completes the CFO estimate, and checks the net ids against sync_words
 * (offset +-2 tolerated, mirroring the reference).
 *
 * @param[in,out] s state with phase FS_SYNC
 * @param[in] in window start; os_factor*(N/2 + N) samples must be readable
 * @return 1 once the sync word is accepted (advance s->consume), 0 while
 *         walking, -1 on sync-word mismatch (state resets to FS_DETECT)
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:545; [gr-lora] demod_impl.cc:279.
 */
int frame_sync_sync_step(struct frame_sync_state *s, const float complex *in);

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
float frame_sync_estimate_cfo_frac_bernier(
    struct frame_sync_state *s, const float complex *samples);

/**
 * @brief Run one SFO_COMPENSATION step: fill out_win with the next
 *        sto_frac-corrected, decimated payload window, applying the SFO
 *        sample-drift bookkeeping to consume.
 *
 * @param[in,out] s state with phase FS_SFO_COMP
 * @param[in] in window start; os_factor*(N/2 + N) samples must be readable
 * @return 0 and out_win filled, or 1 when the symbol budget is spent
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:845.
 */
int frame_sync_output_step(struct frame_sync_state *s, const float complex *in);

#endif // SYNC_H

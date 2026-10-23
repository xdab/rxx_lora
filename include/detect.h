/**
 * @file detect.h
 * @brief DETECT phase: sliding decision windows hunting a stable
 *        preamble-bin run.
 */

#ifndef DETECT_H
#define DETECT_H

#include "frame_sync.h"

/**
 * @brief Run one DETECT step over the next symbol window.
 *
 * @param[in,out] s state with phase FS_DETECT
 * @param[in] in window start; os_factor*(N/2 + N) samples must be readable
 * @return 1 once detected (then advance coarse_consume), 0 while hunting
 *
 * Reference: [gr-lora_sdr] frame_sync_impl.cc:505; [gr-lora] demod_impl.cc:247.
 */
int frame_sync_detect_step(struct frame_sync_state *s, const float complex *in);

#endif // DETECT_H

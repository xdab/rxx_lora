/** @file source.h
 * @brief Pull source presenting the readable cf32 windows the chain runs on.
 */

#ifndef SOURCE_H
#define SOURCE_H

#include <complex.h>
#include <stddef.h>

struct device;

/**
 * @brief Pull source presenting the readable cf32 window the chain's contract
 *        is written against: it pulls device chunks, resamples to bw*os, and
 *        holds just enough history for the rewind a coarse sync can request,
 *        padding the declared look-ahead with zeros at end of stream.
 */
struct source;

/**
 * @brief Create a source over an open device.
 *
 * sf/os fix the retained history; bandwidth (Hz) and os give the working
 * rate bw*os the source resamples the device's native rate to (no-op when
 * they match). conj applies --conj to samples as they enter; dc_block
 * high-passes them with the corner at bandwidth/1000 Hz.
 *
 * @param[in] dev the open device to pull from
 * @param[in] sf spreading factor
 * @param[in] os oversampling factor
 * @param[in] bandwidth chirp bandwidth, Hz
 * @param[in] conj conjugate samples on entry when nonzero
 * @param[in] dc_block run the DC blocker when nonzero
 * @return the source, or NULL on allocation failure
 */
struct source *
source_create(struct device *dev, int sf, int os, double bandwidth, int conj, int dc_block);

/**
 * @brief Release the source.
 */
void source_destroy(struct source *s);

/**
 * @brief Present the window at absolute sample `pos` through `end`.
 *
 * @param[in] s the source
 * @param[in] pos first sample of the window, absolute working-grid position
 * @param[in] end one past the window's last sample
 * @return the window, or NULL when the stream cannot supply it; the pointer
 *         is invalidated by the next source_window/source_advance call
 */
const float complex *source_window(struct source *s, size_t pos, size_t end);

/**
 * @brief Drop history no later step can name, keeping the declared look-ahead
 *        behind scan position `pos`.
 */
void source_advance(struct source *s, size_t pos);

/**
 * @brief Map an absolute working-grid position to the input (device/file)
 *        sample it came from; a pure rate ratio, identity when the device
 *        already runs at bw*os.
 */
size_t source_input_sample(const struct source *s, size_t pos);

/**
 * @brief Wall-clock offset of an absolute working-grid position from stream
 *        start, in seconds.
 */
double source_time_seconds(const struct source *s, size_t pos);

/**
 * @brief Nonzero if the stream ended on a buffer allocation failure.
 */
int source_failed(const struct source *s);

#endif // SOURCE_H

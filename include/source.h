#ifndef SOURCE_H
#define SOURCE_H

#include <complex.h>
#include <stddef.h>

struct device;

/**
 * Pull source presenting the readable cf32 window the chain's contract is
 * written against: it pulls device chunks, resamples to bw*os, and holds just
 * enough history for the rewind a coarse sync can request, padding the
 * declared look-ahead with zeros at end of stream.
 */
struct source;

/**
 * Create a source over an open device. sf/os fix the retained history;
 * bandwidth (Hz) and os give the working rate bw*os the source resamples the
 * device's native rate to (no-op when they match). conj applies --conj to
 * samples as they enter; dc_block high-passes them with the corner at
 * bandwidth/1000 Hz.
 */
struct source *
source_create(struct device *dev, int sf, int os, double bandwidth, int conj, int dc_block);

void source_destroy(struct source *s);

/**
 * Present the window at absolute sample `pos` through `end`, or NULL when the
 * stream cannot supply it. The pointer is invalidated by the next
 * source_window/source_advance call.
 */
const float complex *source_window(struct source *s, size_t pos, size_t end);

/** Drop history no later step can name, keeping the declared look-ahead
 * behind scan position `pos`. */
void source_advance(struct source *s, size_t pos);

/**
 * Map an absolute working-grid position to the input (device/file) sample it
 * came from; a pure rate ratio, identity when the device already runs at
 * bw*os.
 */
size_t source_input_sample(const struct source *s, size_t pos);

/** Wall-clock offset of an absolute working-grid position from stream start,
 * in seconds. */
double source_time_seconds(const struct source *s, size_t pos);

/** Nonzero if the stream ended on a buffer allocation failure. */
int source_failed(const struct source *s);

#endif // SOURCE_H

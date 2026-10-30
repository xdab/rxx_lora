#ifndef RING_H
#define RING_H

#include <complex.h>
#include <stddef.h>

/**
 * Sliding-window sample buffer backing the pull source (source.c): append at
 * the tail, consume from the head, ring_peek() always returns a contiguous
 * pointer into linear storage (the DSP indexes windows in both directions, so
 * a wrapped circular view would not do). Compaction of the consumed head is
 * lazy: ring_drop() moves the live samples down once the dead prefix reaches
 * `chunk`.
 */
struct ring;

struct ring;

/**
 * Create an empty ring; `chunk` is the dead-prefix size (samples) that
 * triggers a compaction. `cap` pre-allocates the storage up front — steady
 * state then never reallocs; 0 defers the allocation to the first
 * ring_reserve. Returns NULL on allocation failure.
 */
struct ring *ring_create(size_t chunk, size_t cap);

void ring_destroy(struct ring *r);

/** Live samples held. */
size_t ring_len(const struct ring *r);

/**
 * Make room for n more appended samples, compacting and/or growing as needed.
 * Returns 0 on success, -1 on allocation failure.
 */
int ring_reserve(struct ring *r, size_t n);

/** Append n samples; ring_reserve(r, n) must have succeeded first. */
void ring_write(struct ring *r, const float complex *in, size_t n);

/**
 * Zero-copy append: the tail of the room ring_reserve(r, n) just made,
 * to be filled by the caller (a device read or a resampler's out buffer).
 * ring_commit(r, filled) then makes `filled` (<= n) samples live.
 */
float complex *ring_write_span(struct ring *r, size_t n);
void ring_commit(struct ring *r, size_t n);

/** Drop the n oldest samples (n <= ring_len); compacts when the dead prefix
 * reaches `chunk`. */
void ring_drop(struct ring *r, size_t n);

/**
 * Contiguous view of the samples starting at offset off from the oldest
 * (the caller bounds its reads by ring_len() - off). Valid until the next
 * reserve/write/drop.
 */
const float complex *ring_peek(const struct ring *r, size_t off);

#endif // RING_H

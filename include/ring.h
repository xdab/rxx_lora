/** @file ring.h
 * @brief Sliding-window sample buffer backing the pull source (source.c).
 */

#ifndef RING_H
#define RING_H

#include <complex.h>
#include <stddef.h>

/**
 * @brief Sliding-window sample buffer: append at the tail, consume from the
 *        head, ring_peek() always returns a contiguous pointer into linear
 *        storage (the DSP indexes windows in both directions, so a wrapped
 *        circular view would not do). Compaction of the consumed head is
 *        lazy: ring_drop() moves the live samples down once the dead prefix
 *        reaches `chunk`.
 */
struct ring;

/**
 * @brief Create an empty ring.
 *
 * @param[in] chunk dead-prefix size (samples) that triggers a compaction
 * @param[in] cap pre-allocated storage; 0 defers it to the first ring_reserve
 * @return the ring, or NULL on allocation failure
 */
struct ring *ring_create(size_t chunk, size_t cap);

/**
 * @brief Release the ring.
 */
void ring_destroy(struct ring *r);

/**
 * @brief Live samples held.
 */
size_t ring_len(const struct ring *r);

/**
 * @brief Make room for n more appended samples, compacting and/or growing as
 *        needed.
 *
 * @param[in] r the ring
 * @param[in] n samples to make room for
 * @return 0 on success, -1 on allocation failure
 */
int ring_reserve(struct ring *r, size_t n);

/**
 * @brief Append n samples.
 *
 * ring_reserve(r, n) must have succeeded first.
 *
 * @param[in] r the ring
 * @param[in] in samples to append
 * @param[in] n sample count
 */
void ring_write(struct ring *r, const float complex *in, size_t n);

/**
 * @brief Zero-copy append: the tail of the room ring_reserve(r, n) just
 *        made, to be filled by the caller.
 *
 * ring_commit(r, filled) makes `filled` (<= n) live.
 *
 * @param[in] r the ring
 * @param[in] n reserved room to expose
 * @return the writable span, or NULL when no room was reserved
 */
float complex *ring_write_span(struct ring *r, size_t n);

/**
 * @brief Make `filled` (<= n) of the last ring_write_span() samples live.
 */
void ring_commit(struct ring *r, size_t n);

/**
 * @brief Drop the n oldest samples (n <= ring_len); compacts when the dead
 *        prefix reaches `chunk`.
 */
void ring_drop(struct ring *r, size_t n);

/**
 * @brief Contiguous view of the samples starting at offset off from the
 *        oldest (the caller bounds its reads by ring_len() - off). Valid
 *        until the next reserve/write/drop.
 */
const float complex *ring_peek(const struct ring *r, size_t off);

#endif // RING_H

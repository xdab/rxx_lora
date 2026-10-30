#include "ring.h"

#include <complex.h>
#include <stdlib.h>
#include <string.h>

struct ring
{
    float complex *buf; // live samples, oldest at buf[start]
    size_t cap;         // allocated samples
    size_t start;       // dead prefix before the oldest live sample
    size_t len;         // live samples
    size_t chunk;       // dead-prefix threshold that triggers a compaction
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // config-style, one call per producer
struct ring *ring_create(size_t chunk, size_t cap)
{
    struct ring *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;

    r->chunk = chunk ? chunk : 1;

    if (cap)
    {
        r->buf = malloc(cap * sizeof(*r->buf));
        if (!r->buf)
        {
            free(r);
            return NULL;
        }
        r->cap = cap;
    }

    return r;
}

void ring_destroy(struct ring *r)
{
    if (!r)
        return;
    free(r->buf);
    free(r);
}

size_t ring_len(const struct ring *r) { return r->len; }

// Move the live samples to the front, discarding the dead prefix.
static void ring_compact(struct ring *r)
{
    if (r->start == 0)
        return;

    if (r->len)
        memmove(r->buf, r->buf + r->start, r->len * sizeof(*r->buf));

    r->start = 0;
}

int ring_reserve(struct ring *r, size_t n)
{
    if (r->start + r->len + n <= r->cap)
        return 0;

    ring_compact(r);

    if (r->len + n <= r->cap)
        return 0;

    // keep a full `chunk` of dead-prefix room so drops stay lazy between grows
    size_t cap = r->cap ? r->cap : 4096;
    while (cap < r->len + n + r->chunk)
        cap += cap / 2 + 4096;

    float complex *b = realloc(r->buf, cap * sizeof(*b));
    if (!b)
        return -1;

    r->buf = b;
    r->cap = cap;

    return 0;
}

float complex *ring_write_span(struct ring *r, size_t n)
{
    // ring_reserve(r, n) must have succeeded: the room exists past start+len
    return r->buf + r->start + r->len;
}

void ring_commit(struct ring *r, size_t n) { r->len += n; }

void ring_write(struct ring *r, const float complex *in, size_t n)
{
    if (!n)
        return;

    memcpy(ring_write_span(r, n), in, n * sizeof(*in));

    ring_commit(r, n);
}

void ring_drop(struct ring *r, size_t n)
{
    if (n >= r->len)
    {
        r->start = 0;
        r->len = 0;
        return;
    }

    r->start += n;
    r->len -= n;

    if (r->start >= r->chunk)
        ring_compact(r);
}

const float complex *ring_peek(const struct ring *r, size_t off) { return r->buf + r->start + off; }

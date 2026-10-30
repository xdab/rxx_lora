#include "source.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "dsp.h"
#include "frame_sync.h" // SYNC_LOOKAHEAD
#include "logging.h"
#include "resample.h"
#include "ring.h"

struct source
{
    struct device *dev;
    struct resampler *rs; // NULL: the device is already at the working rate
    struct ring *ring;    // valid samples; ring index 0 is absolute sample `off`
    size_t off;           // absolute position of the oldest buffered sample
    size_t slack;         // retained history behind the scan, and the end-of-stream pad
    long total;           // data length once the stream has ended (before padding)
    double rate_in;       // device (input/file) rate, for source_input_sample
    double rate_out;      // working rate bw*os
    int eof;
    int err;            // allocation failure: folds into the end-of-stream return
    int conj;           // --conj: conjugate samples as they enter
    int dc_block_on;    // --dc-block: high-pass the device stream
    struct dc_block dc; // the blocker's state, when dc_block_on
};

#define SRC_CHUNK (1 << 14) // device samples pulled per read

/* End of stream: note the data length, pad the look-ahead zeros. Returns
 * whether `need` is covered. */
static int source_eof_pad(struct source *s, size_t need)
{
    s->eof = 1;
    s->total = (long)(s->off + ring_len(s->ring));
    if (ring_reserve(s->ring, s->slack) != 0)
    {
        log_error("out of memory padding the stream tail\n");
        s->err = 1;
        return 0;
    }
    memset(ring_write_span(s->ring, s->slack), 0, s->slack * sizeof(float complex));
    ring_commit(s->ring, s->slack);
    return need <= (size_t)s->total;
}

/* Ensure absolute sample `need` is buffered, plus the declared look-ahead.
 * Returns 1 when `need` is available, 0 when the stream is spent or a buffer
 * allocation failed (`err`). The final filter-tail samples of a finite
 * resampled stream are not driven out here; the zero pad covers the
 * look-ahead they would have filled. */
static int source_fill(struct source *s, size_t need)
{
    if (s->err)
        return 0;

    if (s->eof)
        return need <= (size_t)s->total;

    size_t want = need + s->slack;
    while (s->off + ring_len(s->ring) < want)
    {
        size_t nr;

        if (s->rs)
        {
            // the resampler consumes a private input chunk
            float complex in[SRC_CHUNK];

            nr = device_read(s->dev, in, SRC_CHUNK);
            if (nr == 0)
                return source_eof_pad(s, need);

            if (s->conj)
                for (size_t i = 0; i < nr; i++)
                    in[i] = conjf(in[i]);

            if (s->dc_block_on)
                dc_block_apply(&s->dc, in, nr);

            size_t space = resampler_push_space(s->rs, nr) + 1;
            if (ring_reserve(s->ring, space) != 0)
            {
                log_error("out of memory buffering the input stream\n");
                s->err = 1;
                return 0;
            }

            // the resampler fills the ring's tail directly, no staging copy
            float complex *span = ring_write_span(s->ring, space);
            ring_commit(s->ring, resampler_push(s->rs, in, nr, span, space));
        }
        else
        {
            // rate-matched: read straight into the ring's tail, no chunk copy
            if (ring_reserve(s->ring, SRC_CHUNK) != 0)
            {
                log_error("out of memory buffering the input stream\n");
                s->err = 1;
                return 0;
            }

            float complex *span = ring_write_span(s->ring, SRC_CHUNK);
            nr = device_read(s->dev, span, SRC_CHUNK);
            if (nr == 0)
                return source_eof_pad(s, need);

            if (s->conj)
                for (size_t i = 0; i < nr; i++)
                    span[i] = conjf(span[i]);

            if (s->dc_block_on)
                dc_block_apply(&s->dc, span, nr);

            ring_commit(s->ring, nr);
        }
    }

    return 1;
}

struct source *
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // config-style, one call per binary
source_create(struct device *dev, int sf, int os, double bandwidth, int conj, int dc_block)
{
    struct source *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->dev = dev;
    s->conj = conj;
    s->dc_block_on = dc_block;

    // the declared look-ahead: half a chip past any window the decimator
    // touches, SYNC_LOOKAHEAD windows of network-id scan after a detection
    size_t sps = (size_t)(1u << sf) * (size_t)os;
    s->slack = (size_t)(SYNC_LOOKAHEAD + 1) * sps + (size_t)os / 2;

    /* Reclaim the consumed head in chunks of a few look-aheads, so a steady
     * scan pays one ring compaction per chunk instead of one per window. The
     * pre-sized capacity covers the steady state — dead prefix below `chunk`,
     * a slack plus a read's worth of live samples — so it never reallocs. */
    s->ring = ring_create(4 * s->slack, 6 * s->slack + 2 * (size_t)SRC_CHUNK);
    if (!s->ring)
    {
        free(s);
        return NULL;
    }

    /* Input-rate conversion: the chain runs at exactly bw*os; a device at any
     * other native rate is pulled through a streaming resampler as the chain
     * asks for samples. */
    double rate_target = bandwidth * (double)os;
    double dev_rate = device_rate(dev);
    if (dev_rate <= 0.0)
        dev_rate = rate_target;

    s->rate_in = dev_rate;
    s->rate_out = rate_target;

    /* Strip the zero-IF tuner's DC offset and 1/f junk at the device rate,
     * before the resampler; the corner is far below the signal. */
    if (s->dc_block_on)
        dc_block_init(&s->dc, dev_rate, bandwidth / 1000.0);

    if (fabs(dev_rate - rate_target) > 1e-6 * rate_target)
    {
        s->rs = resampler_create(dev_rate, rate_target, bandwidth);
        if (!s->rs)
        {
            source_destroy(s);
            return NULL;
        }
    }
    return s;
}

void source_destroy(struct source *s)
{
    if (!s)
        return;
    resampler_destroy(s->rs);
    ring_destroy(s->ring);
    free(s);
}

const float complex *source_window(struct source *s, size_t pos, size_t end)
{
    if (!source_fill(s, end))
        return NULL;

    return ring_peek(s->ring, pos - s->off);
}

void source_advance(struct source *s, size_t pos)
{
    size_t keep = pos > s->slack ? pos - s->slack : 0;
    if (keep > s->off)
    {
        ring_drop(s->ring, keep - s->off);
        s->off = keep;
    }
}

size_t source_input_sample(const struct source *s, size_t pos)
{
    if (!s->rs)
        return pos;

    return (size_t)llround((double)pos * s->rate_in / s->rate_out);
}

/* Wall-clock offset of an absolute working-grid position from the start of
 * the stream, in seconds (pos/rate_out == input_sample/rate_in). */
double source_time_seconds(const struct source *s, size_t pos)
{
    return s->rate_out > 0.0 ? (double)pos / s->rate_out : 0.0;
}

int source_failed(const struct source *s) { return s->err; }

/** @file device_stream.h
 * @brief Shared plumbing of the two live device back ends (device_rtl.c,
 *        device_rsp.c): the buffered ring with its drop accounting, the
 *        stream-health report, and the consumer's timed wait. The hardware
 *        open/config/callback code stays in each back end.
 */

#ifndef DEVICE_STREAM_H
#define DEVICE_STREAM_H

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "ring.h"

#define STREAM_REPORT_SECONDS 60.0
#define STREAM_LOSS_PPM 1000.0 // warn above 0.1%

// embedded in each back end's device struct; all access under `lock`
struct stream
{
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct ring *ring;            // buffered device samples, oldest first
    size_t ring_cap;              // soft bound; the oldest samples are dropped beyond it
    unsigned long long delivered; // samples received from the device
    unsigned long long dropped;   // samples lost to a full ring
    double rate;                  // the nominal stream rate, for the health report
    double open_time;             // device-open reference for the delivered rate
    double report_time;
};

static double mono_seconds(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);

    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// make room for m more samples, dropping the oldest past the soft cap
static void stream_make_room(struct stream *s, size_t m)
{
    if (ring_len(s->ring) + m > s->ring_cap)
    {
        size_t drop = ring_len(s->ring) + m - s->ring_cap;
        ring_drop(s->ring, drop);
        s->dropped += drop;
    }
}

// periodic stream health: rate, ring drops, samples short of nominal; called
// with the stream lock held. @p label names the back end in the messages.
static void stream_report(struct stream *s, const char *label, double loss_ppm)
{
    double now = mono_seconds();
    double elapsed = now - s->open_time;
    double want = s->rate * elapsed;
    double ppm = want > 0.0 ? 1e6 * (want - (double)s->delivered) / want : 0.0;
    if (s->dropped)
        log_warn(
            "%s stream: %.0f samples/s over %.0f s, ring drops %llu: "
            "samples were lost to a full buffer\n",
            label,
            (double)s->delivered / elapsed,
            elapsed,
            s->dropped);
    else if (elapsed > 300.0 && ppm > loss_ppm)
        log_warn(
            "%s stream short by %.0f samples over %.0f s (%.0f ppm): "
            "the device dropped samples\n",
            label,
            want - (double)s->delivered,
            elapsed,
            ppm);
    else
        log_info(
            "%s stream: %.0f samples/s over %.0f s (%.0f ppm vs nominal)\n",
            label,
            (double)s->delivered / elapsed,
            elapsed,
            ppm);
    s->report_time = now;
}

// wait for samples to read; @p stop and @p over end the wait (stop also
// yields a zero read even when samples remain). Callers hold no lock.
static size_t stream_read(
    struct stream *s,
    const volatile sig_atomic_t *stop,
    const volatile sig_atomic_t *over,
    float complex *out,
    size_t n)
{
    pthread_mutex_lock(&s->lock);
    while (ring_len(s->ring) == 0 && !*stop && !*over)
    {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100000000; // 100 ms: notice a stop without a wakeup
        if (deadline.tv_nsec >= 1000000000)
        {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&s->cond, &s->lock, &deadline);
    }
    size_t m = *stop ? 0 : ring_len(s->ring);
    if (m > n)
        m = n;
    if (m)
    {
        memcpy(out, ring_peek(s->ring, 0), m * sizeof(*out));
        ring_drop(s->ring, m);
    }
    pthread_mutex_unlock(&s->lock);

    return m;
}

// release the stream's own resources (the device struct is the caller's)
static void stream_free(struct stream *s)
{
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    ring_destroy(s->ring);
}

#endif // DEVICE_STREAM_H

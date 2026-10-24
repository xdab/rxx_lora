#include "device.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <rtl-sdr.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "ring.h"

#define RTL_RATE_MIN_LOW 225001
#define RTL_RATE_MAX_LOW 300000
#define RTL_RATE_MIN_HIGH 900001
#define RTL_RATE_MAX_HIGH 3200000

#define RTL_XFER_BYTES (512 * 512)
#define RTL_XFER_SAMPLES (RTL_XFER_BYTES / 2)
#define RTL_XFER_BUFS 15

#define RTL_RING_SECONDS 2.0
#define RTL_REPORT_SECONDS 60.0
#define RTL_LOSS_PPM 1000.0 // warn above 0.1%

#define RTL_MAX_TUNER_GAINS 128

struct rtl_device
{
    struct device base;
    rtlsdr_dev_t *dev;
    double rate; // the actually-set (snapped) rate
    volatile sig_atomic_t stop;
    FILE *record; // -R: raw cf32 tee, NULL when not recording

    pthread_t reader;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct ring *ring;            // buffered device samples, oldest first
    size_t ring_cap;              // soft bound; the oldest samples are dropped beyond it
    float complex *conv;          // callback scratch: raw bytes -> cf32
    unsigned long long delivered; // samples received from the device
    unsigned long long dropped;   // samples lost to a full ring
    double open_time;             // device-open reference for the delivered rate
    double report_time;
    int reader_started;
    int reader_done;
};

static double mono_seconds(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);

    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static uint32_t snap_rate(double want)
{
    uint32_t r = (uint32_t)(want + 0.5);

    if (r < RTL_RATE_MIN_LOW)
        r = RTL_RATE_MIN_LOW;
    else if (r > RTL_RATE_MAX_LOW && r < RTL_RATE_MIN_HIGH)
        r = RTL_RATE_MIN_HIGH;
    else if (r > RTL_RATE_MAX_HIGH)
        r = RTL_RATE_MAX_HIGH;

    return r;
}

// nearest available tuner gain (tenths of a dB) to the requested target
static int nearest_gain(rtlsdr_dev_t *dev, int target_tenths)
{
    int gains[RTL_MAX_TUNER_GAINS];

    int count = rtlsdr_get_tuner_gains(dev, NULL);
    if (count <= 0)
        return target_tenths;
    if (count > RTL_MAX_TUNER_GAINS)
        count = RTL_MAX_TUNER_GAINS;
    count = rtlsdr_get_tuner_gains(dev, gains);

    int best = gains[0];
    for (int i = 1; i < count; i++)
        if (abs(target_tenths - gains[i]) < abs(target_tenths - best))
            best = gains[i];

    return best;
}

/* Called on librtlsdr's event thread for every completed bulk transfer. Keep it
 * short: converting, teeing and enqueueing is all that may happen here, because
 * the next transfer is only re-posted after this returns (the other buffers
 * stay in flight meanwhile). */
static void rtl_async_cb(unsigned char *buf, uint32_t len, void *ctx)
{
    struct rtl_device *s = (struct rtl_device *)ctx;
    size_t m = len / 2;

    for (size_t i = 0; i < m; i++)
    {
        // bytes are centered at 127; scale to +/-1.0 full scale like a file
        float re = (float)((int)buf[2 * i] - 127);
        float im = (float)((int)buf[2 * i + 1] - 127);
        s->conv[i] = (re + I * im) * (1.0f / 128.0f);
    }
    if (s->record && fwrite(s->conv, sizeof(*s->conv), m, s->record) != m)
    {
        log_warn("IQ recording write failed; recording stopped\n");
        (void)fclose(s->record);
        s->record = NULL;
    }

    pthread_mutex_lock(&s->lock);
    s->delivered += m;
    if (ring_len(s->ring) + m > s->ring_cap)
    {
        size_t drop = ring_len(s->ring) + m - s->ring_cap;
        ring_drop(s->ring, drop);
        s->dropped += drop;
    }
    if (ring_reserve(s->ring, m) == 0)
        ring_write(s->ring, s->conv, m);
    else
        s->dropped += m;

    double now = mono_seconds();
    if (now - s->report_time >= RTL_REPORT_SECONDS)
    {
        double elapsed = now - s->open_time;
        double want = s->rate * elapsed;
        double ppm = want > 0.0 ? 1e6 * (want - (double)s->delivered) / want : 0.0;
        if (s->dropped)
            log_warn(
                "RTL stream: %.0f samples/s over %.0f s, ring drops %llu: "
                "samples were lost to a full buffer\n",
                (double)s->delivered / elapsed,
                elapsed,
                s->dropped);
        else if (elapsed > 300.0 && ppm > RTL_LOSS_PPM)
            log_warn(
                "RTL stream short by %.0f samples over %.0f s (%.0f ppm): "
                "the device dropped samples\n",
                want - (double)s->delivered,
                elapsed,
                ppm);
        else
            log_info(
                "RTL stream: %.0f samples/s over %.0f s (%.0f ppm vs nominal)\n",
                (double)s->delivered / elapsed,
                elapsed,
                ppm);
        s->report_time = now;
    }
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static void *rtl_reader(void *arg)
{
    struct rtl_device *s = (struct rtl_device *)arg;

    int r = rtlsdr_read_async(s->dev, rtl_async_cb, s, RTL_XFER_BUFS, RTL_XFER_BYTES);
    if (r < 0 && !s->stop)
        log_error("RTL async read failed (%d)\n", r);

    pthread_mutex_lock(&s->lock);
    s->reader_done = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    return NULL;
}

static size_t rtl_read(struct device *d, float complex *out, size_t n)
{
    struct rtl_device *s = (struct rtl_device *)d;

    pthread_mutex_lock(&s->lock);
    while (ring_len(s->ring) == 0 && !s->stop && !s->reader_done)
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
    size_t m = s->stop ? 0 : ring_len(s->ring);
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

static double rtl_rate(const struct device *d) { return ((const struct rtl_device *)d)->rate; }

static void rtl_stop(struct device *d) { ((struct rtl_device *)d)->stop = 1; }

static void rtl_free(struct rtl_device *s)
{
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    ring_destroy(s->ring);
    free(s->conv);
    free(s);
}

static void rtl_close(struct device *d)
{
    struct rtl_device *s = (struct rtl_device *)d;

    rtl_stop(d);
    if (s->reader_started)
    {
        rtlsdr_cancel_async(s->dev);
        pthread_join(s->reader, NULL);
    }
    if (s->record && fclose(s->record) != 0)
        log_warn("IQ recording: final flush failed\n");
    if (s->dev)
        rtlsdr_close(s->dev);
    rtl_free(s);
}

static const struct device_ops rtl_ops = {rtl_read, rtl_rate, rtl_stop, rtl_close};

struct device *device_open_rtl(
    int index,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // config-style, one call site (main_rtl)
    double center_hz,
    double capture_hz,
    double gain_db,
    int ppm,
    int biastee,
    const char *record_path)
{
    if (rtlsdr_get_device_count() <= 0)
    {
        log_error("no RTL-SDR devices found\n");
        return NULL;
    }

    if (index < 0)
        index = 0;

    rtlsdr_dev_t *dev = NULL;
    if (rtlsdr_open(&dev, (uint32_t)index) < 0)
    {
        log_error("cannot open RTL-SDR %d\n", index);
        return NULL;
    }

    uint32_t rate = snap_rate(capture_hz);

    if (rtlsdr_set_sample_rate(dev, rate) < 0)
        log_warn("RTL: failed to set sample rate %u Hz\n", rate);

    if (fabs((double)rate - capture_hz) > 1.0)
        log_info("RTL rate snapped %.0f -> %u Hz\n", capture_hz, rate);

    if (rtlsdr_set_center_freq(dev, (uint32_t)(center_hz + 0.5)) < 0)
        log_warn("RTL: failed to tune to %.0f Hz\n", center_hz);

    if (gain_db < 0.0)
    {
        if (rtlsdr_set_tuner_gain_mode(dev, 0) == 0)
            log_info("RTL tuner gain: automatic\n");
        else
            log_warn("RTL: failed to set automatic gain\n");
    }
    else
    {
        rtlsdr_set_tuner_gain_mode(dev, 1);
        int g = nearest_gain(dev, (int)(gain_db * 10.0));
        rtlsdr_set_tuner_gain(dev, g);
        log_info("RTL tuner gain: %.1f dB\n", g / 10.0);
    }

    if (ppm != 0)
        rtlsdr_set_freq_correction(dev, ppm);

    if (biastee)
        rtlsdr_set_bias_tee(dev, 1);

    rtlsdr_reset_buffer(dev);

    struct rtl_device *s = calloc(1, sizeof(*s));
    if (!s)
    {
        rtlsdr_close(dev);
        return NULL;
    }

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->ring = ring_create(1 << 16);
    s->ring_cap = (size_t)(rate * RTL_RING_SECONDS);
    s->conv = malloc(RTL_XFER_SAMPLES * sizeof(*s->conv));

    if (!s->ring || !s->conv)
    {
        log_error("out of memory buffering the RTL stream\n");
        rtl_free(s);
        rtlsdr_close(dev);
        return NULL;
    }

    if (record_path)
    {
        s->record = fopen(record_path, "wb");
        if (!s->record)
        {
            log_error("cannot open %s for IQ recording\n", record_path);
            rtl_free(s);
            rtlsdr_close(dev);
            return NULL;
        }
    }

    s->base.ops = &rtl_ops;
    s->dev = dev;
    s->rate = rate;
    s->open_time = s->report_time = mono_seconds();

    s->reader_started = pthread_create(&s->reader, NULL, rtl_reader, s) == 0;
    if (!s->reader_started)
    {
        log_error("cannot start the RTL reader thread\n");
        if (s->record)
            (void)fclose(s->record);
        rtl_free(s);
        rtlsdr_close(dev);
        return NULL;
    }

    log_info(
        "RTL-SDR %d (%s): %u Hz, center %.6f MHz\n",
        index,
        rtlsdr_get_device_name((uint32_t)index),
        rate,
        center_hz / 1e6);

    return &s->base;
}

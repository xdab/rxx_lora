#include "device.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <sdrplay_api.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "logging.h"
#include "ring.h"

/* The API only offers these output rates: the ADC always runs at 8 MHz and
 * the service decimates by {4, 2, 1}. Requested captures snap up. */
#define RSP_RATE_2M 2000000
#define RSP_RATE_4M 4000000
#define RSP_RATE_8M 8000000

#define RSP_RING_SECONDS 2.0
#define RSP_REPORT_SECONDS 60.0
#define RSP_LOSS_PPM 1000.0 // warn above 0.1%

/* Drop the first 300 ms after sdrplay_api_Init: DC-offset calibration
 * transient (zero-IF), far louder than anything decodeable. */
#define RSP_STARTUP_DROP_SECONDS 0.3

/* Manual-gain defaults when only one of -g/-L was given: most IF gain
 * reduction, most LNA gain. Under AGC the API still validates these fields
 * as its starting point, so the same defaults apply. */
#define RSP_DEFAULT_GRDB 59
#define RSP_DEFAULT_LNA_STATE 0

/* Callback conversion scratch; stream dribbles are ~1344 samples, but the
 * API does not bound numSamples by contract, so convert in slices. */
#define RSP_CONV_SAMPLES 16384

struct rsp_device
{
    struct device base;
    sdrplay_api_DeviceT info; // the selected device (SerNo, hwVer, handle)
    double rate;              // the actually-set (snapped) rate
    volatile sig_atomic_t stop;
    volatile sig_atomic_t failed; // API lost the device; stream is over
    int inited;                   // sdrplay_api_Init succeeded
    int selected;                 // sdrplay_api_SelectDevice succeeded
    int api_open;                 // sdrplay_api_Open succeeded
    FILE *record;                 // -R: raw cf32 tee, NULL when not recording

    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct ring *ring;               // buffered device samples, oldest first
    size_t ring_cap;                 // soft bound; the oldest samples are dropped beyond it
    float complex *conv;             // callback scratch: 16-bit pairs -> cf32
    unsigned long long startup_drop; // samples of the calibration transient left
    unsigned long long delivered;    // samples received from the device
    unsigned long long dropped;      // samples lost to a full ring
    double open_time;                // device-open reference for the delivered rate
    double report_time;
};

static sdrplay_api_DeviceT rsp_devices[SDRPLAY_MAX_DEVICES];
static unsigned int rsp_num_devices;

static double mono_seconds(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);

    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static uint32_t snap_rate(double want)
{
    if (want <= (double)RSP_RATE_2M)
        return RSP_RATE_2M;
    if (want <= (double)RSP_RATE_4M)
        return RSP_RATE_4M;
    return RSP_RATE_8M;
}

static void rsp_fail(struct rsp_device *s, const char *why)
{
    log_error("RSP: %s\n", why);
    s->failed = 1;
    pthread_mutex_lock(&s->lock);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

/* Called on the API's stream thread with zero-IF samples. The 8 MHz ADC +
 * service decimation delivers right-justified 16-bit pairs; scale to the
 * cf32 +-1.0 full scale like a file. */
static void rsp_stream_cb(
    short *xi,
    short *xq,
    sdrplay_api_StreamCbParamsT *params,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // sdrplay_api_StreamCallback_t shape
    unsigned int num_samples,
    unsigned int reset,
    void *ctx)
{
    struct rsp_device *s = (struct rsp_device *)ctx;

    (void)params;
    (void)reset;

    if (s->stop || s->failed)
        return;

    if (s->startup_drop > 0)
    {
        unsigned long long skip = s->startup_drop;
        if (skip > num_samples)
            skip = num_samples;
        xi += skip;
        xq += skip;
        num_samples -= (unsigned int)skip;
        s->startup_drop -= skip;
        if (num_samples == 0)
            return;
    }

    size_t m = num_samples;
    pthread_mutex_lock(&s->lock);
    if (ring_len(s->ring) + m > s->ring_cap)
    {
        size_t drop = ring_len(s->ring) + m - s->ring_cap;
        ring_drop(s->ring, drop);
        s->dropped += drop;
    }

    if (ring_reserve(s->ring, m) != 0)
    {
        s->dropped += m;
        pthread_mutex_unlock(&s->lock);
        return;
    }

    for (size_t off = 0; off < m; off += RSP_CONV_SAMPLES)
    {
        size_t take = m - off < RSP_CONV_SAMPLES ? m - off : RSP_CONV_SAMPLES;

        for (size_t i = 0; i < take; i++)
        {
            float re = (float)xi[off + i] * (1.0f / 32768.0f);
            float im = (float)xq[off + i] * (1.0f / 32768.0f);
            s->conv[i] = re + I * im;
        }
        ring_write(s->ring, s->conv, take);
        if (s->record && fwrite(s->conv, sizeof(*s->conv), take, s->record) != take)
        {
            log_warn("IQ recording write failed; recording stopped\n");
            (void)fclose(s->record);
            s->record = NULL;
        }
    }

    s->delivered += m;

    double now = mono_seconds();
    if (now - s->report_time >= RSP_REPORT_SECONDS)
    {
        double elapsed = now - s->open_time;
        double want = s->rate * elapsed;
        double ppm = want > 0.0 ? 1e6 * (want - (double)s->delivered) / want : 0.0;
        if (s->dropped)
            log_warn(
                "RSP stream: %.0f samples/s over %.0f s, ring drops %llu: "
                "samples were lost to a full buffer\n",
                (double)s->delivered / elapsed,
                elapsed,
                s->dropped);
        else if (elapsed > 300.0 && ppm > RSP_LOSS_PPM)
            log_warn(
                "RSP stream short by %.0f samples over %.0f s (%.0f ppm): "
                "the device dropped samples\n",
                want - (double)s->delivered,
                elapsed,
                ppm);
        else
            log_info(
                "RSP stream: %.0f samples/s over %.0f s (%.0f ppm vs nominal)\n",
                (double)s->delivered / elapsed,
                elapsed,
                ppm);
        s->report_time = now;
    }
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

/* Runs on the API's event thread; the power-overload ack must be issued
 * from here for the API to keep reporting. */
static void rsp_event_cb(
    sdrplay_api_EventT event,
    sdrplay_api_TunerSelectT tuner,
    sdrplay_api_EventParamsT *params,
    void *ctx)
{
    struct rsp_device *s = (struct rsp_device *)ctx;

    switch (event)
    {
    case sdrplay_api_GainChange:
        log_debug(
            "RSP AGC: %.2f dB (gRdB %u, LNA %u)\n",
            params->gainParams.currGain,
            params->gainParams.gRdB,
            params->gainParams.lnaGRdB);
        break;
    case sdrplay_api_PowerOverloadChange:
        log_warn(
            "RSP ADC overload %s\n",
            params->powerOverloadParams.powerOverloadChangeType == sdrplay_api_Overload_Detected
                ? "detected"
                : "corrected");
        (void)sdrplay_api_Update(
            s->info.dev,
            tuner,
            sdrplay_api_Update_Ctrl_OverloadMsgAck,
            sdrplay_api_Update_Ext1_None);
        break;
    case sdrplay_api_DeviceRemoved:
        rsp_fail(s, "device removed (unplugged)");
        break;
    case sdrplay_api_DeviceFailure:
        rsp_fail(s, "device failure reported by the API");
        break;
    default:
        break;
    }
}

static size_t rsp_read(struct device *d, float complex *out, size_t n)
{
    struct rsp_device *s = (struct rsp_device *)d;

    pthread_mutex_lock(&s->lock);
    while (ring_len(s->ring) == 0 && !s->stop && !s->failed)
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

static double rsp_rate(const struct device *d) { return ((const struct rsp_device *)d)->rate; }

static void rsp_stop(struct device *d) { ((struct rsp_device *)d)->stop = 1; }

static void rsp_free(struct rsp_device *s)
{
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    ring_destroy(s->ring);
    free(s->conv);
    free(s);
}

static void rsp_close(struct device *d)
{
    struct rsp_device *s = (struct rsp_device *)d;

    rsp_stop(d);
    if (s->inited)
        (void)sdrplay_api_Uninit(s->info.dev);
    if (s->record && fclose(s->record) != 0)
        log_warn("IQ recording: final flush failed\n");
    if (s->selected)
        (void)sdrplay_api_ReleaseDevice(&s->info);
    if (s->api_open)
        (void)sdrplay_api_Close();
    rsp_free(s);
}

static const struct device_ops rsp_ops = {rsp_read, rsp_rate, rsp_stop, rsp_close};

static int rsp_api_open(struct rsp_device *s)
{
    sdrplay_api_ErrT err;
    float ver = 0.0f;

    err = sdrplay_api_Open();
    if (err != sdrplay_api_Success)
    {
        log_error(
            "sdrplay_api_Open failed: %s (is the sdrplay API service running?)\n",
            sdrplay_api_GetErrorString(err));
        return -1;
    }
    s->api_open = 1;

    (void)sdrplay_api_ApiVersion(&ver);
    log_info("SDRplay API v%.2f\n", ver);
    return 0;
}

/* Enumerate devices and select one; the selector is an index ("0") or a
 * serial-number substring (matched exactly, then as prefix, then suffix). */
static int rsp_select(struct rsp_device *s, const char *selector)
{
    sdrplay_api_ErrT err;

    err = sdrplay_api_LockDeviceApi();
    if (err != sdrplay_api_Success)
    {
        log_error("sdrplay_api_LockDeviceApi failed: %s\n", sdrplay_api_GetErrorString(err));
        return -1;
    }

    err = sdrplay_api_GetDevices(rsp_devices, &rsp_num_devices, SDRPLAY_MAX_DEVICES);
    if (err != sdrplay_api_Success)
    {
        log_error("sdrplay_api_GetDevices failed: %s\n", sdrplay_api_GetErrorString(err));
        (void)sdrplay_api_UnlockDeviceApi();
        return -1;
    }

    if (rsp_num_devices == 0)
    {
        log_error("no RSP devices found\n");
        (void)sdrplay_api_UnlockDeviceApi();
        return -1;
    }

    int idx = -1;
    char *end;
    long num = strtol(selector, &end, 0);
    if (end != selector && *end == '\0' && num >= 0 && (unsigned long)num < rsp_num_devices)
        idx = (int)num;
    for (int pass = 0; pass < 3 && idx < 0; pass++)
        for (unsigned int i = 0; i < rsp_num_devices && idx < 0; i++)
        {
            const char *ser = rsp_devices[i].SerNo;
            size_t len = strlen(selector);
            int hit = 0;

            if (pass == 0)
                hit = strcmp(selector, ser) == 0;
            else if (pass == 1)
                hit = strncmp(selector, ser, len) == 0;
            else
            {
                size_t ser_len = strlen(ser);
                hit = len <= ser_len && strncmp(selector, ser + ser_len - len, len) == 0;
            }
            if (hit)
                idx = (int)i;
        }

    if (idx < 0)
    {
        log_error("no RSP matches \"%s\" among %u device(s)\n", selector, rsp_num_devices);
        for (unsigned int i = 0; i < rsp_num_devices; i++)
            log_error("  %d: SN %s\n", i, rsp_devices[i].SerNo);
        (void)sdrplay_api_UnlockDeviceApi();
        return -1;
    }

    err = sdrplay_api_SelectDevice(&rsp_devices[idx]);
    if (err != sdrplay_api_Success)
    {
        log_error("sdrplay_api_SelectDevice failed: %s\n", sdrplay_api_GetErrorString(err));
        (void)sdrplay_api_UnlockDeviceApi();
        return -1;
    }
    s->selected = 1;
    s->info = rsp_devices[idx];

    (void)sdrplay_api_UnlockDeviceApi();
    return 0;
}

struct device *device_open_rsp(
    const char *selector,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // config-style, one call site (main_rsp)
    double center_hz,
    double capture_hz,
    int gain_rdb,
    int lna_state,
    const char *record_path)
{
    struct rsp_device *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    if (rsp_api_open(s) != 0 || rsp_select(s, selector ? selector : "0") != 0)
        goto fail;

    sdrplay_api_DeviceParamsT *params = NULL;
    sdrplay_api_ErrT err = sdrplay_api_GetDeviceParams(s->info.dev, &params);
    if (err != sdrplay_api_Success || !params || !params->devParams || !params->rxChannelA)
    {
        log_error("sdrplay_api_GetDeviceParams failed\n");
        goto fail;
    }

    uint32_t rate = snap_rate(capture_hz);
    if (fabs((double)rate - capture_hz) > 1.0)
        log_info("RSP rate snapped %.0f -> %u Hz\n", capture_hz, rate);

    sdrplay_api_RxChannelParamsT *ch = params->rxChannelA;

    // ADC always 8 MHz; the service decimates down to the requested rate.
    // This also yields right-justified 16-bit samples instead of the 8-bit
    // dribbles a direct low-rate zero-IF setup would deliver.
    params->devParams->fsFreq.fsHz = (double)RSP_RATE_8M;
    ch->tunerParams.rfFreq.rfHz = center_hz;
    // Analog IF filter must cover the capture but not overflow it
    ch->tunerParams.bwType =
        rate <= RSP_RATE_2M ? sdrplay_api_BW_1_536
                            : (rate <= RSP_RATE_4M ? sdrplay_api_BW_5_000 : sdrplay_api_BW_8_000);
    ch->tunerParams.ifType = sdrplay_api_IF_Zero;
    ch->tunerParams.loMode = sdrplay_api_LO_Auto;

    int agc = gain_rdb < 0 && lna_state < 0;
    if (agc)
    {
        ch->ctrlParams.agc.enable = sdrplay_api_AGC_CTRL_EN;
        ch->ctrlParams.agc.setPoint_dBfs = -30;
        // the API validates the gain fields as its AGC starting point
        gain_rdb = RSP_DEFAULT_GRDB;
        lna_state = RSP_DEFAULT_LNA_STATE;
    }
    else
    {
        ch->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
        if (gain_rdb < 0)
            gain_rdb = RSP_DEFAULT_GRDB;
        if (lna_state < 0)
            lna_state = RSP_DEFAULT_LNA_STATE;
    }
    ch->tunerParams.gain.gRdB = gain_rdb;
    ch->tunerParams.gain.LNAstate = (unsigned char)lna_state;

    unsigned int decim = rate <= RSP_RATE_2M ? 4 : (rate <= RSP_RATE_4M ? 2 : 1);
    if (decim > 1)
    {
        ch->ctrlParams.decimation.enable = 1;
        ch->ctrlParams.decimation.decimationFactor = (unsigned char)decim;
        // wideBandSignal is documented for signals >1.6 MHz wide; this tool
        // demodulates narrowband channels, so use the narrowband decimation
        // path. The wideband setting also makes the service pump ~6000 tiny
        // IPC callbacks/s at 2 MS/s, which can starve a loaded CPU and drip
        // the stream.
        ch->ctrlParams.decimation.wideBandSignal = 0;
    }

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->ring = ring_create(1 << 16);
    s->ring_cap = (size_t)(rate * RSP_RING_SECONDS);
    s->conv = malloc(RSP_CONV_SAMPLES * sizeof(*s->conv));

    if (!s->ring || !s->conv)
    {
        log_error("out of memory buffering the RSP stream\n");
        goto fail;
    }

    if (record_path)
    {
        s->record = fopen(record_path, "wb");
        if (!s->record)
        {
            log_error("cannot open %s for IQ recording\n", record_path);
            goto fail;
        }
    }

    s->base.ops = &rsp_ops;
    s->rate = rate;
    s->startup_drop = (unsigned long long)(rate * RSP_STARTUP_DROP_SECONDS);
    s->open_time = s->report_time = mono_seconds();

    sdrplay_api_CallbackFnsT cb = {0};
    cb.StreamACbFn = rsp_stream_cb;
    cb.StreamBCbFn = rsp_stream_cb;
    cb.EventCbFn = rsp_event_cb;

    err = sdrplay_api_Init(s->info.dev, &cb, s);
    if (err != sdrplay_api_Success)
    {
        log_error("sdrplay_api_Init failed: %s\n", sdrplay_api_GetErrorString(err));
        goto fail;
    }
    s->inited = 1;

    log_info(
        "RSP (SN %s, hwVer %u): %u Hz, center %.6f MHz, %s\n",
        s->info.SerNo,
        s->info.hwVer,
        rate,
        center_hz / 1e6,
        agc ? "AGC" : "manual gain");

    return &s->base;

fail:
    if (s->record)
        (void)fclose(s->record);
    if (s->selected)
        (void)sdrplay_api_ReleaseDevice(&s->info);
    if (s->api_open)
        (void)sdrplay_api_Close();
    if (s->ring || s->conv) // ring/conv allocs imply the mutex/cond exist
        rsp_free(s);
    else
        free(s);
    return NULL;
}

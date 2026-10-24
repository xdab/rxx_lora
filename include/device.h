#ifndef DEVICE_H
#define DEVICE_H

#include <complex.h>
#include <stddef.h>

struct device;

struct device_ops
{
    size_t (*read)(struct device *d, float complex *out, size_t n);

    double (*rate)(const struct device *d);

    void (*stop)(struct device *d);

    void (*close)(struct device *d);
};

struct device
{
    const struct device_ops *ops;
};

/** Open a cf32 file. */
struct device *device_open_file(const char *path, double rate_hz);

/** Open an RTL-SDR dongle via librtlsdr. */
struct device *device_open_rtl(
    int index,
    double center_hz,
    double capture_hz,
    double gain_db,
    int ppm,
    int biastee,
    const char *record_path);

/**
 * Open an SDRplay RSP via the SDRplay API v3 service.
 */
struct device *device_open_rsp(
    const char *selector,
    double center_hz,
    double capture_hz,
    int gain_rdb,
    int lna_state,
    const char *record_path);

static inline double device_rate(const struct device *d) { return d->ops->rate(d); }

static inline size_t device_read(struct device *d, float complex *out, size_t n)
{
    return d->ops->read(d, out, n);
}

static inline void device_stop(struct device *d) { d->ops->stop(d); }

static inline void device_close(struct device *d)
{
    if (d)
        d->ops->close(d);
}

#endif // DEVICE_H

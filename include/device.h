#ifndef DEVICE_H
#define DEVICE_H

#include <complex.h>
#include <stddef.h>

struct device;

/**
 * The input back end's vtable: each device front end (file, RTL-SDR,
 * SDRplay) provides one. All calls go through the static inline wrappers
 * below.
 */
struct device_ops
{
    /** Pull up to n samples into out; returns the delivered count. */
    size_t (*read)(struct device *d, float complex *out, size_t n);

    /** Nominal sample rate the device was opened at, Hz. */
    double (*rate)(const struct device *d);

    /** Stop streaming; the device stays open. */
    void (*stop)(struct device *d);

    /** Stop streaming and release the device. */
    void (*close)(struct device *d);
};

/**
 * One open input device: an ops vtable plus the back end's own state.
 */
struct device
{
    const struct device_ops *ops;
};

/**
 * @brief Open a cf32 file as a device.
 *
 * @param[in] path file to read
 * @param[in] rate_hz the recording's sample rate, Hz
 * @return the device, or NULL on failure
 */
struct device *device_open_file(const char *path, double rate_hz);

/**
 * @brief Open an RTL-SDR dongle via librtlsdr.
 *
 * @param[in] index dongle index
 * @param[in] center_hz RF center frequency, Hz
 * @param[in] capture_hz capture bandwidth, Hz
 * @param[in] gain_db manual gain, dB
 * @param[in] ppm frequency correction, ppm
 * @param[in] biastee enable the bias tee when nonzero
 * @param[in] record_path optional raw cf32 tee of the stream, NULL when not
 *            recording
 * @return the device, or NULL on failure
 */
struct device *device_open_rtl(
    int index,
    double center_hz,
    double capture_hz,
    double gain_db,
    int ppm,
    int biastee,
    const char *record_path);

/**
 * @brief Open an SDRplay RSP via the SDRplay API v3 service.
 *
 * @param[in] selector device index ("0") or serial-number substring
 * @param[in] center_hz RF center frequency, Hz
 * @param[in] capture_hz capture bandwidth, Hz
 * @param[in] gain_rdb IF gain reduction, dB (negative enables the API AGC)
 * @param[in] lna_state LNA attenuation state (negative enables the API AGC)
 * @param[in] record_path optional raw cf32 tee of the stream, NULL when not
 *            recording
 * @return the device, or NULL on failure
 */
struct device *device_open_rsp(
    const char *selector,
    double center_hz,
    double capture_hz,
    int gain_rdb,
    int lna_state,
    const char *record_path);

/**
 * @brief Nominal sample rate of the device, Hz.
 */
static inline double device_rate(const struct device *d) { return d->ops->rate(d); }

/**
 * @brief Pull up to n samples into out; returns the delivered count.
 */
static inline size_t device_read(struct device *d, float complex *out, size_t n)
{
    return d->ops->read(d, out, n);
}

/**
 * @brief Stop streaming; the device stays open.
 */
static inline void device_stop(struct device *d) { d->ops->stop(d); }

/**
 * @brief Stop streaming and release the device; NULL is a no-op.
 */
static inline void device_close(struct device *d)
{
    if (d)
        d->ops->close(d);
}

#endif // DEVICE_H

#include "device.h"

#include <complex.h>
#include <stdio.h>
#include <stdlib.h>

struct file_device
{
    struct device base;
    FILE *f;
    double rate;
};

static size_t file_read(struct device *d, float complex *out, size_t n)
{
    struct file_device *s = (struct file_device *)d;
    return fread(out, sizeof(*out), n, s->f);
}

static double file_rate(const struct device *d) { return ((const struct file_device *)d)->rate; }

static void file_stop(struct device *d)
{
    (void)d; // nothing to close/flush/interrupt
}

static void file_close(struct device *d)
{
    struct file_device *s = (struct file_device *)d;

    if (s->f)
        (void)fclose(s->f);
    free(s);
}

static const struct device_ops file_ops = {file_read, file_rate, file_stop, file_close};

struct device *device_open_file(const char *path, double rate_hz)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    struct file_device *s = calloc(1, sizeof(*s));
    if (!s)
    {
        (void)fclose(f);
        return NULL;
    }

    s->base.ops = &file_ops;
    s->f = f;
    s->rate = rate_hz;

    return &s->base;
}

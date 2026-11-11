/** @file main_live.h
 * @brief Shared plumbing of the two live front ends (main_rtl.c, main_rsp.c):
 *        the signal handler, the -x timer, the shared option set with its
 *        validation, and the run/teardown tail. Device-specific options and
 *        usage text stay in each main.
 */

#ifndef MAIN_LIVE_H
#define MAIN_LIVE_H

#include <getopt.h> // NOLINT(misc-include-cleaner)
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "device.h"
#include "main.h"
#include "receiver.h"
#include "source.h"

static struct device *g_dev;            // for the signal handler
static volatile sig_atomic_t g_timeout; // SIGALRM from -x fired

// long-option values; each main lists its own long_opts table over them
enum
{
    OPT_CONJ = 256,
    OPT_DC_BLOCK,
    OPT_CENTER_FREQ,
    OPT_BW,
    OPT_INPUT_RATE,
    OPT_PPM, // rtl only
    OPT_CHASE,
    OPT_REPORT_BAD,
};

static void live_on_signal(int sig)
{
    if (sig == SIGALRM)
        g_timeout = 1;
    if (g_dev)
        device_stop(g_dev);
}

// the option fields the shared blocks below handle
struct live_opts
{
    int sf, os, preamble_avg, detect_avg;
    long sync_word_filter;
    struct chase_config chase;
    int conjugate_iq, dc_block, report_bad;
    double center_freq, bandwidth, capture_rate;
    const char *record_path;
    double timeout_s;
};

// the shared option cases of the getopt loop; returns 1 when handled
static inline int live_opt(int opt, struct live_opts *o)
{
    switch (opt)
    {
    case 's':
        o->sf = (int)strtol(optarg, NULL, 10);
        return 1;
    case 'o':
        o->os = (int)strtol(optarg, NULL, 10);
        return 1;
    case 'p':
        o->preamble_avg = (int)strtol(optarg, NULL, 10);
        return 1;
    case 'a':
        o->detect_avg = (int)strtol(optarg, NULL, 10);
        return 1;
    case 'w':
        o->sync_word_filter = (long)strtoul(optarg, NULL, 0);
        return 1;
    case 'x':
        o->timeout_s = parse_duration(optarg);
        return 1;
    case 'R':
        o->record_path = optarg;
        return 1;
    case OPT_CHASE:
        o->chase.enabled = 1;
        return 1;
    case OPT_REPORT_BAD:
        o->report_bad = 1;
        return 1;
    case OPT_CONJ:
        o->conjugate_iq = 1;
        return 1;
    case OPT_DC_BLOCK:
        o->dc_block = 1;
        return 1;
    case OPT_CENTER_FREQ:
        o->center_freq = strtod(optarg, NULL);
        return 1;
    case OPT_BW:
        o->bandwidth = strtod(optarg, NULL);
        return 1;
    case OPT_INPUT_RATE:
        o->capture_rate = strtod(optarg, NULL);
        return 1;
    default:
        return 0;
    }
}

// Validate the shared options; prints the problem and returns nonzero on the
// first one. @p app names the device in the center-freq message.
static int live_opts_check(const struct live_opts *o, const char *app)
{
    if (o->preamble_avg >= 6 && o->sf >= 5 && o->sf <= 12 && o->os >= 2 && o->os <= 255 &&
        o->detect_avg >= 1 && o->detect_avg <= 255 && o->sync_word_filter >= 0 &&
        o->sync_word_filter <= 0xff && o->center_freq > 0.0 && o->bandwidth > 0.0 &&
        o->capture_rate >= 0.0 && o->timeout_s >= 0.0)
        return 0;

    if (o->center_freq <= 0.0)
        (void)fprintf(stderr, "%s needs --center-freq\n", app);
    if (o->timeout_s < 0.0)
        (void)fprintf(stderr, "bad -x duration (use a number, optionally s/m/h)\n");
    return 1;
}

// capture_rate unset: the working rate bw*os
static inline void live_opts_default(struct live_opts *o)
{
    if (o->capture_rate <= 0.0)
        o->capture_rate = o->bandwidth * (double)o->os;
}

static void live_signal_setup(void)
{
    (void)signal(SIGINT, live_on_signal);
    (void)signal(SIGTERM, live_on_signal);
}

// route the -x budget through the same clean stop as Ctrl-C (device_stop)
static void live_timer_arm(double timeout_s)
{
    if (timeout_s <= 0.0)
        return;
    (void)signal(SIGALRM, live_on_signal);

    long sec = (long)timeout_s;
    long usec = (long)((timeout_s - (double)sec) * 1e6);
    if (sec == 0 && usec == 0)
        usec = 1; // a sub-microsecond budget still fires

    struct itimerval it = {0};
    it.it_value.tv_sec = sec;
    it.it_value.tv_usec = usec;
    setitimer(ITIMER_REAL, &it, NULL);
}

static void live_timer_disarm(double timeout_s)
{
    if (timeout_s > 0.0)
    {
        struct itimerval dis = {0};
        setitimer(ITIMER_REAL, &dis, NULL); // no late SIGALRM past teardown
    }
    if (g_timeout)
        log_info("duration reached (%.1f s), stopping\n", timeout_s);
}

static void live_record_note(const struct live_opts *o)
{
    if (!o->record_path)
        return;
    log_info("recording raw IQ to %s @ %.0f Hz\n", o->record_path, device_rate(g_dev));
    log_info(
        "replay with: bin/file_lora -s %d -w 0x%02lx --center-freq %.0f --bw %.0f "
        "--input-rate %.0f%s%s %s\n",
        o->sf,
        o->sync_word_filter,
        o->center_freq,
        o->bandwidth,
        device_rate(g_dev),
        o->conjugate_iq ? " --conj" : "",
        o->dc_block ? " --dc-block" : "",
        o->record_path);
}

// the receiver from the shared options; NULL on failure
static struct receiver *live_receiver_create(const struct live_opts *o)
{
    struct receiver_config cfg = {
        .sf = o->sf,
        .os = o->os,
        .preamble_avg = o->preamble_avg,
        .sync_word = o->sync_word_filter,
        .detect_avg = o->detect_avg,
        .center_freq = o->center_freq,
        .bandwidth = o->bandwidth,
        .chase = o->chase,
        .report_crc_bad = o->report_bad,
    };
    return receiver_create(&cfg);
}

static void live_tail(const struct live_opts *o)
{
    log_info(
        "%s: SF=%d os=%d pre=%d avg=%d  center %.6f MHz bw %.0f kHz\n",
        APP_NAME,
        o->sf,
        o->os,
        o->preamble_avg,
        o->detect_avg,
        o->center_freq / 1e6,
        o->bandwidth / 1e3);

    if (o->conjugate_iq)
        log_info("spectrum conjugated (--conj)\n");

    if (o->dc_block)
        log_info("DC blocker on (corner %.0f Hz)\n", o->bandwidth / 1000.0);

    if (o->sync_word_filter)
        log_info("filtering by sync word (net-id) 0x%02lx\n", o->sync_word_filter);
    else
        log_info("no sync word filter (any net-id accepted)\n");

    if (o->chase.enabled)
        log_info("chase-II retry on\n");

    log_info("listening... (Ctrl-C to stop)\n");
}

// run the receiver and tear everything down; the process exit code
static int live_run(struct receiver *rx, struct source *src, const struct live_opts *o)
{
    int n_decoded = receiver_run(rx, src);
    int failed = source_failed(src);

    live_timer_disarm(o->timeout_s);

    receiver_destroy(rx);
    source_destroy(src);
    device_close(g_dev);
    g_dev = NULL;

    log_info("stopped after %d decoded frame(s)\n", n_decoded);
    return failed ? 1 : 0;
}

#endif // MAIN_LIVE_H

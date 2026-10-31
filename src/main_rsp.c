#include <getopt.h> // NOLINT(misc-include-cleaner)
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "device.h"
#include "logging.h"
#include "main.h"
#include "receiver.h"
#include "source.h"

#ifndef APP_NAME
#define APP_NAME "rsp_lora"
#endif

static struct device *g_dev;            // for the signal handler
static volatile sig_atomic_t g_timeout; // SIGALRM from -x fired

static void on_signal(int sig)
{
    if (sig == SIGALRM)
        g_timeout = 1;
    if (g_dev)
        device_stop(g_dev);
}

static void usage(void)
{
    (void)fprintf(
        stderr,
        APP_NAME ", a live LoRa (CSS) receiver for SDRplay RSP receivers\n"
                 "\n"
                 "Use:\t" APP_NAME " --center-freq HZ [-options]\n"
                 "\t[-d SEL] RSP device: index (default: 0) or serial-number\n"
                 "\t         substring\n"
                 "\t[-s sf]  spreading factor, 5..12 (default: 9)\n"
                 "\t[-o os]  oversampling factor, 1..255 (default: 4)\n"
                 "\t[-p pre] preamble detection windows to average (default: 8)\n"
                 "\t[-a avg] detector correlation budget K, 1..255 (default: 4)\n"
                 "\t[-w id]  hex sync word to filter by: 0x12 gr-lora,\n"
                 "\t         0x2b Meshtastic; omit to accept any\n"
                 "\t[-c]     chase-II retry: on a payload CRC fail, re-decode the\n"
                 "\t         least-reliable codewords on their runner-ups\n"
                 "\t[--chase-k N] [--chase-marg X] [--chase-tau X]\n"
                 "\t[--chase-singles N] [--chase-pairs N] retry tuning with -c\n"
                 "\t         (defaults: 4, 0.2, 1.0, unlimited, unlimited)\n"
                 "\t[-g db]  IF gain reduction, 0..59 dB (default: automatic);\n"
                 "\t         giving -g or -L selects manual gain\n"
                 "\t[-L n]   LNA state (RSP1: 0..2, 0 = max gain)\n"
                 "\t[-R FILE] record raw device IQ to FILE as cf32 (replays via\n"
                 "\t         file_lora --input-rate)\n"
                 "\t[-x TIME] stop cleanly after TIME seconds; suffixes s/m/h\n"
                 "\t[-v [N]] verbosity: 1 (or bare -v) adds DEBUG, 2 adds TRACE\n"
                 "\t[-q [N]] quietness: 1 drops INFO, 2 drops WARN, 3 silences stderr\n"
                 "\t[--conj] conjugate the samples first (mirrored-spectrum front ends)\n"
                 "\t[--dc-block] high-pass the samples (strip the zero-IF tuner's DC\n"
                 "\t         offset / 1-f junk; corner at bw/1000)\n"
                 "\t[--center-freq HZ] RF center to tune to (required); also enables\n"
                 "\t         the CFO->SFO inference on the live clock\n"
                 "\t[--bw HZ] chirp bandwidth (default: 125000); sets the working\n"
                 "\t         rate bw*os and the default capture rate\n"
                 "\t[--input-rate HZ] RSP capture rate (default: bw*os); snapped to\n"
                 "\t         the API's 2/4/8 MS/s\n"
                 "\n"
                 "Example:\t" APP_NAME " --center-freq 869.525e6 -s 9 -w 0x2b\n"
                 "\n");
}

// seconds from a decimal with an optional s/m/h suffix; -1 on a malformed
// value (a bare number is already seconds). h=3600, m=60, s=1.
static double parse_duration(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || v < 0.0)
        return -1.0;
    if (*end == '\0' || ((*end == 's' || *end == 'S') && end[1] == '\0'))
        return v;
    if ((*end == 'm' || *end == 'M') && end[1] == '\0')
        return v * 60.0;
    if ((*end == 'h' || *end == 'H') && end[1] == '\0')
        return v * 3600.0;
    return -1.0;
}

int main(int argc, char **argv)
{
    // --- defaults ---
    const char *selector = "0";
    int sf = 9;
    int os = 4;
    int preamble_avg = 8;
    int detect_avg = 4;
    long sync_word_filter = 0;
    struct chase_config chase = CHASE_CONFIG_DEFAULTS;
    int conjugate_iq = 0;
    int dc_block = 0;
    double center_freq = 0.0; // required: an RSP must be tuned
    double bandwidth = 250e3;
    int gain_rdb = -1;         // < 0 with lna: API AGC
    int lna_state = -1;        // < 0 with gain: API AGC
    double capture_rate = 0.0; // 0 = bw*os
    const char *record_path = NULL;
    double timeout_s = 0.0; // -x: 0 = run until stopped

    enum
    {
        OPT_CONJ = 256,
        OPT_DC_BLOCK,
        OPT_CENTER_FREQ,
        OPT_BW,
        OPT_INPUT_RATE,
        OPT_CHASE_K,
        OPT_CHASE_MARG,
        OPT_CHASE_TAU,
        OPT_CHASE_SINGLES,
        OPT_CHASE_PAIRS
    };
    static const struct option long_opts[] = {
        {"conj", no_argument, NULL, OPT_CONJ},
        {"dc-block", no_argument, NULL, OPT_DC_BLOCK},
        {"center-freq", required_argument, NULL, OPT_CENTER_FREQ},
        {"bw", required_argument, NULL, OPT_BW},
        {"input-rate", required_argument, NULL, OPT_INPUT_RATE},
        {"chase", no_argument, NULL, 'c'},
        {"chase-k", required_argument, NULL, OPT_CHASE_K},
        {"chase-marg", required_argument, NULL, OPT_CHASE_MARG},
        {"chase-tau", required_argument, NULL, OPT_CHASE_TAU},
        {"chase-singles", required_argument, NULL, OPT_CHASE_SINGLES},
        {"chase-pairs", required_argument, NULL, OPT_CHASE_PAIRS},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:s:o:p:a:w:g:L:v::q::R:x:c", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
        case 'd':
            selector = optarg;
            break;
        case 's':
            sf = (int)strtol(optarg, NULL, 10);
            break;
        case 'o':
            os = (int)strtol(optarg, NULL, 10);
            break;
        case 'p':
            preamble_avg = (int)strtol(optarg, NULL, 10);
            break;
        case 'a':
            detect_avg = (int)strtol(optarg, NULL, 10);
            break;
        case 'w':
            sync_word_filter = (long)strtoul(optarg, NULL, 0);
            break;
        case 'c':
            chase.enabled = 1;
            break;
        case OPT_CHASE_K:
            chase.k = (int)strtol(optarg, NULL, 10);
            break;
        case OPT_CHASE_MARG:
            chase.marg = strtof(optarg, NULL);
            break;
        case OPT_CHASE_TAU:
            chase.tau = strtof(optarg, NULL);
            break;
        case OPT_CHASE_SINGLES:
            chase.singles = (int)strtol(optarg, NULL, 10);
            break;
        case OPT_CHASE_PAIRS:
            chase.pairs = (int)strtol(optarg, NULL, 10);
            break;
        case 'g':
            gain_rdb = (int)strtol(optarg, NULL, 10);
            break;
        case 'L':
            lna_state = (int)strtol(optarg, NULL, 10);
            break;
        case 'R':
            record_path = optarg;
            break;
        case 'x':
            timeout_s = parse_duration(optarg);
            break;
        case OPT_CONJ:
            conjugate_iq = 1;
            break;
        case OPT_DC_BLOCK:
            dc_block = 1;
            break;
        case OPT_CENTER_FREQ:
            center_freq = strtod(optarg, NULL);
            break;
        case OPT_BW:
            bandwidth = strtod(optarg, NULL);
            break;
        case OPT_INPUT_RATE:
            capture_rate = strtod(optarg, NULL);
            break;
        case 'v':
        case 'q':
            // -v/-v1/-v 1 and -q/-q1/-q 1
            log_verbosity_opt(opt, argc, argv);
            break;
        default:
            usage();
            return opt == 'h' ? 0 : 1;
        }
    }

    if (optind < argc)
    {
        (void)fprintf(stderr, "%s takes no positional arguments\n", APP_NAME);
        usage();
        return 1;
    }

    if (preamble_avg < 6 || sf < 5 || sf > 12 || os < 1 || os > 255 || detect_avg < 1 ||
        detect_avg > 255 || sync_word_filter < 0 || sync_word_filter > 0xff || center_freq <= 0.0 ||
        bandwidth <= 0.0 || capture_rate < 0.0 || timeout_s < 0.0 || gain_rdb < -1 ||
        gain_rdb > 59 || lna_state < -1 || lna_state > 9 || chase.k < 1 || chase.k > 8 ||
        chase.singles < -1 || chase.pairs < -1)
    {
        if (center_freq <= 0.0)
            (void)fprintf(stderr, "RSP needs --center-freq\n");

        if (timeout_s < 0.0)
            (void)fprintf(stderr, "bad -x duration (use a number, optionally s/m/h)\n");

        if (gain_rdb < -1 || gain_rdb > 59)
            (void)fprintf(stderr, "bad -g (IF gain reduction must be 0-59 dB)\n");

        if (lna_state < -1 || lna_state > 9)
            (void)fprintf(stderr, "bad -L (LNA state out of range for any RSP)\n");

        usage();
        return 1;
    }

    if (capture_rate <= 0.0)
        capture_rate = bandwidth * (double)os;

    g_dev = device_open_rsp(selector, center_freq, capture_rate, gain_rdb, lna_state, record_path);
    if (!g_dev)
        return 1;

    (void)signal(SIGINT, on_signal);
    (void)signal(SIGTERM, on_signal);

    if (timeout_s > 0.0)
    {
        // route the budget through the same clean stop as Ctrl-C (device_stop)
        (void)signal(SIGALRM, on_signal);

        long sec = (long)timeout_s;
        long usec = (long)((timeout_s - (double)sec) * 1e6);
        if (sec == 0 && usec == 0)
            usec = 1; // a sub-microsecond budget still fires

        struct itimerval it = {0};
        it.it_value.tv_sec = sec;
        it.it_value.tv_usec = usec;
        setitimer(ITIMER_REAL, &it, NULL);
    }

    if (record_path)
    {
        log_info("recording raw IQ to %s @ %.0f Hz\n", record_path, device_rate(g_dev));
        log_info(
            "replay with: bin/file_lora -s %d -w 0x%02lx --center-freq %.0f --bw %.0f "
            "--input-rate %.0f%s%s %s\n",
            sf,
            sync_word_filter,
            center_freq,
            bandwidth,
            device_rate(g_dev),
            conjugate_iq ? " --conj" : "",
            dc_block ? " --dc-block" : "",
            record_path);
    }

    struct source *src = source_create(g_dev, sf, os, bandwidth, conjugate_iq, dc_block);
    if (!src)
    {
        device_close(g_dev);
        return 1;
    }

    struct receiver *rx;
    {
        struct receiver_config cfg = {
            .sf = sf,
            .os = os,
            .preamble_avg = preamble_avg,
            .sync_word = sync_word_filter,
            .detect_avg = detect_avg,
            .center_freq = center_freq,
            .bandwidth = bandwidth,
            .chase = chase,
        };
        rx = receiver_create(&cfg);
    }
    if (!rx)
    {
        source_destroy(src);
        device_close(g_dev);
        return 1;
    }

    log_info(
        "%s: SF=%d os=%d pre=%d avg=%d  center %.6f MHz bw %.0f kHz\n",
        APP_NAME,
        sf,
        os,
        preamble_avg,
        detect_avg,
        center_freq / 1e6,
        bandwidth / 1e3);

    if (conjugate_iq)
        log_info("spectrum conjugated (--conj)\n");

    if (dc_block)
        log_info("DC blocker on (corner %.0f Hz)\n", bandwidth / 1000.0);

    if (sync_word_filter)
        log_info("filtering by sync word (net-id) 0x%02lx\n", sync_word_filter);
    else
        log_info("no sync word filter (any net-id accepted)\n");

    if (chase.enabled)
        log_info(
            "chase-II retry on: k=%d marg=%g tau=%g singles=%d pairs=%d\n",
            chase.k,
            (double)chase.marg,
            (double)chase.tau,
            chase.singles,
            chase.pairs);

    log_info("listening... (Ctrl-C to stop)\n");

    int n_decoded = receiver_run(rx, src);
    int failed = source_failed(src);

    if (timeout_s > 0.0)
    {
        struct itimerval dis = {0};
        setitimer(ITIMER_REAL, &dis, NULL); // no late SIGALRM past teardown
    }
    if (g_timeout)
        log_info("duration reached (%.1f s), stopping\n", timeout_s);

    receiver_destroy(rx);
    source_destroy(src);
    device_close(g_dev);
    g_dev = NULL;

    log_info("stopped after %d decoded frame(s)\n", n_decoded);
    return failed ? 1 : 0;
}

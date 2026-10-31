#include <getopt.h> // NOLINT(misc-include-cleaner)
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "device.h"
#include "logging.h"
#include "main.h"
#include "receiver.h"
#include "source.h"

#ifndef APP_NAME
#define APP_NAME "file_lora"
#endif

static void usage(void)
{
    (void)fprintf(
        stderr,
        APP_NAME ", a LoRa (CSS) decoder for cf32 IQ recordings\n"
                 "\n"
                 "Use:\t" APP_NAME " [-options] FILE.cf32\n"
                 "\tFILE    interleaved float32 I/Q capture (baseband)\n"
                 "\t[-s sf]  spreading factor, 5..12 (default: 9)\n"
                 "\t[-o os]  oversampling factor, 1..255 (default: 2)\n"
                 "\t[-p pre] preamble detection windows to average (default: 8)\n"
                 "\t[-a avg] detector correlation budget K, 1..255 (default: 4)\n"
                 "\t[-w id]  hex sync word to filter by: 0x12 gr-lora,\n"
                 "\t         0x2b Meshtastic; omit to accept any\n"
                 "\t[-c]     chase-II retry: on a payload CRC fail, re-decode the\n"
                 "\t         least-reliable codewords on their runner-ups\n"
                 "\t[--chase-k N] [--chase-marg X] [--chase-tau X]\n"
                 "\t[--chase-singles N] [--chase-pairs N] retry tuning with -c\n"
                 "\t         (defaults: 4, 0.2, 1.0, unlimited, unlimited)\n"
                 "\t[-v [N]] verbosity: 1 (or bare -v) adds DEBUG, 2 adds TRACE\n"
                 "\t[-q [N]] quietness: 1 drops INFO, 2 drops WARN, 3 silences stderr\n"
                 "\t[--conj] conjugate the samples first (mirrored-spectrum captures)\n"
                 "\t[--dc-block] high-pass the samples (strip a zero-IF tuner's DC\n"
                 "\t         offset; corner at bw/1000)\n"
                 "\t[--center-freq HZ] RF center of a real recording (default: 0 =\n"
                 "\t         off); enables the CFO->SFO inference\n"
                 "\t[--bw HZ] chirp bandwidth of the recording (default: 125000);\n"
                 "\t         scales the bins-to-Hz prints and the SFO inference\n"
                 "\t[--input-rate HZ] sample rate of FILE; the file is resampled\n"
                 "\t         to bw*os before decoding; omit when the\n"
                 "\t         capture is already at bw*os\n"
                 "\n"
                 "Example:\t" APP_NAME " -w 0x2b --center-freq 868.1e6 --bw 125e3 frame.cf32\n"
                 "\n");
}

int main(int argc, char **argv)
{
    // --- defaults ---
    int sf = 9;
    int os = 2;
    int preamble_avg = 8;
    int detect_avg = 4;
    long sync_word_filter = 0;
    struct chase_config chase = CHASE_CONFIG_DEFAULTS;
    int conjugate_iq = 0;
    int dc_block = 0;
    double center_freq = 0.0;
    double bandwidth = 250e3;
    double input_rate = 0.0;

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
    while ((opt = getopt_long(argc, argv, "s:o:p:a:w:v::q::c", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
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
            input_rate = strtod(optarg, NULL);
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

    if (optind >= argc || preamble_avg < 6 || sf < 5 || sf > 12 || os < 1 || os > 255 ||
        detect_avg < 1 || detect_avg > 255 || sync_word_filter < 0 || sync_word_filter > 0xff ||
        center_freq < 0.0 || bandwidth <= 0.0 || input_rate < 0.0 || chase.k < 1 || chase.k > 8 ||
        chase.singles < -1 || chase.pairs < -1)
    {
        usage();
        return 1;
    }
    const char *file_path = argv[optind];

    struct device *dev = device_open_file(file_path, input_rate);
    if (!dev)
    {
        log_error("cannot open %s\n", file_path);
        return 1;
    }

    struct source *src = source_create(dev, sf, os, bandwidth, conjugate_iq, dc_block);
    if (!src)
    {
        device_close(dev);
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
        device_close(dev);
        return 1;
    }

    // stream length is only known at end of stream in pull mode; the file
    // size gives the count of complex samples the file holds
    struct stat st;
    long file_bytes = (stat(file_path, &st) == 0) ? (long)st.st_size : 0;
    long est = (long)(file_bytes / (2 * sizeof(float)));
    log_info(
        "%s: %s  SF=%d os=%d pre=%d avg=%d  ~%ld input samples\n",
        APP_NAME,
        file_path,
        sf,
        os,
        preamble_avg,
        detect_avg,
        est);

    if (conjugate_iq)
        log_info("spectrum conjugated (--conj)\n");

    if (dc_block)
        log_info("DC blocker on (corner %.0f Hz)\n", bandwidth / 1000.0);

    if (center_freq > 0.0)
        log_info(
            "SFO compensation on: cfo->sfo inference at %.6f MHz, bw %.0f kHz\n",
            center_freq / 1e6,
            bandwidth / 1e3);

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

    int n_decoded = receiver_run(rx, src);
    int failed = source_failed(src);

    receiver_destroy(rx);
    source_destroy(src);
    device_close(dev);

    if (failed)
        return 1;
    return (n_decoded > 0) ? 0 : 1;
}

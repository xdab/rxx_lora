#include <stdio.h>
#include <stdlib.h>

#ifndef APP_NAME
#define APP_NAME "rtl_lora"
#endif

#include "main_live.h"

#include "device.h"
#include "main.h"
#include "receiver.h"
#include "source.h"

static void usage(void)
{
    (void)fprintf(
        stderr,
        APP_NAME ", a live LoRa (CSS) receiver for RTL-SDR dongles\n"
                 "\n"
                 "Use:\t" APP_NAME " --center-freq HZ [-options]\n"
                 "\t[-d N]   RTL-SDR device index (default: 0)\n"
                 "\t[-s sf]  spreading factor, 5..12 (default: 9)\n"
                 "\t[-o os]  oversampling factor, 2..255 (default: 4)\n"
                 "\t[-p pre] preamble detection windows to average (default: 8)\n"
                 "\t[-a avg] detector correlation budget K, 1..255 (default: 4)\n"
                 "\t[-w id]  hex sync word to filter by: 0x12 gr-lora,\n"
                 "\t         0x2b Meshtastic; omit to accept any\n"
                 "\t[--chase] chase-II retry: on a payload CRC fail, re-decode the\n"
                 "\t         least-reliable codewords on their runner-ups\n"
                 "\t[--report-bad] also write CRC-invalid frames to stdout\n"
                 "\t         (default: only CRC-valid and CRC-absent frames)\n"
                 "\t[-g db]  tuner gain in dB (default: automatic)\n"
                 "\t[--ppm N] frequency correction in ppm (default: 0)\n"
                 "\t[-T]     enable bias-T on GPIO pin 0\n"
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
                 "\t[--bw HZ] chirp bandwidth (default: 250000); sets the working\n"
                 "\t         rate bw*os and the default capture rate\n"
                 "\t[--input-rate HZ] dongle capture rate (default: bw*os); snapped\n"
                 "\t         to a rate the RTL2832U supports\n"
                 "\n"
                 "Example:\t" APP_NAME " --center-freq 869.525e6 -s 9 -w 0x2b\n"
                 "\n");
}

int main(int argc, char **argv)
{
    // --- defaults ---
    int dev_index = 0;
    double gain_db = -1.0; // < 0 = automatic tuner gain
    int ppm = 0;
    int biastee = 0;
    struct live_opts o = {
        .sf = 9,
        .os = 4,
        .preamble_avg = 8,
        .detect_avg = 4,
        .chase = CHASE_CONFIG_DEFAULTS,
        .bandwidth = 250e3,
    };

    static const struct option long_opts[] = {
        {"conj", no_argument, NULL, OPT_CONJ},
        {"dc-block", no_argument, NULL, OPT_DC_BLOCK},
        {"center-freq", required_argument, NULL, OPT_CENTER_FREQ},
        {"bw", required_argument, NULL, OPT_BW},
        {"input-rate", required_argument, NULL, OPT_INPUT_RATE},
        {"ppm", required_argument, NULL, OPT_PPM},
        {"chase", no_argument, NULL, OPT_CHASE},
        {"report-bad", no_argument, NULL, OPT_REPORT_BAD},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:s:o:p:a:w:g:Tv::q::R:x:", long_opts, NULL)) != -1)
    {
        if (live_opt(opt, &o))
            continue;
        switch (opt)
        {
        case 'd':
            dev_index = (int)strtol(optarg, NULL, 10);
            break;
        case 'g':
            gain_db = strtod(optarg, NULL);
            break;
        case 'T':
            biastee = 1;
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

    if (live_opts_check(&o, "RTL"))
    {
        usage();
        return 1;
    }
    live_opts_default(&o);

    g_dev = device_open_rtl(
        dev_index, o.center_freq, o.capture_rate, gain_db, ppm, biastee, o.record_path);
    if (!g_dev)
        return 1;

    live_signal_setup();
    live_timer_arm(o.timeout_s);
    live_record_note(&o);

    struct source *src = source_create(g_dev, o.sf, o.os, o.bandwidth, o.conjugate_iq, o.dc_block);
    if (!src)
    {
        device_close(g_dev);
        return 1;
    }

    struct receiver *rx = live_receiver_create(&o);
    if (!rx)
    {
        source_destroy(src);
        device_close(g_dev);
        return 1;
    }

    live_tail(&o);

    return live_run(rx, src, &o);
}

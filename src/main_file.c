#include <complex.h>
#include <getopt.h>
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
    fprintf(
        stderr,
        APP_NAME ", a LoRa (CSS) decoder for cf32 IQ recordings\n"
                 "\n"
                 "Use:\t" APP_NAME " [-options] FILE.cf32\n"
                 "\tFILE    interleaved float32 I/Q capture (baseband)\n"
                 "\t[-s sf]  spreading factor, 5..12 (default: 7)\n"
                 "\t[-o os]  oversampling factor, 1..255 (default: 2)\n"
                 "\t[-p pre] preamble detection windows to average (default: 8)\n"
                 "\t[-a avg] detector correlation budget K, 1..255 (default: 4)\n"
                 "\t[-w id]  hex sync word to filter by: 0x12 gr-lora,\n"
                 "\t         0x2b Meshtastic; omit to accept any\n"
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
        OPT_INPUT_RATE
    };
    static const struct option long_opts[] = {
        {"conj", no_argument, NULL, OPT_CONJ},
        {"dc-block", no_argument, NULL, OPT_DC_BLOCK},
        {"center-freq", required_argument, NULL, OPT_CENTER_FREQ},
        {"bw", required_argument, NULL, OPT_BW},
        {"input-rate", required_argument, NULL, OPT_INPUT_RATE},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:o:p:a:w:v::q::", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
        case 's':
            sf = atoi(optarg);
            break;
        case 'o':
            os = atoi(optarg);
            break;
        case 'p':
            preamble_avg = atoi(optarg);
            break;
        case 'a':
            detect_avg = atoi(optarg);
            break;
        case 'w':
            sync_word_filter = (long)strtoul(optarg, NULL, 0);
            break;
        case OPT_CONJ:
            conjugate_iq = 1;
            break;
        case OPT_DC_BLOCK:
            dc_block = 1;
            break;
        case OPT_CENTER_FREQ:
            center_freq = atof(optarg);
            break;
        case OPT_BW:
            bandwidth = atof(optarg);
            break;
        case OPT_INPUT_RATE:
            input_rate = atof(optarg);
            break;
        case 'v':
        case 'q':
        {
            // -v/-v1/-v 1 and -q/-q1/-q 1
            int n = 1;
            if (optarg)
                n = atoi(optarg);
            else if (optind < argc && digits_only(argv[optind]))
                n = atoi(argv[optind++]);
            if (n < 1)
                n = 1;
            log_set_level(opt == 'v' ? LOG_INFO - (n > 2 ? 2 : n) : LOG_INFO + (n > 3 ? 3 : n));
            break;
        }
        default:
            usage();
            return opt == 'h' ? 0 : 1;
        }
    }

    if (optind >= argc || preamble_avg < 6 || sf < 5 || sf > 12 || os < 1 || os > 255 ||
        detect_avg < 1 || detect_avg > 255 || sync_word_filter < 0 || sync_word_filter > 0xff ||
        center_freq < 0.0 || bandwidth <= 0.0 || input_rate < 0.0)
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

    struct receiver *rx =
        receiver_create(sf, os, preamble_avg, sync_word_filter, detect_avg, center_freq, bandwidth);
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
    long est = file_bytes / (2 * sizeof(float));
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

    int n_decoded = receiver_run(rx, src);
    int failed = source_failed(src);

    receiver_destroy(rx);
    source_destroy(src);
    device_close(dev);

    if (failed)
        return 1;
    return (n_decoded > 0) ? 0 : 1;
}

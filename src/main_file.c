#include <stdio.h>
#include <sys/stat.h>

#ifndef APP_NAME
#define APP_NAME "file_lora"
#endif

#include "main_live.h"

#include "device.h"
#include "logging.h"
#include "main.h"
#include "receiver.h"
#include "source.h"

static void usage(void)
{
    (void)fprintf(
        stderr,
        APP_NAME ", a LoRa (CSS) decoder for cf32 IQ recordings\n"
                 "\n"
                 "Use:\t" APP_NAME " [-options] FILE.cf32\n"
                 "\tFILE    interleaved float32 I/Q capture (baseband)\n"
                 "\t[-s sf]  spreading factor, 5..12 (default: 9)\n"
                 "\t[-o os]  oversampling factor, 2..255 (default: 2)\n"
                 "\t[-p pre] preamble detection windows to average (default: 8)\n"
                 "\t[-a avg] detector correlation budget K, 1..255 (default: 4)\n"
                 "\t[-w id]  hex sync word to filter by: 0x12 gr-lora,\n"
                 "\t         0x2b Meshtastic; omit to accept any\n"
                 "\t[--chase] chase-II retry: on a payload CRC fail, re-decode the\n"
                 "\t         least-reliable codewords on their runner-ups\n"
                 "\t[--report-bad] also write CRC-invalid frames to stdout\n"
                 "\t         (default: only CRC-valid and CRC-absent frames)\n"
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
    // --- defaults: shared option set, file-specific center-freq 0 = off;
    // --input-rate rides the shared capture_rate field ---
    struct live_opts o = {
        .sf = 9,
        .os = 2,
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
        {"chase", no_argument, NULL, OPT_CHASE},
        {"report-bad", no_argument, NULL, OPT_REPORT_BAD},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:o:p:a:w:v::q::", long_opts, NULL)) != -1)
    {
        if (live_opt(opt, &o))
            continue;
        switch (opt)
        {
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

    if (optind >= argc || o.preamble_avg < 6 || o.sf < 5 || o.sf > 12 || o.os < 2 || o.os > 255 ||
        o.detect_avg < 1 || o.detect_avg > 255 || o.sync_word_filter < 0 ||
        o.sync_word_filter > 0xff || o.center_freq < 0.0 || o.bandwidth <= 0.0)
    {
        usage();
        return 1;
    }
    const char *file_path = argv[optind];

    struct device *dev = device_open_file(file_path, o.capture_rate);
    if (!dev)
    {
        log_error("cannot open %s\n", file_path);
        return 1;
    }

    struct source *src = source_create(dev, o.sf, o.os, o.bandwidth, o.conjugate_iq, o.dc_block);
    if (!src)
    {
        device_close(dev);
        return 1;
    }

    struct receiver *rx = live_receiver_create(&o);
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
        o.sf,
        o.os,
        o.preamble_avg,
        o.detect_avg,
        est);

    if (o.center_freq > 0.0)
        log_info(
            "SFO compensation on: cfo->sfo inference at %.6f MHz, bw %.0f kHz\n",
            o.center_freq / 1e6,
            o.bandwidth / 1e3);

    live_tail(&o);

    int n_decoded = receiver_run(rx, src);
    int failed = source_failed(src);

    receiver_destroy(rx);
    source_destroy(src);
    device_close(dev);

    if (failed)
        return 1;
    return (n_decoded > 0) ? 0 : 1;
}

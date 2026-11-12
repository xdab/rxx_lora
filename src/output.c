/*
 * Structured frame output: the stdout half of the program's reporting.
 * Human-readable progress stays on stderr through logging.h.
 */

#include "output.h"

#include <stdint.h>
#include <stdio.h>

void output_frame(const struct output_record *rec, const char *b64)
{
    printf(
        "{\"packet\":\"%s\",\"len\":%u,\"snr_db\":%.1f,"
        "\"crc_present\":%s,\"crc_valid\":%s,"
        "\"cfo_hz\":%.1f}\n",
        b64,
        (unsigned)rec->len,
        (double)rec->snr_db,
        rec->crc_present ? "true" : "false",
        rec->crc_valid < 0 ? "null" : (rec->crc_valid ? "true" : "false"),
        rec->cfo_hz);

    (void)fflush(stdout);
}

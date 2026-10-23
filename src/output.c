/*
 * Structured frame output: the stdout half of the program's reporting.
 * Human-readable progress stays on stderr through logging.h.
 */

#include "output.h"

#include <stdio.h>

#include "utilities.h"

void output_frame(const struct output_record *rec)
{
    char b64[384] = {0}; // max 341 used

    if (!rec)
        return;

    if (!rec->packet || b64_encode(rec->packet, rec->len, b64, sizeof(b64)) != 0)
        b64[0] = '\0';

    printf(
        "{\"packet\":\"%s\",\"len\":%u,\"snr_db\":%.1f,\"cr\":%u,"
        "\"crc_present\":%s,\"crc_valid\":%s,"
        "\"net0\":%d,\"net1\":%d,\"sync_word\":%u,"
        "\"cfo_hz\":%.1f,\"sfo\":%.6f,\"sto_frac\":%.6f,"
        "\"input_sample\":%zu,\"time_s\":%.6f}\n",
        b64,
        (unsigned)rec->len,
        (double)rec->snr_db,
        (unsigned)rec->cr,
        rec->crc_present ? "true" : "false",
        rec->crc_valid < 0 ? "null" : (rec->crc_valid ? "true" : "false"),
        rec->net0,
        rec->net1,
        (unsigned)rec->sync_word,
        rec->cfo_hz,
        rec->sfo,
        rec->sto_frac,
        rec->input_sample,
        rec->time_seconds);

    fflush(stdout);
}

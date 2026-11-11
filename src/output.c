/*
 * Structured frame output: the stdout half of the program's reporting.
 * Human-readable progress stays on stderr through logging.h.
 */

#include "output.h"

#include <stdint.h>
#include <stdio.h>

int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!out || 4 * ((in_len + 2) / 3) + 1 > out_cap)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len)
            v |= in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = i + 1 < in_len ? tab[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < in_len ? tab[v & 63] : '=';
    }
    out[o] = '\0';
    return 0;
}

void output_frame(const struct output_record *rec)
{
    char b64[384] = {0}; // max 341 used

    if (!rec->packet || b64_encode(rec->packet, rec->len, b64, sizeof(b64)) != 0)
        b64[0] = '\0';

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

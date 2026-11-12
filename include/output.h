/**
 * @file output.h
 * @brief Structured frame output: one JSON object per line on stdout;
 *        stderr logging (logging.h) is untouched.
 */

#ifndef OUTPUT_H
#define OUTPUT_H

#include <stddef.h>
#include <stdint.h>

// One decoded frame's fields, ready to render.
struct output_record
{
    const uint8_t *packet; // de-whitened on-air payload (res->bytes)
    uint32_t len;          // payload length in bytes (header pay_len)
    float snr_db;          // header symbols' mean on-channel SNR, in dB
    int crc_present;       // header CRC flag
    int crc_valid;         // 1 valid, 0 invalid, -1 absent (crc_present == 0)
    double cfo_hz;         // carrier-frequency offset, in Hz
};

/**
 * @brief Render one record as a single JSON line on stdout. @p b64 is the
 *        record's already base64-encoded packet (b64_encode).
 *
 * A fixed printf template, not a serializer; every other field is a JSON
 * number, boolean or null.
 */
void output_frame(const struct output_record *rec, const char *b64);

#endif // OUTPUT_H

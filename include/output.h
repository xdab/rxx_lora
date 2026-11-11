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
 * @brief Render one record as a single JSON line on stdout.
 *
 * A fixed printf template, not a serializer; packet is base64 and every
 * other field is a JSON number, boolean or null.
 */
void output_frame(const struct output_record *rec);

/**
 * @brief Encode a byte buffer as base64 into out, null-terminated.
 *
 * The output should be at least 4*(in_len+2)/3 + 1 bytes in size (including
 * the terminator).
 *
 * @return 0 on success, -1 when out is NULL or out_cap is too small
 */
int b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

#endif // OUTPUT_H

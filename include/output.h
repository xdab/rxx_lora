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
    uint8_t cr;            // coding rate 4/(4+cr), 1..4
    int crc_present;       // header CRC flag
    int crc_valid;         // 1 valid, 0 invalid, -1 absent (crc_present == 0)
    int net0;              // decoded net-id symbol 1, in FFT bins
    int net1;              // decoded net-id symbol 2, in FFT bins
    uint8_t sync_word;     // sync word rebuilt from the received net-id pair
    double cfo_hz;         // carrier-frequency offset, in Hz
    double sfo;            // sampling-frequency offset estimate
    double sto_frac;       // fractional sampling-time offset, in bins
    size_t input_sample;   // frame start, in device/file input samples
    double time_seconds;   // frame start, in seconds from stream start
};

/**
 * @brief Render one record as a single JSON line on stdout.
 *
 * A fixed printf template, not a serializer; packet is base64 and every
 * other field is a JSON number, boolean or null.
 */
void output_frame(const struct output_record *rec);

#endif // OUTPUT_H

// PCM WAV header helpers. The capture file is written in small chunks;
// these functions only touch the 44-byte header.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_offset;
    uint32_t data_bytes;
} companion_wav_info_t;

void companion_wav_header(uint8_t dst[44], uint32_t pcm_bytes, uint32_t sample_rate);

// Reads a standard PCM header. len may be just the start of the file.
bool companion_wav_parse(const uint8_t *src, size_t len, companion_wav_info_t *out);

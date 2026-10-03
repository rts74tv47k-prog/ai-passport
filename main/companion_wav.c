#include "companion_wav.h"

#include <string.h>

static void write_u16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static uint16_t read_u16(const uint8_t *src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t read_u32(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

void companion_wav_header(uint8_t dst[44], uint32_t pcm_bytes, uint32_t sample_rate) {
    memset(dst, 0, 44);
    memcpy(dst, "RIFF", 4);
    write_u32(dst + 4, 36u + pcm_bytes);
    memcpy(dst + 8, "WAVE", 4);
    memcpy(dst + 12, "fmt ", 4);
    write_u32(dst + 16, 16);
    write_u16(dst + 20, 1);
    write_u16(dst + 22, 1);
    write_u32(dst + 24, sample_rate);
    write_u32(dst + 28, sample_rate * 2u);
    write_u16(dst + 32, 2);
    write_u16(dst + 34, 16);
    memcpy(dst + 36, "data", 4);
    write_u32(dst + 40, pcm_bytes);
}

bool companion_wav_parse(const uint8_t *src, size_t len, companion_wav_info_t *out) {
    if (!src || !out || len < 44) return false;
    if (memcmp(src, "RIFF", 4) != 0 || memcmp(src + 8, "WAVE", 4) != 0) return false;
    size_t i = 12;
    bool have_fmt = false;
    bool have_data = false;
    memset(out, 0, sizeof(*out));
    while (i + 8 <= len) {
        const uint8_t *chunk = src + i;
        uint32_t size = read_u32(chunk + 4);
        if (i + 8u + size > len && memcmp(chunk, "data", 4) != 0) return false;
        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16 || i + 24 > len) return false;
            if (read_u16(src + i + 8) != 1) return false;
            out->channels = read_u16(src + i + 10);
            out->sample_rate = read_u32(src + i + 12);
            out->bits = read_u16(src + i + 22);
            have_fmt = out->channels > 0 && out->sample_rate > 0 && out->bits > 0;
        } else if (memcmp(chunk, "data", 4) == 0) {
            out->data_offset = (uint32_t)(i + 8);
            out->data_bytes = size;
            have_data = true;
            break;
        }
        i += 8u + size + (size & 1u);
    }
    return have_fmt && have_data;
}

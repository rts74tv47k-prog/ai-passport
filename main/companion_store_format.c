#include "companion_store.h"

#include <stdio.h>
#include <string.h>

#define COMPANION_MAGIC_RECORD 0x31434552u   /* REC1 */
#define COMPANION_MAGIC_CATEGORY 0x31544143u /* CAT1 */
#define COMPANION_MAGIC_PERSONA 0x31524550u  /* PER1 */
#define COMPANION_MAGIC_META 0x3154454Du     /* MET1 */
#define COMPANION_CATEGORY_VERSION 1u

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
    return (uint32_t)src[0]
        | ((uint32_t)src[1] << 8)
        | ((uint32_t)src[2] << 16)
        | ((uint32_t)src[3] << 24);
}

uint32_t companion_crc32_start(void) {
    return 0xFFFFFFFFu;
}

uint32_t companion_crc32_feed(uint32_t crc, const void *data, size_t len) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t companion_crc32_finish(uint32_t crc) {
    return ~crc;
}

uint32_t companion_crc32(const void *data, size_t len) {
    return companion_crc32_finish(companion_crc32_feed(companion_crc32_start(), data, len));
}

static bool utf8_control(uint32_t code_point) {
    return code_point < 0x20u || code_point == 0x7Fu;
}

companion_err_t companion_utf8_check(const char *text, size_t len, bool allow_empty, bool allow_controls) {
    if (len == 0) return allow_empty ? COMPANION_OK : COMPANION_ERR_ARG;
    if (!text) return COMPANION_ERR_ARG;

    size_t i = 0;
    while (i < len) {
        const uint8_t lead = (uint8_t)text[i];
        size_t need = 0;
        uint32_t code_point = 0;
        uint32_t minimum = 0;

        if (lead <= 0x7Fu) {
            need = 1;
            code_point = lead;
            minimum = 0;
        } else if ((lead & 0xE0u) == 0xC0u) {
            need = 2;
            code_point = lead & 0x1Fu;
            minimum = 0x80u;
        } else if ((lead & 0xF0u) == 0xE0u) {
            need = 3;
            code_point = lead & 0x0Fu;
            minimum = 0x800u;
        } else if ((lead & 0xF8u) == 0xF0u) {
            need = 4;
            code_point = lead & 0x07u;
            minimum = 0x10000u;
        } else {
            return COMPANION_ERR_ARG;
        }

        if (i + need > len) return COMPANION_ERR_ARG;
        for (size_t extra = 1; extra < need; extra++) {
            uint8_t cont = (uint8_t)text[i + extra];
            if ((cont & 0xC0u) != 0x80u) return COMPANION_ERR_ARG;
            code_point = (code_point << 6) | (cont & 0x3Fu);
        }
        if (code_point < minimum || code_point > 0x10FFFFu) return COMPANION_ERR_ARG;
        if (code_point >= 0xD800u && code_point <= 0xDFFFu) return COMPANION_ERR_ARG;
        if (code_point == 0 || (!allow_controls && utf8_control(code_point))) return COMPANION_ERR_ARG;
        i += need;
    }
    return COMPANION_OK;
}

static void civil_from_unix(uint32_t unix_time, int *year, unsigned *month, unsigned *day,
                            unsigned *hour, unsigned *minute, unsigned *second) {
    uint32_t days = unix_time / 86400u;
    uint32_t rem = unix_time % 86400u;
    *hour = rem / 3600u;
    *minute = (rem % 3600u) / 60u;
    *second = rem % 60u;

    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    unsigned m = mp < 10u ? mp + 3u : mp - 9u;
    y += (m <= 2u);
    *year = (int)y;
    *month = m;
    *day = d;
}

void companion_format_unix_utc(uint32_t unix_time, char out[20]) {
    if (!out) return;
    if (unix_time == 0) {
        memcpy(out, "unknown", 8);
        return;
    }
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    civil_from_unix(unix_time, &year, &month, &day, &hour, &minute, &second);
    if (year < 1970 || year > 9999) {
        memcpy(out, "unknown", 8);
        return;
    }
    snprintf(out, 20, "%04d-%02u-%02u %02u:%02u:%02u",
             year, month, day, hour, minute, second);
}

static companion_err_t encode_slot(const companion_category_slot_t *slot, uint8_t dst[72]) {
    memset(dst, 0, 72);
    if (!slot->active) return COMPANION_OK;
    if (slot->id == 0 || slot->name_len > COMPANION_CATEGORY_NAME_MAX) return COMPANION_ERR_FORMAT;
    if (companion_utf8_check(slot->name, slot->name_len, false, false) != COMPANION_OK) {
        return COMPANION_ERR_FORMAT;
    }
    write_u32(dst, slot->id);
    dst[4] = 1;
    dst[5] = slot->name_len;
    memcpy(dst + 8, slot->name, slot->name_len);
    return COMPANION_OK;
}

companion_err_t companion_category_encode(const companion_category_table_t *table,
                                         uint8_t *dst, size_t dst_len) {
    if (!table || !dst || dst_len < COMPANION_CATEGORY_FILE_BYTES) return COMPANION_ERR_ARG;
    if (table->next_id == 0) return COMPANION_ERR_ARG;

    memset(dst, 0, COMPANION_CATEGORY_FILE_BYTES);
    for (size_t i = 0; i < COMPANION_CATEGORY_MAX; i++) {
        companion_err_t err = encode_slot(&table->slots[i], dst + 16 + (i * 72u));
        if (err != COMPANION_OK) return err;
    }
    uint32_t crc = companion_crc32(dst + 16, COMPANION_CATEGORY_FILE_BYTES - 16u);
    write_u32(dst, COMPANION_MAGIC_CATEGORY);
    write_u16(dst + 4, COMPANION_CATEGORY_VERSION);
    write_u16(dst + 6, COMPANION_CATEGORY_MAX);
    write_u32(dst + 8, table->next_id);
    write_u32(dst + 12, crc);
    return COMPANION_OK;
}

static companion_err_t decode_slot(const uint8_t src[72], companion_category_slot_t *slot) {
    memset(slot, 0, sizeof(*slot));
    uint8_t active = src[4];
    uint8_t name_len = src[5];
    if (active > 1 || name_len > COMPANION_CATEGORY_NAME_MAX) return COMPANION_ERR_FORMAT;
    if (!active) return COMPANION_OK;
    uint32_t id = read_u32(src);
    if (id == 0) return COMPANION_ERR_FORMAT;
    if (companion_utf8_check((const char *)src + 8, name_len, false, false) != COMPANION_OK) {
        return COMPANION_ERR_FORMAT;
    }
    slot->id = id;
    slot->active = 1;
    slot->name_len = name_len;
    memcpy(slot->name, src + 8, name_len);
    slot->name[name_len] = '\0';
    return COMPANION_OK;
}

companion_err_t companion_category_decode(const uint8_t *src, size_t src_len,
                                         companion_category_table_t *table) {
    if (!src || !table || src_len != COMPANION_CATEGORY_FILE_BYTES) return COMPANION_ERR_FORMAT;
    if (read_u32(src) != COMPANION_MAGIC_CATEGORY) return COMPANION_ERR_FORMAT;
    if (read_u16(src + 4) != COMPANION_CATEGORY_VERSION) return COMPANION_ERR_FORMAT;
    if (read_u16(src + 6) != COMPANION_CATEGORY_MAX) return COMPANION_ERR_FORMAT;
    uint32_t next_id = read_u32(src + 8);
    uint32_t stored_crc = read_u32(src + 12);
    uint32_t actual_crc = companion_crc32(src + 16, COMPANION_CATEGORY_FILE_BYTES - 16u);
    if (next_id == 0 || stored_crc != actual_crc) return COMPANION_ERR_FORMAT;

    memset(table, 0, sizeof(*table));
    table->next_id = next_id;
    for (size_t i = 0; i < COMPANION_CATEGORY_MAX; i++) {
        companion_err_t err = decode_slot(src + 16 + (i * 72u), &table->slots[i]);
        if (err != COMPANION_OK) return err;
        if (!table->slots[i].active) continue;
        if (table->slots[i].id >= next_id) return COMPANION_ERR_FORMAT;
        for (size_t earlier = 0; earlier < i; earlier++) {
            if (!table->slots[earlier].active) continue;
            if (table->slots[earlier].id == table->slots[i].id) return COMPANION_ERR_FORMAT;
            if (table->slots[earlier].name_len == table->slots[i].name_len
                && memcmp(table->slots[earlier].name, table->slots[i].name, table->slots[i].name_len) == 0) {
                return COMPANION_ERR_FORMAT;
            }
        }
    }
    return COMPANION_OK;
}

companion_err_t companion_persona_encode(const char *text, size_t len,
                                        uint8_t *dst, size_t dst_cap, size_t *out_len) {
    if (!dst || !out_len) return COMPANION_ERR_ARG;
    if (len > COMPANION_PERSONA_MAX) return COMPANION_ERR_TOO_BIG;
    if (len > 0 && !text) return COMPANION_ERR_ARG;
    companion_err_t utf8 = companion_utf8_check(text, len, true, true);
    if (utf8 != COMPANION_OK) return utf8;
    size_t total = 12u + len;
    if (dst_cap < total) return COMPANION_ERR_ARG;
    uint32_t crc = companion_crc32(text, len);
    write_u32(dst, COMPANION_MAGIC_PERSONA);
    write_u32(dst + 4, (uint32_t)len);
    write_u32(dst + 8, crc);
    if (len > 0) memcpy(dst + 12, text, len);
    *out_len = total;
    return COMPANION_OK;
}

companion_err_t companion_persona_decode(const uint8_t *src, size_t src_len,
                                        char *dst, size_t dst_cap, size_t *out_len) {
    if (!src || src_len < 12 || !out_len) return COMPANION_ERR_FORMAT;
    if (read_u32(src) != COMPANION_MAGIC_PERSONA) return COMPANION_ERR_FORMAT;
    uint32_t text_len = read_u32(src + 4);
    uint32_t stored_crc = read_u32(src + 8);
    if (text_len > COMPANION_PERSONA_MAX || src_len != 12u + text_len) return COMPANION_ERR_FORMAT;
    const uint8_t *text = src + 12;
    if (companion_crc32(text, text_len) != stored_crc) return COMPANION_ERR_FORMAT;
    if (companion_utf8_check((const char *)text, text_len, true, true) != COMPANION_OK) {
        return COMPANION_ERR_FORMAT;
    }
    *out_len = text_len;
    if (!dst) return COMPANION_OK;
    if (dst_cap <= text_len) return COMPANION_ERR_TOO_BIG;
    if (text_len > 0) memcpy(dst, text, text_len);
    dst[text_len] = '\0';
    return COMPANION_OK;
}

companion_err_t companion_meta_encode(uint32_t next_id, uint8_t dst[12]) {
    if (!dst || next_id == 0) return COMPANION_ERR_ARG;
    uint8_t raw[4];
    write_u32(raw, next_id);
    write_u32(dst, COMPANION_MAGIC_META);
    write_u32(dst + 4, next_id);
    write_u32(dst + 8, companion_crc32(raw, sizeof raw));
    return COMPANION_OK;
}

companion_err_t companion_meta_decode(const uint8_t *src, size_t src_len, uint32_t *next_id) {
    if (!src || !next_id || src_len != 12) return COMPANION_ERR_FORMAT;
    if (read_u32(src) != COMPANION_MAGIC_META) return COMPANION_ERR_FORMAT;
    uint32_t value = read_u32(src + 4);
    uint8_t raw[4];
    write_u32(raw, value);
    if (value == 0 || companion_crc32(raw, sizeof raw) != read_u32(src + 8)) return COMPANION_ERR_FORMAT;
    *next_id = value;
    return COMPANION_OK;
}

void companion_record_prefix(uint8_t prefix[18], uint32_t id, uint32_t category_id,
                             uint32_t created_unix, uint16_t text_len) {
    write_u32(prefix, COMPANION_MAGIC_RECORD);
    write_u32(prefix + 4, id);
    write_u32(prefix + 8, category_id);
    write_u32(prefix + 12, created_unix);
    write_u16(prefix + 16, text_len);
}

companion_err_t companion_record_encode_header(uint8_t dst[COMPANION_RECORD_HEADER_BYTES],
                                              uint32_t id, uint32_t category_id,
                                              uint32_t created_unix, uint16_t text_len,
                                              uint8_t deleted0, uint8_t deleted1,
                                              const uint8_t *text) {
    if (!dst || id == 0) return COMPANION_ERR_ARG;
    if (text_len > COMPANION_RECORD_TEXT_MAX) return COMPANION_ERR_TOO_BIG;
    if (text_len > 0 && !text) return COMPANION_ERR_ARG;
    uint8_t prefix[18];
    companion_record_prefix(prefix, id, category_id, created_unix, text_len);
    uint32_t crc = companion_crc32_start();
    crc = companion_crc32_feed(crc, prefix, sizeof prefix);
    crc = companion_crc32_feed(crc, text, text_len);
    crc = companion_crc32_finish(crc);
    memcpy(dst, prefix, 18);
    dst[18] = deleted0;
    dst[19] = deleted1;
    write_u32(dst + 20, crc);
    return COMPANION_OK;
}

bool companion_record_magic_ok(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]) {
    return hdr && read_u32(hdr) == COMPANION_MAGIC_RECORD;
}

uint32_t companion_record_id(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]) {
    return read_u32(hdr + 4);
}

uint32_t companion_record_category_id(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]) {
    return read_u32(hdr + 8);
}

uint32_t companion_record_created(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]) {
    return read_u32(hdr + 12);
}

uint16_t companion_record_text_len(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]) {
    return read_u16(hdr + 16);
}

uint32_t companion_record_stored_crc(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]) {
    return read_u32(hdr + 20);
}

bool companion_record_is_live(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]) {
    return hdr[18] != 1 || hdr[19] != 1;
}

// Companion knowledge-base store.
//
// Records, categories, and the persona live in the FAT "storage" partition.
// Raw audio is never stored. Callers must not log record text, persona text,
// or category names; this module logs only ids, lengths, and error codes.
//
// companion_files_* is the portable file layer (host tests and the device).
// companion_store_* mounts that layer on the device and serializes access.
// The files object is several kilobytes; allocate it with companion_files_alloc(),
// never on a small task stack. No single heap block in this module exceeds 32 KB.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COMPANION_RECORD_TEXT_MAX 8192u
#define COMPANION_PERSONA_MAX 4096u
#define COMPANION_CATEGORY_NAME_MAX 63u
#define COMPANION_CATEGORY_MAX 32u
#define COMPANION_RECORDS_BYTES_LIMIT (3u * 1024u * 1024u)
#define COMPANION_RECORD_HEADER_BYTES 24u
#define COMPANION_CATEGORY_FILE_BYTES (16u + (COMPANION_CATEGORY_MAX * 72u))

#define COMPANION_FILE_RECORDS "RECORDS.DAT"
#define COMPANION_FILE_RECORDS_NEW "RECORDS.NEW"
#define COMPANION_FILE_CATEGORIES "CATEG.DAT"
#define COMPANION_FILE_CATEGORIES_NEW "CATEG.NEW"
#define COMPANION_FILE_PERSONA "PERSONA.TXT"
#define COMPANION_FILE_PERSONA_NEW "PERSONA.NEW"
#define COMPANION_FILE_META "META.DAT"
#define COMPANION_FILE_META_NEW "META.NEW"
#define COMPANION_FILE_RECORD_COMMIT "REC.OK"

typedef enum {
    COMPANION_OK = 0,
    COMPANION_ERR_ARG = 1,
    COMPANION_ERR_IO = 2,
    COMPANION_ERR_FULL = 3,
    COMPANION_ERR_NOT_FOUND = 4,
    COMPANION_ERR_EXISTS = 5,
    COMPANION_ERR_TOO_BIG = 6,
    COMPANION_ERR_FORMAT = 7,
    COMPANION_ERR_BUSY = 8,
    COMPANION_ERR_NO_MEM = 9,
    COMPANION_ERR_STATE = 10,
} companion_err_t;

typedef struct {
    uint32_t id;
    uint8_t active;
    uint8_t name_len;
    char name[COMPANION_CATEGORY_NAME_MAX + 1];
} companion_category_slot_t;

typedef struct {
    uint32_t next_id;
    companion_category_slot_t slots[COMPANION_CATEGORY_MAX];
} companion_category_table_t;

typedef struct {
    uint32_t id;
    char name[COMPANION_CATEGORY_NAME_MAX + 1];
} companion_category_t;

typedef struct {
    uint32_t id;
    uint32_t category_id;
    uint32_t created_unix;
    uint16_t text_len;
} companion_record_info_t;

typedef companion_err_t (*companion_export_write_fn)(void *ctx, const void *data, size_t len);

uint32_t companion_crc32(const void *data, size_t len);
uint32_t companion_crc32_start(void);
uint32_t companion_crc32_feed(uint32_t crc, const void *data, size_t len);
uint32_t companion_crc32_finish(uint32_t crc);

// allow_empty: persona may be empty; category names and records may not.
// allow_controls: records and persona may contain tab and newline; category names may not.
// U+0000 is always rejected. Returns COMPANION_ERR_ARG when the bytes are not valid UTF-8.
companion_err_t companion_utf8_check(const char *text, size_t len, bool allow_empty, bool allow_controls);

// out must hold 20 bytes. Unix time 0 is the "clock unset" sentinel and formats as "unknown".
void companion_format_unix_utc(uint32_t unix_time, char out[20]);

companion_err_t companion_category_encode(const companion_category_table_t *table,
                                         uint8_t *dst, size_t dst_len);
companion_err_t companion_category_decode(const uint8_t *src, size_t src_len,
                                         companion_category_table_t *table);

companion_err_t companion_persona_encode(const char *text, size_t len,
                                        uint8_t *dst, size_t dst_cap, size_t *out_len);
companion_err_t companion_persona_decode(const uint8_t *src, size_t src_len,
                                        char *dst, size_t dst_cap, size_t *out_len);

companion_err_t companion_meta_encode(uint32_t next_id, uint8_t dst[12]);
companion_err_t companion_meta_decode(const uint8_t *src, size_t src_len, uint32_t *next_id);

companion_err_t companion_record_encode_header(uint8_t dst[COMPANION_RECORD_HEADER_BYTES],
                                              uint32_t id, uint32_t category_id,
                                              uint32_t created_unix, uint16_t text_len,
                                              uint8_t deleted0, uint8_t deleted1,
                                              const uint8_t *text);
bool companion_record_magic_ok(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]);
uint32_t companion_record_id(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]);
uint32_t companion_record_category_id(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]);
uint32_t companion_record_created(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]);
uint16_t companion_record_text_len(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]);
uint32_t companion_record_stored_crc(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]);
bool companion_record_is_live(const uint8_t hdr[COMPANION_RECORD_HEADER_BYTES]);
void companion_record_prefix(uint8_t prefix[18], uint32_t id, uint32_t category_id,
                             uint32_t created_unix, uint16_t text_len);

typedef struct companion_files companion_files_t;

companion_files_t *companion_files_alloc(void);
void companion_files_free(companion_files_t *store);
companion_err_t companion_files_open(companion_files_t *store, const char *dir, uint32_t records_limit);
void companion_files_close(companion_files_t *store);

companion_err_t companion_files_persona_get(companion_files_t *store, char *buf, size_t buf_len, size_t *out_len);
companion_err_t companion_files_persona_set(companion_files_t *store, const char *text, size_t len);

companion_err_t companion_files_category_create(companion_files_t *store, const char *name, size_t len, uint32_t *out_id);
companion_err_t companion_files_category_rename(companion_files_t *store, uint32_t id, const char *name, size_t len);
companion_err_t companion_files_category_delete(companion_files_t *store, uint32_t id);
companion_err_t companion_files_category_count(companion_files_t *store, size_t *out_count);
companion_err_t companion_files_category_at(companion_files_t *store, size_t index, companion_category_t *out);
companion_err_t companion_files_category_find(companion_files_t *store, uint32_t id, companion_category_t *out);

// created_unix 0 means the clock was unset. text is the transcript only.
companion_err_t companion_files_record_append(companion_files_t *store, uint32_t category_id,
                                             uint32_t created_unix, const char *text, size_t text_len,
                                             uint32_t *out_id);
companion_err_t companion_files_record_delete(companion_files_t *store, uint32_t id);
companion_err_t companion_files_record_set_category(companion_files_t *store, uint32_t id, uint32_t category_id);
// category_id 0 counts or lists every live record. Index order is file order.
companion_err_t companion_files_record_count(companion_files_t *store, uint32_t category_id, size_t *out_count);
companion_err_t companion_files_record_at(companion_files_t *store, uint32_t category_id, size_t index,
                                         companion_record_info_t *info, char *text, size_t text_cap,
                                         size_t *out_text_len);

// UTF-8 with BOM, LF newlines, one pass. Suitable for Notepad and WeChat.
companion_err_t companion_files_export_txt(companion_files_t *store, companion_export_write_fn write, void *ctx);
companion_err_t companion_files_compact(companion_files_t *store);

companion_err_t companion_store_mount(void);
void companion_store_unmount(void);
bool companion_store_ready(void);

companion_err_t companion_store_persona_get(char *buf, size_t buf_len, size_t *out_len);
companion_err_t companion_store_persona_set(const char *text, size_t len);
companion_err_t companion_store_category_create(const char *name, size_t len, uint32_t *out_id);
companion_err_t companion_store_category_rename(uint32_t id, const char *name, size_t len);
companion_err_t companion_store_category_delete(uint32_t id);
companion_err_t companion_store_category_count(size_t *out_count);
companion_err_t companion_store_category_at(size_t index, companion_category_t *out);
companion_err_t companion_store_category_find(uint32_t id, companion_category_t *out);
companion_err_t companion_store_record_append(uint32_t category_id, uint32_t created_unix,
                                             const char *text, size_t text_len, uint32_t *out_id);
companion_err_t companion_store_record_delete(uint32_t id);
companion_err_t companion_store_record_set_category(uint32_t id, uint32_t category_id);
companion_err_t companion_store_record_count(uint32_t category_id, size_t *out_count);
companion_err_t companion_store_record_at(uint32_t category_id, size_t index, companion_record_info_t *info,
                                         char *text, size_t text_cap, size_t *out_text_len);
companion_err_t companion_store_export_txt(companion_export_write_fn write, void *ctx);

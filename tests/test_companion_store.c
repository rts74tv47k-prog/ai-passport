#include "companion_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[128];

static void path_of(const char *name, char *out, size_t out_len) {
    int wrote = snprintf(out, out_len, "%s/%s", g_dir, name);
    assert(wrote > 0 && (size_t)wrote < out_len);
}

static void remove_store_files(void) {
    static const char *names[] = {
        COMPANION_FILE_RECORDS, COMPANION_FILE_RECORDS_NEW, COMPANION_FILE_RECORD_COMMIT,
        COMPANION_FILE_CATEGORIES, COMPANION_FILE_CATEGORIES_NEW,
        COMPANION_FILE_PERSONA, COMPANION_FILE_PERSONA_NEW,
        COMPANION_FILE_META, COMPANION_FILE_META_NEW,
    };
    char path[192];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        path_of(names[i], path, sizeof path);
        remove(path);
    }
}

static companion_files_t *open_store(uint32_t limit) {
    companion_files_t *store = companion_files_alloc();
    assert(store);
    assert(companion_files_open(store, g_dir, limit) == COMPANION_OK);
    return store;
}

static void reopen(companion_files_t **store, uint32_t limit) {
    companion_files_close(*store);
    assert(companion_files_open(*store, g_dir, limit) == COMPANION_OK);
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} capture_t;

static companion_err_t capture_write(void *ctx, const void *data, size_t len) {
    capture_t *capture = ctx;
    if (capture->len + len >= capture->cap) return COMPANION_ERR_FULL;
    memcpy(capture->buf + capture->len, data, len);
    capture->len += len;
    capture->buf[capture->len] = '\0';
    return COMPANION_OK;
}

static long file_length(const char *name) {
    char path[192];
    path_of(name, path, sizeof path);
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    fclose(file);
    return size;
}

int main(void) {
    assert(companion_crc32("123456789", 9) == 0xCBF43926u);

    assert(companion_utf8_check("abc", 3, false, false) == COMPANION_OK);
    assert(companion_utf8_check("\xE4\xBD\xA0\xE5\xA5\xBD", 6, false, false) == COMPANION_OK);
    assert(companion_utf8_check("", 0, true, false) == COMPANION_OK);
    assert(companion_utf8_check("", 0, false, false) == COMPANION_ERR_ARG);
    assert(companion_utf8_check("a\nb", 3, false, false) == COMPANION_ERR_ARG);
    assert(companion_utf8_check("a\nb", 3, false, true) == COMPANION_OK);
    assert(companion_utf8_check("\xC0\x80", 2, false, true) == COMPANION_ERR_ARG);
    assert(companion_utf8_check("\xE4\xBD", 2, false, true) == COMPANION_ERR_ARG);
    assert(companion_utf8_check("\xED\xA0\x80", 3, false, true) == COMPANION_ERR_ARG);
    assert(companion_utf8_check("bad\0hidden", 10, false, true) == COMPANION_ERR_ARG);

    char when[20];
    companion_format_unix_utc(0, when);
    assert(strcmp(when, "unknown") == 0);
    companion_format_unix_utc(1696118400u, when);
    assert(strcmp(when, "2023-10-01 00:00:00") == 0);
    companion_format_unix_utc(1582934400u, when);
    assert(strcmp(when, "2020-02-29 00:00:00") == 0);

    uint8_t header[COMPANION_RECORD_HEADER_BYTES];
    assert(companion_record_encode_header(header, 1, 2, 3, 4, 0, 0, (const uint8_t *)"abcd") == COMPANION_OK);
    uint8_t prefix[18];
    companion_record_prefix(prefix, 1, 2, 3, 4);
    uint32_t crc = companion_crc32_feed(companion_crc32_start(), prefix, sizeof prefix);
    crc = companion_crc32_finish(companion_crc32_feed(crc, "abcd", 4));
    assert(crc == companion_record_stored_crc(header));
    assert(companion_record_is_live(header));
    header[18] = 1;
    assert(companion_record_is_live(header));
    header[19] = 1;
    assert(!companion_record_is_live(header));

    companion_category_table_t *table = calloc(1, sizeof(*table));
    companion_category_table_t *decoded = calloc(1, sizeof(*decoded));
    uint8_t *encoded = malloc(COMPANION_CATEGORY_FILE_BYTES);
    assert(table && decoded && encoded);
    table->next_id = 2;
    table->slots[0].active = 1;
    table->slots[0].id = 1;
    table->slots[0].name_len = 4;
    memcpy(table->slots[0].name, "work", 4);
    assert(companion_category_encode(table, encoded, COMPANION_CATEGORY_FILE_BYTES) == COMPANION_OK);
    assert(companion_category_decode(encoded, COMPANION_CATEGORY_FILE_BYTES, decoded) == COMPANION_OK);
    assert(decoded->slots[0].id == 1);
    assert(strcmp(decoded->slots[0].name, "work") == 0);
    encoded[16] ^= 0x01;
    assert(companion_category_decode(encoded, COMPANION_CATEGORY_FILE_BYTES, decoded) == COMPANION_ERR_FORMAT);
    free(table);
    free(decoded);
    free(encoded);

    snprintf(g_dir, sizeof g_dir, "/tmp/companion-store-XXXXXX");
    assert(mkdtemp(g_dir) != NULL);

    companion_files_t *store = open_store(COMPANION_RECORDS_BYTES_LIMIT);
    size_t persona_len = 99;
    assert(companion_files_persona_get(store, NULL, 0, &persona_len) == COMPANION_OK);
    assert(persona_len == 0);
    assert(companion_files_persona_set(store, "\xE4\xBD\xA0\xE5\xA5\xBD", 6) == COMPANION_OK);
    char persona[32];
    assert(companion_files_persona_get(store, persona, sizeof persona, &persona_len) == COMPANION_OK);
    assert(persona_len == 6);
    assert(strcmp(persona, "\xE4\xBD\xA0\xE5\xA5\xBD") == 0);
    char big[COMPANION_PERSONA_MAX + 1];
    memset(big, 'a', sizeof big);
    assert(companion_files_persona_set(store, big, COMPANION_PERSONA_MAX + 1) == COMPANION_ERR_TOO_BIG);
    assert(companion_files_persona_get(store, persona, sizeof persona, &persona_len) == COMPANION_OK);
    assert(strcmp(persona, "\xE4\xBD\xA0\xE5\xA5\xBD") == 0);

    uint32_t notes_id = 0;
    uint32_t tasks_id = 0;
    assert(companion_files_category_create(store, "\n", 1, &notes_id) == COMPANION_ERR_ARG);
    assert(companion_files_category_create(store, "\xE7\xAC\x94\xE8\xAE\xB0", 6, &notes_id) == COMPANION_OK);
    assert(companion_files_category_create(store, "\xE7\xAC\x94\xE8\xAE\xB0", 6, &tasks_id) == COMPANION_ERR_EXISTS);
    assert(companion_files_category_create(store, "tasks", 5, &tasks_id) == COMPANION_OK);
    assert(companion_files_category_rename(store, notes_id, "notes", 5) == COMPANION_OK);
    size_t category_count = 0;
    assert(companion_files_category_count(store, &category_count) == COMPANION_OK);
    assert(category_count == 2);
    companion_category_t category;
    assert(companion_files_category_at(store, 0, &category) == COMPANION_OK);
    assert(category.id == notes_id);
    assert(strcmp(category.name, "notes") == 0);

    uint32_t record_id = 0;
    assert(companion_files_record_append(store, 99, 1696118400u, "orphan", 6, &record_id) == COMPANION_ERR_NOT_FOUND);
    assert(companion_files_record_append(store, notes_id, 1696118400u, "", 0, &record_id) == COMPANION_ERR_TOO_BIG);
    assert(companion_files_record_append(store, notes_id, 1696118400u, "KEEP-ME", 7, &record_id) == COMPANION_OK);
    assert(record_id == 1);
    uint32_t deleted_id = 0;
    assert(companion_files_record_append(store, notes_id, 0, "DELETE-ME", 9, &deleted_id) == COMPANION_OK);
    assert(companion_files_record_set_category(store, record_id, tasks_id) == COMPANION_OK);
    companion_record_info_t info;
    char text[32];
    size_t text_len = 0;
    assert(companion_files_record_at(store, tasks_id, 0, &info, text, sizeof text, &text_len) == COMPANION_OK);
    assert(info.id == record_id);
    assert(info.category_id == tasks_id);
    assert(info.created_unix == 1696118400u);
    assert(text_len == 7);
    assert(strcmp(text, "KEEP-ME") == 0);
    size_t in_notes = 9;
    assert(companion_files_record_count(store, notes_id, &in_notes) == COMPANION_OK);
    assert(in_notes == 1);
    assert(companion_files_category_delete(store, notes_id) == COMPANION_ERR_BUSY);
    assert(companion_files_record_delete(store, deleted_id) == COMPANION_OK);
    assert(companion_files_record_count(store, notes_id, &in_notes) == COMPANION_OK);
    assert(in_notes == 0);
    assert(companion_files_category_delete(store, notes_id) == COMPANION_OK);
    assert(companion_files_record_delete(store, deleted_id) == COMPANION_ERR_NOT_FOUND);

    char export_buf[1024];
    capture_t capture = {.buf = export_buf, .cap = sizeof export_buf};
    assert(companion_files_export_txt(store, capture_write, &capture) == COMPANION_OK);
    assert(memcmp(export_buf, "\xEF\xBB\xBF# Categories\n", 16) == 0);
    assert(strstr(export_buf, "- tasks\n") != NULL);
    assert(strstr(export_buf, "[2023-10-01 00:00:00] tasks\nKEEP-ME\n") != NULL);
    assert(strstr(export_buf, "DELETE-ME") == NULL);
    assert(strstr(export_buf, "notes") == NULL);

    reopen(&store, COMPANION_RECORDS_BYTES_LIMIT);
    assert(companion_files_persona_get(store, persona, sizeof persona, &persona_len) == COMPANION_OK);
    assert(strcmp(persona, "\xE4\xBD\xA0\xE5\xA5\xBD") == 0);
    assert(companion_files_record_count(store, 0, &in_notes) == COMPANION_OK);
    assert(in_notes == 1);
    assert(companion_files_category_find(store, tasks_id, &category) == COMPANION_OK);
    assert(strcmp(category.name, "tasks") == 0);

    companion_files_close(store);
    char records_path[192];
    path_of(COMPANION_FILE_RECORDS, records_path, sizeof records_path);
    FILE *records = fopen(records_path, "ab");
    assert(records);
    unsigned char garbage[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    assert(fwrite(garbage, 1, sizeof garbage, records) == sizeof garbage);
    fclose(records);
    assert(companion_files_open(store, g_dir, COMPANION_RECORDS_BYTES_LIMIT) == COMPANION_OK);
    assert(companion_files_record_count(store, 0, &in_notes) == COMPANION_OK);
    assert(in_notes == 1);
    assert(file_length(COMPANION_FILE_RECORDS) == (long)(COMPANION_RECORD_HEADER_BYTES + 7
        + COMPANION_RECORD_HEADER_BYTES + 9));

    assert(companion_files_record_append(store, tasks_id, 1582934400u, "bbbb", 4, &record_id) == COMPANION_OK);
    companion_files_close(store);
    records = fopen(records_path, "r+b");
    assert(records);
    long flip_at = (long)(COMPANION_RECORD_HEADER_BYTES + 7 + COMPANION_RECORD_HEADER_BYTES + 9
        + COMPANION_RECORD_HEADER_BYTES);
    assert(fseek(records, flip_at, SEEK_SET) == 0);
    assert(fputc('X', records) == 'X');
    fclose(records);
    assert(companion_files_open(store, g_dir, COMPANION_RECORDS_BYTES_LIMIT) == COMPANION_OK);
    assert(companion_files_record_count(store, 0, &in_notes) == COMPANION_OK);
    assert(in_notes == 1);
    assert(companion_files_record_at(store, 0, 0, &info, text, sizeof text, &text_len) == COMPANION_OK);
    assert(strcmp(text, "KEEP-ME") == 0);

    char meta_path[192];
    path_of(COMPANION_FILE_META, meta_path, sizeof meta_path);
    assert(remove(meta_path) == 0);
    reopen(&store, COMPANION_RECORDS_BYTES_LIMIT);
    uint32_t next_id = 0;
    assert(companion_files_record_append(store, tasks_id, 0, "after-meta", 10, &next_id) == COMPANION_OK);
    assert(next_id == 3);

    companion_files_close(store);
    char persona_path[192];
    char persona_new[192];
    path_of(COMPANION_FILE_PERSONA, persona_path, sizeof persona_path);
    path_of(COMPANION_FILE_PERSONA_NEW, persona_new, sizeof persona_new);
    assert(rename(persona_path, persona_new) == 0);
    assert(companion_files_open(store, g_dir, COMPANION_RECORDS_BYTES_LIMIT) == COMPANION_OK);
    assert(companion_files_persona_set(store, "replacement", 11) == COMPANION_OK);
    companion_files_close(store);
    FILE *side = fopen(persona_new, "wb");
    assert(side);
    uint8_t persona_blob[64];
    size_t persona_blob_len = 0;
    assert(companion_persona_encode("from-temp", 9, persona_blob, sizeof persona_blob, &persona_blob_len) == COMPANION_OK);
    assert(fwrite(persona_blob, 1, persona_blob_len, side) == persona_blob_len);
    fclose(side);
    assert(companion_files_open(store, g_dir, COMPANION_RECORDS_BYTES_LIMIT) == COMPANION_OK);
    assert(companion_files_persona_get(store, persona, sizeof persona, &persona_len) == COMPANION_OK);
    assert(strcmp(persona, "from-temp") == 0);

    companion_files_free(store);
    remove_store_files();
    store = open_store(56);
    assert(companion_files_category_create(store, "box", 3, &notes_id) == COMPANION_OK);
    assert(companion_files_record_append(store, notes_id, 1, "aaaa", 4, &record_id) == COMPANION_OK);
    assert(companion_files_record_append(store, notes_id, 1, "bbbb", 4, &deleted_id) == COMPANION_OK);
    assert(companion_files_record_append(store, notes_id, 1, "cccc", 4, &next_id) == COMPANION_ERR_FULL);
    assert(companion_files_record_delete(store, record_id) == COMPANION_OK);
    assert(companion_files_record_append(store, notes_id, 1, "dddd", 4, &next_id) == COMPANION_OK);
    reopen(&store, 56);
    assert(companion_files_record_count(store, 0, &in_notes) == COMPANION_OK);
    assert(in_notes == 2);
    assert(companion_files_record_at(store, 0, 0, &info, text, sizeof text, &text_len) == COMPANION_OK);
    assert(strcmp(text, "bbbb") == 0);
    assert(companion_files_record_at(store, 0, 1, &info, text, sizeof text, &text_len) == COMPANION_OK);
    assert(strcmp(text, "dddd") == 0);

    for (int i = 0; i < (int)COMPANION_CATEGORY_MAX - 1; i++) {
        char name[8];
        int n = snprintf(name, sizeof name, "c%d", i);
        assert(n > 0);
        uint32_t id = 0;
        assert(companion_files_category_create(store, name, (size_t)n, &id) == COMPANION_OK);
    }
    assert(companion_files_category_create(store, "overflow", 8, &next_id) == COMPANION_ERR_FULL);

    companion_files_free(store);
    remove_store_files();
    assert(rmdir(g_dir) == 0);
    return 0;
}

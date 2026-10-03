#include "companion_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RECORD_COMMIT_MAGIC 0x314B4F52u /* ROK1 */

struct companion_files {
    char dir[160];
    uint32_t records_limit;
    uint32_t next_record_id;
    uint32_t records_end;
    uint32_t records_live_bytes;
    bool open;
    companion_category_table_t categories;
};

typedef struct {
    uint32_t offset;
    uint32_t id;
    uint32_t category_id;
    uint32_t created_unix;
    uint16_t text_len;
} record_loc_t;

_Static_assert(sizeof(companion_category_table_t) < 4096, "category table must stay off small stacks");
_Static_assert(sizeof(struct companion_files) < 8192, "store object must stay a single small allocation");
_Static_assert(COMPANION_CATEGORY_FILE_BYTES == 2320, "category file layout changed");
_Static_assert(COMPANION_RECORD_TEXT_MAX <= 8192, "record text exceeds the 32 KB block budget");
_Static_assert(COMPANION_PERSONA_MAX + 12u <= 8192, "persona file exceeds the 32 KB block budget");

static companion_err_t require_open(companion_files_t *store) {
    if (!store) return COMPANION_ERR_ARG;
    if (!store->open) return COMPANION_ERR_STATE;
    return COMPANION_OK;
}

static companion_err_t make_path(const companion_files_t *store, const char *name,
                                char *out, size_t out_len) {
    int wrote = snprintf(out, out_len, "%s/%s", store->dir, name);
    if (wrote < 0 || (size_t)wrote >= out_len) return COMPANION_ERR_ARG;
    return COMPANION_OK;
}

static companion_err_t sync_file(FILE *file) {
    if (fflush(file) != 0) return COMPANION_ERR_IO;
    if (fsync(fileno(file)) != 0) return COMPANION_ERR_IO;
    return COMPANION_OK;
}

static void sync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    (void)fsync(fd);
    close(fd);
}

static void remove_file(const char *path) {
    if (remove(path) != 0 && errno != ENOENT) {
        /* Best-effort cleanup. The caller still checks the name that must remain. */
    }
}

static companion_err_t read_at(FILE *file, uint32_t offset, void *buf, size_t len) {
    if (len == 0) return COMPANION_OK;
    if (fseek(file, (long)offset, SEEK_SET) != 0) return COMPANION_ERR_IO;
    if (fread(buf, 1, len, file) != len) return COMPANION_ERR_IO;
    return COMPANION_OK;
}

static companion_err_t read_capped(const char *path, uint8_t **out, size_t *out_len, size_t max_len) {
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? COMPANION_ERR_NOT_FOUND : COMPANION_ERR_IO;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    if ((size_t)size > max_len) {
        fclose(file);
        return COMPANION_ERR_FORMAT;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    uint8_t *buf = malloc(size == 0 ? 1 : (size_t)size);
    if (!buf) {
        fclose(file);
        return COMPANION_ERR_NO_MEM;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, file) != (size_t)size) {
        free(buf);
        fclose(file);
        return COMPANION_ERR_IO;
    }
    fclose(file);
    *out = buf;
    *out_len = (size_t)size;
    return COMPANION_OK;
}

static companion_err_t write_temp_file(const char *path, const uint8_t *data, size_t len) {
    FILE *file = fopen(path, "wb");
    if (!file) return COMPANION_ERR_IO;
    if (len > 0 && fwrite(data, 1, len, file) != len) {
        int err = errno;
        fclose(file);
        remove_file(path);
        return err == ENOSPC ? COMPANION_ERR_FULL : COMPANION_ERR_IO;
    }
    companion_err_t sync_err = sync_file(file);
    fclose(file);
    if (sync_err != COMPANION_OK) {
        remove_file(path);
        return sync_err;
    }
    return COMPANION_OK;
}

/* A fully synced temp file is already durable. Promotion failure leaves it for the next open. */
static companion_err_t publish_temp(companion_files_t *store, const char *final_name,
                                   const char *temp_name, const uint8_t *data, size_t len) {
    char final_path[192];
    char temp_path[192];
    companion_err_t err = make_path(store, final_name, final_path, sizeof final_path);
    if (err != COMPANION_OK) return err;
    err = make_path(store, temp_name, temp_path, sizeof temp_path);
    if (err != COMPANION_OK) return err;
    err = write_temp_file(temp_path, data, len);
    if (err != COMPANION_OK) return err;
    remove_file(final_path);
    if (rename(temp_path, final_path) != 0) {
        sync_dir(store->dir);
        return COMPANION_OK;
    }
    sync_dir(store->dir);
    return COMPANION_OK;
}

static bool category_bytes_ok(const uint8_t *bytes, size_t len, companion_category_table_t *table) {
    return companion_category_decode(bytes, len, table) == COMPANION_OK;
}

static companion_err_t load_categories(companion_files_t *store) {
    char final_path[192];
    char temp_path[192];
    companion_err_t err = make_path(store, COMPANION_FILE_CATEGORIES, final_path, sizeof final_path);
    if (err != COMPANION_OK) return err;
    err = make_path(store, COMPANION_FILE_CATEGORIES_NEW, temp_path, sizeof temp_path);
    if (err != COMPANION_OK) return err;

    uint8_t *bytes = NULL;
    size_t len = 0;
    companion_category_table_t *loaded = malloc(sizeof(*loaded));
    if (!loaded) return COMPANION_ERR_NO_MEM;

    err = read_capped(temp_path, &bytes, &len, COMPANION_CATEGORY_FILE_BYTES);
    if (err == COMPANION_ERR_NO_MEM || err == COMPANION_ERR_IO) {
        free(loaded);
        return err;
    }
    if (err == COMPANION_OK && category_bytes_ok(bytes, len, loaded)) {
        free(bytes);
        store->categories = *loaded;
        free(loaded);
        remove_file(final_path);
        if (rename(temp_path, final_path) != 0) sync_dir(store->dir);
        return COMPANION_OK;
    }
    free(bytes);
    bytes = NULL;
    if (err == COMPANION_OK || err == COMPANION_ERR_FORMAT) remove_file(temp_path);

    err = read_capped(final_path, &bytes, &len, COMPANION_CATEGORY_FILE_BYTES);
    if (err == COMPANION_ERR_NOT_FOUND) {
        memset(loaded, 0, sizeof(*loaded));
        loaded->next_id = 1;
        uint8_t *encoded = malloc(COMPANION_CATEGORY_FILE_BYTES);
        if (!encoded) {
            free(loaded);
            return COMPANION_ERR_NO_MEM;
        }
        err = companion_category_encode(loaded, encoded, COMPANION_CATEGORY_FILE_BYTES);
        if (err == COMPANION_OK) {
            err = publish_temp(store, COMPANION_FILE_CATEGORIES, COMPANION_FILE_CATEGORIES_NEW,
                               encoded, COMPANION_CATEGORY_FILE_BYTES);
        }
        free(encoded);
        if (err == COMPANION_OK) store->categories = *loaded;
        free(loaded);
        return err;
    }
    if (err != COMPANION_OK) {
        free(loaded);
        return err;
    }
    bool ok = category_bytes_ok(bytes, len, loaded);
    free(bytes);
    if (!ok) {
        free(loaded);
        return COMPANION_ERR_FORMAT;
    }
    store->categories = *loaded;
    free(loaded);
    return COMPANION_OK;
}

static companion_err_t save_categories(companion_files_t *store, const companion_category_table_t *table) {
    uint8_t *encoded = malloc(COMPANION_CATEGORY_FILE_BYTES);
    if (!encoded) return COMPANION_ERR_NO_MEM;
    companion_err_t err = companion_category_encode(table, encoded, COMPANION_CATEGORY_FILE_BYTES);
    if (err == COMPANION_OK) {
        err = publish_temp(store, COMPANION_FILE_CATEGORIES, COMPANION_FILE_CATEGORIES_NEW,
                           encoded, COMPANION_CATEGORY_FILE_BYTES);
    }
    free(encoded);
    if (err == COMPANION_OK && table != &store->categories) store->categories = *table;
    return err;
}

static bool meta_bytes_ok(const uint8_t *bytes, size_t len, uint32_t *next_id) {
    return companion_meta_decode(bytes, len, next_id) == COMPANION_OK;
}

static companion_err_t load_meta(companion_files_t *store, bool *found) {
    char final_path[192];
    char temp_path[192];
    *found = false;
    companion_err_t err = make_path(store, COMPANION_FILE_META, final_path, sizeof final_path);
    if (err != COMPANION_OK) return err;
    err = make_path(store, COMPANION_FILE_META_NEW, temp_path, sizeof temp_path);
    if (err != COMPANION_OK) return err;

    uint8_t *bytes = NULL;
    size_t len = 0;
    uint32_t next_id = 1;
    err = read_capped(temp_path, &bytes, &len, 12);
    if (err == COMPANION_ERR_NO_MEM || err == COMPANION_ERR_IO) return err;
    if (err == COMPANION_OK && meta_bytes_ok(bytes, len, &next_id)) {
        free(bytes);
        store->next_record_id = next_id;
        *found = true;
        remove_file(final_path);
        if (rename(temp_path, final_path) != 0) sync_dir(store->dir);
        return COMPANION_OK;
    }
    free(bytes);
    bytes = NULL;
    if (err == COMPANION_OK || err == COMPANION_ERR_FORMAT) remove_file(temp_path);

    err = read_capped(final_path, &bytes, &len, 12);
    if (err == COMPANION_ERR_NOT_FOUND) return COMPANION_OK;
    if (err != COMPANION_OK) return err;
    bool ok = meta_bytes_ok(bytes, len, &next_id);
    free(bytes);
    if (!ok) {
        /* Next-id cache only. A damaged file must not hide records; the scan rebuilds it. */
        remove_file(final_path);
        return COMPANION_OK;
    }
    store->next_record_id = next_id;
    *found = true;
    return COMPANION_OK;
}

static companion_err_t save_meta(companion_files_t *store) {
    if (store->next_record_id == 0) return COMPANION_OK;
    uint8_t encoded[12];
    companion_err_t err = companion_meta_encode(store->next_record_id, encoded);
    if (err != COMPANION_OK) return err;
    return publish_temp(store, COMPANION_FILE_META, COMPANION_FILE_META_NEW, encoded, sizeof encoded);
}

static companion_err_t write_u32_local(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
    return COMPANION_OK;
}

static uint32_t read_u32_local(const uint8_t *src) {
    return (uint32_t)src[0]
        | ((uint32_t)src[1] << 8)
        | ((uint32_t)src[2] << 16)
        | ((uint32_t)src[3] << 24);
}

static companion_err_t save_record_commit(companion_files_t *store, uint32_t length) {
    uint8_t encoded[12];
    uint8_t raw[4];
    write_u32_local(raw, length);
    write_u32_local(encoded, RECORD_COMMIT_MAGIC);
    write_u32_local(encoded + 4, length);
    write_u32_local(encoded + 8, companion_crc32(raw, sizeof raw));
    char path[192];
    companion_err_t err = make_path(store, COMPANION_FILE_RECORD_COMMIT, path, sizeof path);
    if (err != COMPANION_OK) return err;
    return write_temp_file(path, encoded, sizeof encoded);
}

static bool record_commit_ok(companion_files_t *store, uint32_t *length_out) {
    char path[192];
    if (make_path(store, COMPANION_FILE_RECORD_COMMIT, path, sizeof path) != COMPANION_OK) return false;
    uint8_t *bytes = NULL;
    size_t len = 0;
    if (read_capped(path, &bytes, &len, 12) != COMPANION_OK || len != 12) {
        free(bytes);
        return false;
    }
    uint8_t raw[4];
    uint32_t length = read_u32_local(bytes + 4);
    write_u32_local(raw, length);
    bool ok = read_u32_local(bytes) == RECORD_COMMIT_MAGIC
        && companion_crc32(raw, sizeof raw) == read_u32_local(bytes + 8);
    free(bytes);
    if (!ok) return false;
    *length_out = length;
    return true;
}

static companion_err_t file_size(const char *path, bool *exists, uint32_t *size_out) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT) {
            *exists = false;
            return COMPANION_OK;
        }
        return COMPANION_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    long size = ftell(file);
    fclose(file);
    if (size < 0 || (unsigned long)size > UINT32_MAX) return COMPANION_ERR_FORMAT;
    *exists = true;
    *size_out = (uint32_t)size;
    return COMPANION_OK;
}

/* A committed RECORDS.NEW replaces RECORDS.DAT. An uncommitted temp is discarded. */
static companion_err_t recover_records(companion_files_t *store) {
    char dat_path[192];
    char new_path[192];
    char commit_path[192];
    companion_err_t err = make_path(store, COMPANION_FILE_RECORDS, dat_path, sizeof dat_path);
    if (err != COMPANION_OK) return err;
    err = make_path(store, COMPANION_FILE_RECORDS_NEW, new_path, sizeof new_path);
    if (err != COMPANION_OK) return err;
    err = make_path(store, COMPANION_FILE_RECORD_COMMIT, commit_path, sizeof commit_path);
    if (err != COMPANION_OK) return err;

    uint32_t committed = 0;
    bool new_exists = false;
    uint32_t new_size = 0;
    err = file_size(new_path, &new_exists, &new_size);
    if (err != COMPANION_OK) return err;
    if (record_commit_ok(store, &committed) && new_exists && new_size == committed) {
        remove_file(dat_path);
        if (rename(new_path, dat_path) != 0) return COMPANION_ERR_IO;
        remove_file(commit_path);
        sync_dir(store->dir);
        return COMPANION_OK;
    }
    remove_file(new_path);
    remove_file(commit_path);
    return COMPANION_OK;
}

static companion_err_t stream_crc(FILE *file, uint32_t offset, uint16_t text_len, uint32_t *crc_out,
                                 const uint8_t prefix[18]) {
    uint32_t crc = companion_crc32_feed(companion_crc32_start(), prefix, 18);
    uint8_t chunk[256];
    uint32_t left = text_len;
    uint32_t pos = offset + COMPANION_RECORD_HEADER_BYTES;
    while (left > 0) {
        uint32_t step = left > sizeof chunk ? (uint32_t)sizeof chunk : left;
        companion_err_t err = read_at(file, pos, chunk, step);
        if (err != COMPANION_OK) return err;
        crc = companion_crc32_feed(crc, chunk, step);
        pos += step;
        left -= step;
    }
    *crc_out = companion_crc32_finish(crc);
    return COMPANION_OK;
}

/* visit > 0 stops successfully, visit < 0 aborts with -error, visit 0 continues. */
static companion_err_t walk_records(companion_files_t *store, bool repair,
                                   int (*visit)(void *ctx, FILE *file, const record_loc_t *loc),
                                   void *ctx) {
    char path[192];
    companion_err_t err = make_path(store, COMPANION_FILE_RECORDS, path, sizeof path);
    if (err != COMPANION_OK) return err;
    FILE *file = fopen(path, "r+b");
    if (!file) {
        if (errno == ENOENT) {
            if (repair) {
                store->records_end = 0;
                store->records_live_bytes = 0;
            }
            return COMPANION_OK;
        }
        return COMPANION_ERR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    long raw_size = ftell(file);
    if (raw_size < 0 || (unsigned long)raw_size > UINT32_MAX) {
        fclose(file);
        return COMPANION_ERR_FORMAT;
    }
    uint32_t size = (uint32_t)raw_size;
    uint32_t offset = 0;
    uint32_t live_bytes = 0;
    uint32_t max_id = 0;
    bool stopped = false;
    bool tail_bad = false;

    while (offset + COMPANION_RECORD_HEADER_BYTES <= size) {
        uint8_t header[COMPANION_RECORD_HEADER_BYTES];
        err = read_at(file, offset, header, sizeof header);
        if (err != COMPANION_OK) {
            fclose(file);
            return err;
        }
        uint16_t text_len = companion_record_text_len(header);
        uint32_t span = COMPANION_RECORD_HEADER_BYTES + (uint32_t)text_len;
        if (!companion_record_magic_ok(header) || text_len > COMPANION_RECORD_TEXT_MAX
            || offset > UINT32_MAX - span || offset + span > size) {
            tail_bad = true;
            break;
        }
        uint32_t id = companion_record_id(header);
        uint8_t prefix[18];
        companion_record_prefix(prefix, id, companion_record_category_id(header),
                                companion_record_created(header), text_len);
        uint32_t actual_crc = 0;
        err = stream_crc(file, offset, text_len, &actual_crc, prefix);
        if (err != COMPANION_OK) {
            fclose(file);
            return err;
        }
        if (id == 0 || actual_crc != companion_record_stored_crc(header)) {
            offset += span;
            continue;
        }
        record_loc_t loc = {
            .offset = offset,
            .id = id,
            .category_id = companion_record_category_id(header),
            .created_unix = companion_record_created(header),
            .text_len = text_len,
        };
        if (companion_record_is_live(header)) {
            live_bytes += span;
            if (visit) {
                int verdict = visit(ctx, file, &loc);
                if (verdict > 0) {
                    stopped = true;
                    offset += span;
                    break;
                }
                if (verdict < 0) {
                    fclose(file);
                    return (companion_err_t)(-verdict);
                }
            }
        }
        if (id > max_id) max_id = id;
        offset += span;
    }
    if (offset < size) tail_bad = true;

    if (repair && !stopped && tail_bad) {
        if (fflush(file) != 0 || ftruncate(fileno(file), (off_t)offset) != 0 || fsync(fileno(file)) != 0) {
            fclose(file);
            return COMPANION_ERR_IO;
        }
    }
    fclose(file);
    if (repair && !stopped) {
        store->records_end = offset;
        store->records_live_bytes = live_bytes;
        if (max_id >= store->next_record_id) {
            if (max_id == UINT32_MAX) return COMPANION_ERR_FULL;
            store->next_record_id = max_id + 1;
        }
    }
    return COMPANION_OK;
}

static int find_active_slot(const companion_category_table_t *table, uint32_t id) {
    for (int i = 0; i < (int)COMPANION_CATEGORY_MAX; i++) {
        if (table->slots[i].active && table->slots[i].id == id) return i;
    }
    return -1;
}

static int find_name_slot(const companion_category_table_t *table, const char *name, size_t len) {
    for (int i = 0; i < (int)COMPANION_CATEGORY_MAX; i++) {
        if (!table->slots[i].active || table->slots[i].name_len != len) continue;
        if (memcmp(table->slots[i].name, name, len) == 0) return i;
    }
    return -1;
}

static const char *category_name(const companion_files_t *store, uint32_t id) {
    int slot = find_active_slot(&store->categories, id);
    if (slot < 0) return NULL;
    return store->categories.slots[slot].name;
}

static void fill_category(const companion_category_slot_t *slot, companion_category_t *out) {
    out->id = slot->id;
    memcpy(out->name, slot->name, slot->name_len);
    out->name[slot->name_len] = '\0';
}

typedef struct {
    uint32_t id;
    bool found;
    record_loc_t loc;
} find_ctx_t;

static int find_visit(void *ctx, FILE *file, const record_loc_t *loc) {
    (void)file;
    find_ctx_t *find = ctx;
    if (loc->id != find->id) return 0;
    find->found = true;
    find->loc = *loc;
    return 1;
}

static companion_err_t find_live(companion_files_t *store, uint32_t id, record_loc_t *loc) {
    find_ctx_t find = {.id = id};
    companion_err_t err = walk_records(store, false, find_visit, &find);
    if (err != COMPANION_OK) return err;
    if (!find.found) return COMPANION_ERR_NOT_FOUND;
    *loc = find.loc;
    return COMPANION_OK;
}

typedef struct {
    uint32_t category_id;
    size_t count;
} count_ctx_t;

static int count_visit(void *ctx, FILE *file, const record_loc_t *loc) {
    (void)file;
    count_ctx_t *count = ctx;
    if (count->category_id == 0 || loc->category_id == count->category_id) count->count++;
    return 0;
}

typedef struct {
    uint32_t category_id;
    size_t target;
    size_t seen;
    bool found;
    record_loc_t loc;
} at_ctx_t;

static int at_visit(void *ctx, FILE *file, const record_loc_t *loc) {
    (void)file;
    at_ctx_t *at = ctx;
    if (at->category_id != 0 && loc->category_id != at->category_id) return 0;
    if (at->seen == at->target) {
        at->found = true;
        at->loc = *loc;
        return 1;
    }
    at->seen++;
    return 0;
}

static companion_err_t copy_text(FILE *file, const record_loc_t *loc, char *text, size_t text_cap,
                                size_t *out_text_len) {
    if (out_text_len) *out_text_len = loc->text_len;
    if (!text) return COMPANION_OK;
    if (text_cap <= loc->text_len) return COMPANION_ERR_TOO_BIG;
    companion_err_t err = read_at(file, loc->offset + COMPANION_RECORD_HEADER_BYTES, text, loc->text_len);
    if (err != COMPANION_OK) return err;
    text[loc->text_len] = '\0';
    return COMPANION_OK;
}

typedef struct {
    FILE *out;
    uint32_t written;
    companion_err_t error;
} compact_ctx_t;

static int compact_visit(void *ctx, FILE *file, const record_loc_t *loc) {
    compact_ctx_t *compact = ctx;
    uint32_t span = COMPANION_RECORD_HEADER_BYTES + (uint32_t)loc->text_len;
    if (compact->written > UINT32_MAX - span) {
        compact->error = COMPANION_ERR_FULL;
        return -(int)COMPANION_ERR_FULL;
    }
    uint8_t chunk[256];
    uint32_t left = span;
    uint32_t pos = loc->offset;
    while (left > 0) {
        uint32_t step = left > sizeof chunk ? (uint32_t)sizeof chunk : left;
        if (read_at(file, pos, chunk, step) != COMPANION_OK
            || fwrite(chunk, 1, step, compact->out) != step) {
            compact->error = COMPANION_ERR_IO;
            return -(int)COMPANION_ERR_IO;
        }
        pos += step;
        left -= step;
    }
    compact->written += span;
    return 0;
}

companion_err_t companion_files_compact(companion_files_t *store) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (store->records_live_bytes == store->records_end) return COMPANION_OK;

    char new_path[192];
    char dat_path[192];
    err = make_path(store, COMPANION_FILE_RECORDS_NEW, new_path, sizeof new_path);
    if (err != COMPANION_OK) return err;
    err = make_path(store, COMPANION_FILE_RECORDS, dat_path, sizeof dat_path);
    if (err != COMPANION_OK) return err;

    FILE *out = fopen(new_path, "wb");
    if (!out) return COMPANION_ERR_IO;
    compact_ctx_t compact = {.out = out};
    err = walk_records(store, false, compact_visit, &compact);
    if (err != COMPANION_OK || compact.error != COMPANION_OK) {
        fclose(out);
        remove_file(new_path);
        return err != COMPANION_OK ? err : compact.error;
    }
    if (sync_file(out) != COMPANION_OK) {
        fclose(out);
        remove_file(new_path);
        return COMPANION_ERR_IO;
    }
    if (fseek(out, 0, SEEK_END) != 0) {
        fclose(out);
        remove_file(new_path);
        return COMPANION_ERR_IO;
    }
    long new_size = ftell(out);
    fclose(out);
    if (new_size < 0 || (uint32_t)new_size != compact.written) {
        remove_file(new_path);
        return COMPANION_ERR_IO;
    }
    err = save_record_commit(store, compact.written);
    if (err != COMPANION_OK) {
        remove_file(new_path);
        return err;
    }
    remove_file(dat_path);
    if (rename(new_path, dat_path) != 0) {
        if (recover_records(store) != COMPANION_OK) return COMPANION_ERR_IO;
    } else {
        char commit_path[192];
        if (make_path(store, COMPANION_FILE_RECORD_COMMIT, commit_path, sizeof commit_path) == COMPANION_OK) {
            remove_file(commit_path);
        }
        sync_dir(store->dir);
    }
    bool exists = false;
    uint32_t size = 0;
    err = file_size(dat_path, &exists, &size);
    if (err != COMPANION_OK || !exists || size != compact.written) {
        (void)walk_records(store, true, NULL, NULL);
        return COMPANION_ERR_IO;
    }
    store->records_end = compact.written;
    store->records_live_bytes = compact.written;
    return COMPANION_OK;
}

companion_files_t *companion_files_alloc(void) {
    return calloc(1, sizeof(struct companion_files));
}

void companion_files_close(companion_files_t *store) {
    if (!store) return;
    store->open = false;
}

void companion_files_free(companion_files_t *store) {
    if (!store) return;
    companion_files_close(store);
    free(store);
}

companion_err_t companion_files_open(companion_files_t *store, const char *dir, uint32_t records_limit) {
    if (!store || !dir || dir[0] == '\0' || records_limit == 0) return COMPANION_ERR_ARG;
    if (store->open) return COMPANION_ERR_BUSY;
    size_t dir_len = strlen(dir);
    if (dir_len >= sizeof store->dir) return COMPANION_ERR_ARG;
    memset(store, 0, sizeof(*store));
    memcpy(store->dir, dir, dir_len + 1);
    if (dir_len > 1 && store->dir[dir_len - 1] == '/') store->dir[dir_len - 1] = '\0';
    store->records_limit = records_limit;
    store->next_record_id = 1;

    companion_err_t err = recover_records(store);
    if (err != COMPANION_OK) return err;
    bool meta_found = false;
    err = load_meta(store, &meta_found);
    if (err != COMPANION_OK) return err;
    err = load_categories(store);
    if (err != COMPANION_OK) return err;
    uint32_t before = store->next_record_id;
    err = walk_records(store, true, NULL, NULL);
    if (err != COMPANION_OK) return err;
    if (!meta_found || store->next_record_id != before) {
        companion_err_t meta_err = save_meta(store);
        if (meta_err != COMPANION_OK) return meta_err;
    }
    store->open = true;
    return COMPANION_OK;
}

static companion_err_t persona_from(const uint8_t *bytes, size_t len, char *buf, size_t buf_len,
                                   size_t *out_len) {
    size_t text_len = 0;
    companion_err_t err = companion_persona_decode(bytes, len, NULL, 0, &text_len);
    if (err != COMPANION_OK) return err;
    if (out_len) *out_len = text_len;
    if (!buf) return COMPANION_OK;
    return companion_persona_decode(bytes, len, buf, buf_len, &text_len);
}

companion_err_t companion_files_persona_get(companion_files_t *store, char *buf, size_t buf_len,
                                           size_t *out_len) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (!out_len) return COMPANION_ERR_ARG;

    char final_path[192];
    char temp_path[192];
    err = make_path(store, COMPANION_FILE_PERSONA, final_path, sizeof final_path);
    if (err != COMPANION_OK) return err;
    err = make_path(store, COMPANION_FILE_PERSONA_NEW, temp_path, sizeof temp_path);
    if (err != COMPANION_OK) return err;

    uint8_t *bytes = NULL;
    size_t len = 0;
    err = read_capped(temp_path, &bytes, &len, COMPANION_PERSONA_MAX + 12u);
    if (err == COMPANION_OK) {
        size_t text_len = 0;
        if (companion_persona_decode(bytes, len, NULL, 0, &text_len) == COMPANION_OK) {
            companion_err_t copy_err = persona_from(bytes, len, buf, buf_len, out_len);
            free(bytes);
            remove_file(final_path);
            if (rename(temp_path, final_path) != 0) sync_dir(store->dir);
            return copy_err;
        }
        free(bytes);
        bytes = NULL;
        remove_file(temp_path);
    } else if (err != COMPANION_ERR_NOT_FOUND) {
        return err;
    }

    err = read_capped(final_path, &bytes, &len, COMPANION_PERSONA_MAX + 12u);
    if (err == COMPANION_ERR_NOT_FOUND) {
        *out_len = 0;
        if (buf && buf_len > 0) buf[0] = '\0';
        if (buf && buf_len == 0) return COMPANION_ERR_TOO_BIG;
        return COMPANION_OK;
    }
    if (err != COMPANION_OK) return err;
    err = persona_from(bytes, len, buf, buf_len, out_len);
    free(bytes);
    return err;
}

companion_err_t companion_files_persona_set(companion_files_t *store, const char *text, size_t len) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (len > COMPANION_PERSONA_MAX) return COMPANION_ERR_TOO_BIG;
    uint8_t *encoded = malloc(12u + len);
    if (!encoded) return COMPANION_ERR_NO_MEM;
    size_t encoded_len = 0;
    err = companion_persona_encode(text, len, encoded, 12u + len, &encoded_len);
    if (err == COMPANION_OK) {
        err = publish_temp(store, COMPANION_FILE_PERSONA, COMPANION_FILE_PERSONA_NEW, encoded, encoded_len);
    }
    free(encoded);
    return err;
}

companion_err_t companion_files_category_create(companion_files_t *store, const char *name, size_t len,
                                               uint32_t *out_id) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (!out_id) return COMPANION_ERR_ARG;
    err = companion_utf8_check(name, len, false, false);
    if (err != COMPANION_OK) return err;
    if (len > COMPANION_CATEGORY_NAME_MAX) return COMPANION_ERR_TOO_BIG;

    companion_category_table_t *next = malloc(sizeof(*next));
    if (!next) return COMPANION_ERR_NO_MEM;
    *next = store->categories;
    if (find_name_slot(next, name, len) >= 0) {
        free(next);
        return COMPANION_ERR_EXISTS;
    }
    int slot = -1;
    for (int i = 0; i < (int)COMPANION_CATEGORY_MAX; i++) {
        if (!next->slots[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0 || next->next_id == 0 || next->next_id == UINT32_MAX) {
        free(next);
        return COMPANION_ERR_FULL;
    }
    uint32_t id = next->next_id++;
    next->slots[slot].id = id;
    next->slots[slot].active = 1;
    next->slots[slot].name_len = (uint8_t)len;
    memset(next->slots[slot].name, 0, sizeof next->slots[slot].name);
    memcpy(next->slots[slot].name, name, len);
    err = save_categories(store, next);
    free(next);
    if (err == COMPANION_OK) *out_id = id;
    return err;
}

companion_err_t companion_files_category_rename(companion_files_t *store, uint32_t id,
                                               const char *name, size_t len) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    err = companion_utf8_check(name, len, false, false);
    if (err != COMPANION_OK) return err;
    if (len > COMPANION_CATEGORY_NAME_MAX) return COMPANION_ERR_TOO_BIG;

    companion_category_table_t *next = malloc(sizeof(*next));
    if (!next) return COMPANION_ERR_NO_MEM;
    *next = store->categories;
    int slot = find_active_slot(next, id);
    if (slot < 0) {
        free(next);
        return COMPANION_ERR_NOT_FOUND;
    }
    int named = find_name_slot(next, name, len);
    if (named >= 0 && named != slot) {
        free(next);
        return COMPANION_ERR_EXISTS;
    }
    next->slots[slot].name_len = (uint8_t)len;
    memset(next->slots[slot].name, 0, sizeof next->slots[slot].name);
    memcpy(next->slots[slot].name, name, len);
    err = save_categories(store, next);
    free(next);
    return err;
}

companion_err_t companion_files_category_delete(companion_files_t *store, uint32_t id) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (find_active_slot(&store->categories, id) < 0) return COMPANION_ERR_NOT_FOUND;
    size_t count = 0;
    err = companion_files_record_count(store, id, &count);
    if (err != COMPANION_OK) return err;
    if (count > 0) return COMPANION_ERR_BUSY;

    companion_category_table_t *next = malloc(sizeof(*next));
    if (!next) return COMPANION_ERR_NO_MEM;
    *next = store->categories;
    int slot = find_active_slot(next, id);
    memset(&next->slots[slot], 0, sizeof next->slots[slot]);
    err = save_categories(store, next);
    free(next);
    return err;
}

companion_err_t companion_files_category_count(companion_files_t *store, size_t *out_count) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (!out_count) return COMPANION_ERR_ARG;
    size_t count = 0;
    for (int i = 0; i < (int)COMPANION_CATEGORY_MAX; i++) {
        if (store->categories.slots[i].active) count++;
    }
    *out_count = count;
    return COMPANION_OK;
}

companion_err_t companion_files_category_at(companion_files_t *store, size_t index, companion_category_t *out) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (!out) return COMPANION_ERR_ARG;
    size_t seen = 0;
    for (int i = 0; i < (int)COMPANION_CATEGORY_MAX; i++) {
        if (!store->categories.slots[i].active) continue;
        if (seen == index) {
            fill_category(&store->categories.slots[i], out);
            return COMPANION_OK;
        }
        seen++;
    }
    return COMPANION_ERR_NOT_FOUND;
}

companion_err_t companion_files_category_find(companion_files_t *store, uint32_t id, companion_category_t *out) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (!out) return COMPANION_ERR_ARG;
    int slot = find_active_slot(&store->categories, id);
    if (slot < 0) return COMPANION_ERR_NOT_FOUND;
    fill_category(&store->categories.slots[slot], out);
    return COMPANION_OK;
}

companion_err_t companion_files_record_append(companion_files_t *store, uint32_t category_id,
                                             uint32_t created_unix, const char *text, size_t text_len,
                                             uint32_t *out_id) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (!out_id || text_len > UINT16_MAX) return COMPANION_ERR_ARG;
    if (text_len == 0 || text_len > COMPANION_RECORD_TEXT_MAX) return COMPANION_ERR_TOO_BIG;
    err = companion_utf8_check(text, text_len, false, true);
    if (err != COMPANION_OK) return err;
    if (find_active_slot(&store->categories, category_id) < 0) return COMPANION_ERR_NOT_FOUND;
    if (store->next_record_id == 0) return COMPANION_ERR_FULL;

    uint32_t need = COMPANION_RECORD_HEADER_BYTES + (uint32_t)text_len;
    if (need > store->records_limit) return COMPANION_ERR_FULL;
    if (store->records_end > store->records_limit - need) {
        err = walk_records(store, true, NULL, NULL);
        if (err != COMPANION_OK) return err;
        if (store->records_end > store->records_limit - need
            && store->records_live_bytes <= store->records_limit - need
            && store->records_live_bytes < store->records_end) {
            err = companion_files_compact(store);
            if (err != COMPANION_OK) return err;
        }
        if (store->records_end > store->records_limit - need) return COMPANION_ERR_FULL;
    }

    uint32_t id = store->next_record_id;
    uint8_t header[COMPANION_RECORD_HEADER_BYTES];
    err = companion_record_encode_header(header, id, category_id, created_unix, (uint16_t)text_len,
                                        0, 0, (const uint8_t *)text);
    if (err != COMPANION_OK) return err;

    char path[192];
    err = make_path(store, COMPANION_FILE_RECORDS, path, sizeof path);
    if (err != COMPANION_OK) return err;
    FILE *file = fopen(path, "r+b");
    if (!file && errno == ENOENT && store->records_end == 0) file = fopen(path, "w+b");
    if (!file) return COMPANION_ERR_IO;
    if (fseek(file, (long)store->records_end, SEEK_SET) != 0) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    bool wrote = fwrite(header, 1, sizeof header, file) == sizeof header
        && fwrite(text, 1, text_len, file) == text_len;
    if (!wrote || sync_file(file) != COMPANION_OK) {
        int io_err = errno;
        (void)fflush(file);
        (void)ftruncate(fileno(file), (off_t)store->records_end);
        (void)fsync(fileno(file));
        fclose(file);
        return io_err == ENOSPC ? COMPANION_ERR_FULL : COMPANION_ERR_IO;
    }
    fclose(file);

    store->records_end += need;
    store->records_live_bytes += need;
    store->next_record_id = id == UINT32_MAX ? 0 : id + 1;
    (void)save_meta(store);
    *out_id = id;
    return COMPANION_OK;
}

companion_err_t companion_files_record_delete(companion_files_t *store, uint32_t id) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    record_loc_t loc;
    err = find_live(store, id, &loc);
    if (err != COMPANION_OK) return err;

    char path[192];
    err = make_path(store, COMPANION_FILE_RECORDS, path, sizeof path);
    if (err != COMPANION_OK) return err;
    FILE *file = fopen(path, "r+b");
    if (!file) return COMPANION_ERR_IO;
    if (fseek(file, (long)loc.offset + 18, SEEK_SET) != 0) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    if (fputc(1, file) == EOF || sync_file(file) != COMPANION_OK) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    if (fputc(1, file) == EOF || sync_file(file) != COMPANION_OK) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    fclose(file);
    uint32_t span = COMPANION_RECORD_HEADER_BYTES + (uint32_t)loc.text_len;
    if (store->records_live_bytes >= span) store->records_live_bytes -= span;
    else store->records_live_bytes = 0;
    return COMPANION_OK;
}

companion_err_t companion_files_record_set_category(companion_files_t *store, uint32_t id,
                                                   uint32_t category_id) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (find_active_slot(&store->categories, category_id) < 0) return COMPANION_ERR_NOT_FOUND;
    record_loc_t loc;
    err = find_live(store, id, &loc);
    if (err != COMPANION_OK) return err;
    if (loc.category_id == category_id) return COMPANION_OK;

    uint8_t *text = malloc(loc.text_len ? loc.text_len : 1);
    if (!text) return COMPANION_ERR_NO_MEM;
    char path[192];
    err = make_path(store, COMPANION_FILE_RECORDS, path, sizeof path);
    if (err != COMPANION_OK) {
        free(text);
        return err;
    }
    FILE *file = fopen(path, "r+b");
    if (!file) {
        free(text);
        return COMPANION_ERR_IO;
    }
    err = read_at(file, loc.offset + COMPANION_RECORD_HEADER_BYTES, text, loc.text_len);
    if (err != COMPANION_OK) {
        fclose(file);
        free(text);
        return err;
    }
    uint8_t header[COMPANION_RECORD_HEADER_BYTES];
    err = companion_record_encode_header(header, loc.id, category_id, loc.created_unix, loc.text_len,
                                        0, 0, text);
    free(text);
    if (err != COMPANION_OK) {
        fclose(file);
        return err;
    }
    if (fseek(file, (long)loc.offset, SEEK_SET) != 0
        || fwrite(header, 1, sizeof header, file) != sizeof header
        || sync_file(file) != COMPANION_OK) {
        fclose(file);
        return COMPANION_ERR_IO;
    }
    fclose(file);
    return COMPANION_OK;
}

companion_err_t companion_files_record_count(companion_files_t *store, uint32_t category_id, size_t *out_count) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (!out_count) return COMPANION_ERR_ARG;
    count_ctx_t count = {.category_id = category_id};
    err = walk_records(store, false, count_visit, &count);
    if (err != COMPANION_OK) return err;
    *out_count = count.count;
    return COMPANION_OK;
}

companion_err_t companion_files_record_at(companion_files_t *store, uint32_t category_id, size_t index,
                                         companion_record_info_t *info, char *text, size_t text_cap,
                                         size_t *out_text_len) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (!info) return COMPANION_ERR_ARG;
    at_ctx_t at = {.category_id = category_id, .target = index};
    err = walk_records(store, false, at_visit, &at);
    if (err != COMPANION_OK) return err;
    if (!at.found) return COMPANION_ERR_NOT_FOUND;

    char path[192];
    err = make_path(store, COMPANION_FILE_RECORDS, path, sizeof path);
    if (err != COMPANION_OK) return err;
    FILE *file = fopen(path, "rb");
    if (!file) return COMPANION_ERR_IO;
    err = copy_text(file, &at.loc, text, text_cap, out_text_len);
    fclose(file);
    if (err != COMPANION_OK && err != COMPANION_ERR_TOO_BIG) return err;
    info->id = at.loc.id;
    info->category_id = at.loc.category_id;
    info->created_unix = at.loc.created_unix;
    info->text_len = at.loc.text_len;
    return err;
}

typedef struct {
    companion_files_t *store;
    companion_export_write_fn write;
    void *write_ctx;
} export_ctx_t;

static companion_err_t emit(export_ctx_t *export, const void *data, size_t len) {
    if (len == 0) return COMPANION_OK;
    return export->write(export->write_ctx, data, len);
}

static int export_visit(void *ctx, FILE *file, const record_loc_t *loc) {
    export_ctx_t *export = ctx;
    char when[20];
    companion_format_unix_utc(loc->created_unix, when);
    const char *name = category_name(export->store, loc->category_id);
    if (!name) name = "(missing)";
    char line[96];
    int wrote = snprintf(line, sizeof line, "[%s] %s\n", when, name);
    if (wrote < 0 || (size_t)wrote >= sizeof line) return -(int)COMPANION_ERR_IO;
    if (emit(export, line, (size_t)wrote) != COMPANION_OK) return -(int)COMPANION_ERR_IO;

    uint8_t chunk[256];
    uint32_t left = loc->text_len;
    uint32_t pos = loc->offset + COMPANION_RECORD_HEADER_BYTES;
    uint8_t last = 0;
    while (left > 0) {
        uint32_t step = left > sizeof chunk ? (uint32_t)sizeof chunk : left;
        if (read_at(file, pos, chunk, step) != COMPANION_OK) return -(int)COMPANION_ERR_IO;
        if (emit(export, chunk, step) != COMPANION_OK) return -(int)COMPANION_ERR_IO;
        last = chunk[step - 1];
        pos += step;
        left -= step;
    }
    if (loc->text_len == 0 || last != '\n') {
        if (emit(export, "\n", 1) != COMPANION_OK) return -(int)COMPANION_ERR_IO;
    }
    if (emit(export, "\n", 1) != COMPANION_OK) return -(int)COMPANION_ERR_IO;
    return 0;
}

companion_err_t companion_files_export_txt(companion_files_t *store, companion_export_write_fn write, void *ctx) {
    companion_err_t err = require_open(store);
    if (err != COMPANION_OK) return err;
    if (!write) return COMPANION_ERR_ARG;
    export_ctx_t export = {.store = store, .write = write, .write_ctx = ctx};
    static const uint8_t bom[3] = {0xEF, 0xBB, 0xBF};
    err = emit(&export, bom, sizeof bom);
    if (err != COMPANION_OK) return err;
    err = emit(&export, "# Categories\n", 13);
    if (err != COMPANION_OK) return err;
    for (int i = 0; i < (int)COMPANION_CATEGORY_MAX; i++) {
        if (!store->categories.slots[i].active) continue;
        err = emit(&export, "- ", 2);
        if (err != COMPANION_OK) return err;
        err = emit(&export, store->categories.slots[i].name, store->categories.slots[i].name_len);
        if (err != COMPANION_OK) return err;
        err = emit(&export, "\n", 1);
        if (err != COMPANION_OK) return err;
    }
    err = emit(&export, "\n# Records\n", 11);
    if (err != COMPANION_OK) return err;
    return walk_records(store, false, export_visit, &export);
}

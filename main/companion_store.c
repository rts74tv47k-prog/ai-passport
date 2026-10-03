#include "companion_store.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wear_levelling.h"

#include <stddef.h>

static const char *TAG = "store";
static const char *STORE_PATH = "/store";
static const char *STORE_LABEL = "storage";

static companion_files_t *s_files;
static SemaphoreHandle_t s_lock;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_ready;

static companion_err_t map_esp(esp_err_t err) {
    if (err == ESP_ERR_NOT_FOUND) return COMPANION_ERR_NOT_FOUND;
    if (err == ESP_ERR_NO_MEM) return COMPANION_ERR_NO_MEM;
    if (err == ESP_ERR_INVALID_STATE) return COMPANION_ERR_STATE;
    return COMPANION_ERR_IO;
}

static companion_err_t begin(void) {
    if (!s_lock || !s_ready || !s_files) return COMPANION_ERR_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(8000)) != pdTRUE) return COMPANION_ERR_BUSY;
    return COMPANION_OK;
}

static void end(void) {
    xSemaphoreGive(s_lock);
}

companion_err_t companion_store_mount(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return COMPANION_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return COMPANION_ERR_BUSY;
    if (s_ready) {
        xSemaphoreGive(s_lock);
        return COMPANION_OK;
    }

    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    esp_err_t esp_err = esp_vfs_fat_spiflash_mount_rw_wl(STORE_PATH, STORE_LABEL, &cfg, &s_wl);
    if (esp_err != ESP_OK) {
        ESP_LOGE(TAG, "fat mount failed: %s", esp_err_to_name(esp_err));
        s_wl = WL_INVALID_HANDLE;
        xSemaphoreGive(s_lock);
        return map_esp(esp_err);
    }

    s_files = companion_files_alloc();
    if (!s_files) {
        esp_vfs_fat_spiflash_unmount_rw_wl(STORE_PATH, s_wl);
        s_wl = WL_INVALID_HANDLE;
        xSemaphoreGive(s_lock);
        return COMPANION_ERR_NO_MEM;
    }
    companion_err_t err = companion_files_open(s_files, STORE_PATH, COMPANION_RECORDS_BYTES_LIMIT);
    if (err != COMPANION_OK) {
        ESP_LOGE(TAG, "storage open failed: %d", (int)err);
        companion_files_free(s_files);
        s_files = NULL;
        esp_vfs_fat_spiflash_unmount_rw_wl(STORE_PATH, s_wl);
        s_wl = WL_INVALID_HANDLE;
        xSemaphoreGive(s_lock);
        return err;
    }
    s_ready = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "storage mounted");
    return COMPANION_OK;
}

void companion_store_unmount(void) {
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    if (s_files) {
        companion_files_free(s_files);
        s_files = NULL;
    }
    if (s_wl != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(STORE_PATH, s_wl);
        s_wl = WL_INVALID_HANDLE;
    }
    s_ready = false;
    xSemaphoreGive(s_lock);
}

bool companion_store_ready(void) {
    return s_ready;
}

companion_err_t companion_store_persona_get(char *buf, size_t buf_len, size_t *out_len) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_persona_get(s_files, buf, buf_len, out_len);
    end();
    if (err != COMPANION_OK) ESP_LOGW(TAG, "persona read failed: %d", (int)err);
    return err;
}

companion_err_t companion_store_persona_set(const char *text, size_t len) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_persona_set(s_files, text, len);
    end();
    if (err == COMPANION_OK) ESP_LOGI(TAG, "persona updated bytes=%u", (unsigned)len);
    else ESP_LOGW(TAG, "persona update failed: %d", (int)err);
    return err;
}

companion_err_t companion_store_category_create(const char *name, size_t len, uint32_t *out_id) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_category_create(s_files, name, len, out_id);
    uint32_t id = (err == COMPANION_OK && out_id) ? *out_id : 0;
    end();
    if (err == COMPANION_OK) ESP_LOGI(TAG, "category created id=%u", (unsigned)id);
    else ESP_LOGW(TAG, "category create failed: %d", (int)err);
    return err;
}

companion_err_t companion_store_category_rename(uint32_t id, const char *name, size_t len) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_category_rename(s_files, id, name, len);
    end();
    if (err == COMPANION_OK) ESP_LOGI(TAG, "category renamed id=%u", (unsigned)id);
    else ESP_LOGW(TAG, "category rename failed: %d", (int)err);
    return err;
}

companion_err_t companion_store_category_delete(uint32_t id) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_category_delete(s_files, id);
    end();
    if (err == COMPANION_OK) ESP_LOGI(TAG, "category deleted id=%u", (unsigned)id);
    else ESP_LOGW(TAG, "category delete failed: %d", (int)err);
    return err;
}

companion_err_t companion_store_category_count(size_t *out_count) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_category_count(s_files, out_count);
    end();
    return err;
}

companion_err_t companion_store_category_at(size_t index, companion_category_t *out) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_category_at(s_files, index, out);
    end();
    return err;
}

companion_err_t companion_store_category_find(uint32_t id, companion_category_t *out) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_category_find(s_files, id, out);
    end();
    return err;
}

companion_err_t companion_store_record_append(uint32_t category_id, uint32_t created_unix,
                                             const char *text, size_t text_len, uint32_t *out_id) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_record_append(s_files, category_id, created_unix, text, text_len, out_id);
    uint32_t id = (err == COMPANION_OK && out_id) ? *out_id : 0;
    end();
    if (err == COMPANION_OK) {
        ESP_LOGI(TAG, "record saved id=%u category=%u bytes=%u",
                 (unsigned)id, (unsigned)category_id, (unsigned)text_len);
    } else {
        ESP_LOGW(TAG, "record save failed: %d", (int)err);
    }
    return err;
}

companion_err_t companion_store_record_delete(uint32_t id) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_record_delete(s_files, id);
    end();
    if (err == COMPANION_OK) ESP_LOGI(TAG, "record deleted id=%u", (unsigned)id);
    else ESP_LOGW(TAG, "record delete failed: %d", (int)err);
    return err;
}

companion_err_t companion_store_record_set_category(uint32_t id, uint32_t category_id) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_record_set_category(s_files, id, category_id);
    end();
    if (err == COMPANION_OK) {
        ESP_LOGI(TAG, "record moved id=%u category=%u", (unsigned)id, (unsigned)category_id);
    } else {
        ESP_LOGW(TAG, "record move failed: %d", (int)err);
    }
    return err;
}

companion_err_t companion_store_record_count(uint32_t category_id, size_t *out_count) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_record_count(s_files, category_id, out_count);
    end();
    return err;
}

companion_err_t companion_store_record_at(uint32_t category_id, size_t index, companion_record_info_t *info,
                                         char *text, size_t text_cap, size_t *out_text_len) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_record_at(s_files, category_id, index, info, text, text_cap, out_text_len);
    end();
    return err;
}

companion_err_t companion_store_export_txt(companion_export_write_fn write, void *ctx) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_export_txt(s_files, write, ctx);
    end();
    if (err != COMPANION_OK) ESP_LOGW(TAG, "export failed: %d", (int)err);
    return err;
}

companion_err_t companion_store_usage(uint32_t *live_bytes, uint32_t *limit_bytes) {
    companion_err_t err = begin();
    if (err != COMPANION_OK) return err;
    err = companion_files_usage(s_files, live_bytes, limit_bytes);
    end();
    return err;
}

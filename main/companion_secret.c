#include "companion_secret.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "secret";
static const char *NS = "companion";
static const char *KEY = "apikey";

bool companion_secret_set(const char *key, size_t len) {
    if (!key || len == 0 || len > 128) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c < 0x21 || c > 0x7e) return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %d", (int)err);
        return false;
    }
    err = nvs_set_blob(handle, KEY, key, len);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %d", (int)err);
        return false;
    }
    ESP_LOGI(TAG, "api key stored bytes=%u", (unsigned)len);
    return true;
}

bool companion_secret_get(char *dst, size_t cap, size_t *out_len) {
    if (!dst || cap < 2) return false;
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t n = cap - 1;
    esp_err_t err = nvs_get_blob(handle, KEY, dst, &n);
    nvs_close(handle);
    if (err != ESP_OK || n == 0 || n >= cap) return false;
    dst[n] = '\0';
    if (out_len) *out_len = n;
    return true;
}

bool companion_secret_is_set(void) {
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t n = 0;
    esp_err_t err = nvs_get_blob(handle, KEY, NULL, &n);
    nvs_close(handle);
    return err == ESP_OK && n > 0 && n <= 128;
}

// Station Wi-Fi, NimBLE BLUFI, and a temporary open SoftAP.
// Adapted from the upstream demo/blufi-provisioning snapshot
// 9c039cc5127f22072afa83bedb7fa3d8efe635ad. The demo screen is not used.
// BLE carries credentials only. This file never logs an SSID password.
#include "companion_net.h"

#include "companion_blufi_security.h"
#include "companion_ui.h"
#include "companion_web.h"

#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "lwip/ip4_addr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "net";
static const char *DEVICE_NAME = "BLUFI_FoloPassport";
static const char *AP_SSID = "AI-Passport";

#define AP_LIST_COUNT 8

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static wifi_config_t s_sta_config;
static SemaphoreHandle_t s_mu;
static bool s_wifi_started;
static bool s_ap_on;
static bool s_ble_connected;
static bool s_wifi_connecting;
static bool s_wifi_got_ip;
static bool s_reconnect_after_disconnect;
static bool s_sntp_started;
static volatile bool s_time_synced;
static bool s_drop_posted;
static int s_retries;
static char s_ip[16];

static void send_wifi_report(esp_blufi_sta_conn_state_t state) {
    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    esp_blufi_extra_info_t info = {0};
    size_t ssid_len = strnlen((const char *)s_sta_config.sta.ssid, sizeof(s_sta_config.sta.ssid));
    if (ssid_len > 0) {
        info.sta_ssid = s_sta_config.sta.ssid;
        info.sta_ssid_len = ssid_len;
    }
    esp_blufi_send_wifi_conn_report(mode, state, 0, &info);
}

static void send_wifi_list(void) {
    static wifi_ap_record_t records[AP_LIST_COUNT];
    static esp_blufi_ap_record_t list[AP_LIST_COUNT];
    uint16_t count = AP_LIST_COUNT;
    memset(records, 0, sizeof(records));
    memset(list, 0, sizeof(list));
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }
    if (count > AP_LIST_COUNT) count = AP_LIST_COUNT;
    for (uint16_t i = 0; i < count; i++) {
        list[i].rssi = records[i].rssi;
        memcpy(list[i].ssid, records[i].ssid, sizeof(list[i].ssid));
    }
    if (s_ble_connected) esp_blufi_send_wifi_list(count, list);
}

static void request_wifi_connect(void) {
    bool was_connected = s_wifi_got_ip;
    s_wifi_connecting = true;
    s_wifi_got_ip = false;
    s_retries = 0;
    if (was_connected) {
        s_reconnect_after_disconnect = true;
        if (esp_wifi_disconnect() == ESP_OK) return;
        s_reconnect_after_disconnect = false;
    }
    if (esp_wifi_connect() != ESP_OK) {
        s_wifi_connecting = false;
        ESP_LOGW(TAG, "wifi connect start failed");
    }
}

static void bring_ap_up(void) {
    if (s_ap_on) return;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK) {
        s_ap_on = true;
        ESP_LOGI(TAG, "setup AP started");
    } else {
        ESP_LOGW(TAG, "setup AP start failed");
    }
}

static void drop_ap_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (s_wifi_got_ip && s_ap_on) {
        if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
            s_ap_on = false;
            ESP_LOGI(TAG, "setup AP stopped");
        } else {
            ESP_LOGW(TAG, "setup AP stop failed");
        }
    }
    s_drop_posted = false;
    vTaskDelete(NULL);
}

static void schedule_drop_ap(void) {
    if (!s_ap_on || s_drop_posted) return;
    s_drop_posted = true;
    if (xTaskCreate(drop_ap_task, "apdrop", 3072, NULL, 2, NULL) != pdPASS) {
        s_drop_posted = false;
        ESP_LOGW(TAG, "setup AP stop task failed");
    }
}

static void on_time_sync(struct timeval *tv) {
    (void)tv;
    s_time_synced = true;
    ESP_LOGI(TAG, "time synced");
}

static void start_sntp(void) {
    if (s_sntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "sntp start");
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_get_config(WIFI_IF_STA, &s_sta_config);
        if (s_sta_config.sta.ssid[0] != '\0') {
            s_wifi_connecting = true;
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        unsigned reason = 0;
        if (data) {
            reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        }
        ESP_LOGW(TAG, "wifi disconnected reason=%u", reason);
        if (s_reconnect_after_disconnect) {
            s_reconnect_after_disconnect = false;
            esp_wifi_connect();
            return;
        }
        bool was_trying = s_wifi_connecting;
        s_wifi_got_ip = false;
        s_ip[0] = '\0';
        companion_ui_set_link(false, NULL);
        if (was_trying && s_retries < 3) {
            s_retries++;
            esp_wifi_connect();
            return;
        }
        s_wifi_connecting = false;
        if (s_ble_connected) send_wifi_report(ESP_BLUFI_STA_CONN_FAIL);
        bring_ap_up();
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        send_wifi_list();
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP && data) {
        ip_event_got_ip_t *event = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connecting = false;
        s_wifi_got_ip = true;
        s_retries = 0;
        ESP_LOGI(TAG, "wifi got ip");
        companion_ui_set_link(true, s_ip);
        if (s_ble_connected) send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
        start_sntp();
        schedule_drop_ap();
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_wifi_got_ip = false;
        s_ip[0] = '\0';
        companion_ui_set_link(false, NULL);
    }
}

static void blufi_reset(int reason) {
    ESP_LOGE(TAG, "nimble reset: %d", reason);
}

static void blufi_sync(void) {
    int rc = esp_blufi_profile_init();
    if (rc != 0) ESP_LOGE(TAG, "blufi profile init failed: %d", rc);
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param) {
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        esp_blufi_adv_start_with_name(DEVICE_NAME);
        ESP_LOGI(TAG, "blufi advertising");
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        s_ble_connected = true;
        esp_blufi_adv_stop();
        if (companion_blufi_security_init() != 0) {
            ESP_LOGE(TAG, "blufi security init failed");
        }
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        s_ble_connected = false;
        companion_blufi_security_deinit();
        esp_blufi_adv_start_with_name(DEVICE_NAME);
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        if (s_wifi_got_ip) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_ap_on = false;
        } else {
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            s_ap_on = true;
        }
        break;
    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(s_sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        s_sta_config.sta.bssid_set = true;
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len >= sizeof(s_sta_config.sta.ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(s_sta_config.sta.ssid, 0, sizeof(s_sta_config.sta.ssid));
        memset(s_sta_config.sta.password, 0, sizeof(s_sta_config.sta.password));
        memset(s_sta_config.sta.bssid, 0, sizeof(s_sta_config.sta.bssid));
        s_sta_config.sta.bssid_set = false;
        s_sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        memcpy(s_sta_config.sta.ssid, param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len >= sizeof(s_sta_config.sta.password)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(s_sta_config.sta.password, 0, sizeof(s_sta_config.sta.password));
        memcpy(s_sta_config.sta.password, param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        request_wifi_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        if (s_wifi_got_ip) send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
        else if (s_wifi_connecting) send_wifi_report(ESP_BLUFI_STA_CONNECTING);
        else send_wifi_report(ESP_BLUFI_STA_CONN_FAIL);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
        wifi_scan_config_t config = {0};
        if (esp_wifi_scan_start(&config, false) != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        esp_blufi_send_error_info(param->report_error.state);
        break;
    default:
        break;
    }
}

static esp_blufi_callbacks_t s_callbacks = {
    .event_cb = blufi_event,
    .negotiate_data_handler = companion_blufi_negotiate,
    .encrypt_func = companion_blufi_encrypt,
    .decrypt_func = companion_blufi_decrypt,
    .checksum_func = companion_blufi_checksum,
};

static esp_err_t prepare_platform(void) {
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %d", (int)err);
        return err;
    }
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    return ESP_OK;
}

static esp_err_t wifi_start(void) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;

    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, AP_SSID, strlen(AP_SSID));
    ap.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 2;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err == ESP_OK) {
        s_wifi_started = true;
        s_ap_on = true;
    }
    return err;
}

static esp_err_t host_start(void) {
    esp_err_t err = esp_blufi_register_callbacks(&s_callbacks);
    if (err != ESP_OK) return err;
    err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.reset_cb = blufi_reset;
    ble_hs_cfg.sync_cb = blufi_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    if (esp_blufi_gatt_svr_init() != 0) return ESP_FAIL;
    if (ble_svc_gap_device_name_set(DEVICE_NAME) != 0) return ESP_FAIL;
    esp_blufi_btc_init();
    return esp_nimble_enable(host_task);
}

void companion_net_start(void) {
    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) {
        ESP_LOGE(TAG, "mutex failed");
        return;
    }
    esp_err_t err = prepare_platform();
    if (err == ESP_OK) err = wifi_start();
    if (err == ESP_OK) err = host_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network start failed: %d", (int)err);
        return;
    }
    companion_web_start();
    ESP_LOGI(TAG, "network ready");
}

bool companion_net_set_sta(const char *ssid, size_t ssid_len,
                           const char *password, size_t password_len) {
    if (!s_wifi_started || !s_mu || !ssid || ssid_len == 0 ||
        ssid_len >= sizeof(s_sta_config.sta.ssid) ||
        password_len >= sizeof(s_sta_config.sta.password)) {
        return false;
    }
    if (password_len > 0 && !password) return false;
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    if (password_len > 0) memcpy(cfg.sta.password, password, password_len);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(2000)) != pdTRUE) {
        memset(cfg.sta.password, 0, sizeof(cfg.sta.password));
        return false;
    }
    s_sta_config = cfg;
    xSemaphoreGive(s_mu);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(cfg.sta.password, 0, sizeof(cfg.sta.password));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi config failed: %d", (int)err);
        return false;
    }
    ESP_LOGI(TAG, "wifi config updated");
    request_wifi_connect();
    return true;
}

void companion_net_forget(void) {
    if (!s_wifi_started) return;
    wifi_config_t empty = {0};
    s_reconnect_after_disconnect = false;
    s_wifi_connecting = false;
    s_wifi_got_ip = false;
    s_ip[0] = '\0';
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    if (s_mu && xSemaphoreTake(s_mu, pdMS_TO_TICKS(2000)) == pdTRUE) {
        memset(&s_sta_config, 0, sizeof(s_sta_config));
        xSemaphoreGive(s_mu);
    }
    companion_ui_set_link(false, NULL);
    bring_ap_up();
    ESP_LOGI(TAG, "wifi forgotten");
}

uint32_t companion_net_unix_time(void) {
    if (!s_time_synced) return 0;
    time_t now = time(NULL);
    if (now < 1700000000) return 0;
    return (uint32_t)now;
}

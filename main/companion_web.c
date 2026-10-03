// LAN page on port 80. One request at a time. Bodies stay in static buffers
// of at most 8KB. The API key and Wi-Fi password are never logged or echoed.
#include "companion_web.h"

#include "companion_nav.h"
#include "companion_net.h"
#include "companion_secret.h"
#include "companion_store.h"
#include "companion_text.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web";

static httpd_handle_t s_httpd;
static SemaphoreHandle_t s_mu;
static char s_body[8192];
static char s_field[4096];
static char s_export[4096];
static size_t s_body_len;

_Static_assert(sizeof(s_body) <= 8192, "http body exceeds 8KB");
_Static_assert(sizeof(s_field) <= 8192, "http field exceeds 8KB");
_Static_assert(sizeof(s_export) <= 8192, "export buffer exceeds 8KB");

static bool chunk(httpd_req_t *req, const char *text) {
    if (!text) return false;
    return httpd_resp_send_chunk(req, text, strlen(text)) == ESP_OK;
}

static bool chunk_escape(httpd_req_t *req, const char *src, size_t len) {
    char buf[96];
    size_t used = 0;
    for (size_t i = 0; i < len; i++) {
        const char *rep = NULL;
        size_t n = 1;
        char one = src[i];
        if (src[i] == '&') {
            rep = "&amp;";
            n = 5;
        } else if (src[i] == '<') {
            rep = "&lt;";
            n = 4;
        } else if (src[i] == '>') {
            rep = "&gt;";
            n = 4;
        } else if (src[i] == '"') {
            rep = "&quot;";
            n = 6;
        }
        if (used + n >= sizeof(buf)) {
            if (httpd_resp_send_chunk(req, buf, used) != ESP_OK) return false;
            used = 0;
        }
        if (rep) memcpy(buf + used, rep, n);
        else buf[used] = one;
        used += n;
    }
    if (used > 0 && httpd_resp_send_chunk(req, buf, used) != ESP_OK) return false;
    return true;
}

static esp_err_t redirect(httpd_req_t *req, int code) {
    char loc[16];
    snprintf(loc, sizeof(loc), "/?e=%d", code);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t read_body(httpd_req_t *req) {
    if (req->content_len > sizeof(s_body)) {
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, NULL);
    }
    size_t got = 0;
    size_t need = req->content_len;
    int spins = 0;
    while (got < need) {
        int n = httpd_req_recv(req, s_body + got, need - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++spins > 5) return ESP_FAIL;
            continue;
        }
        if (n <= 0) return ESP_FAIL;
        got += (size_t)n;
        spins = 0;
    }
    s_body_len = got;
    return ESP_OK;
}

static bool take_field(const char *key, size_t cap, size_t *out_len) {
    if (cap > sizeof(s_field)) cap = sizeof(s_field);
    return companion_form_get(s_body, s_body_len, key, s_field, cap, out_len);
}

static int query_code(httpd_req_t *req) {
    char query[32];
    char value[8];
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= sizeof(query)) return 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return 0;
    if (httpd_query_key_value(query, "e", value, sizeof(value)) != ESP_OK) return 0;
    char *end = NULL;
    long code = strtol(value, &end, 10);
    if (!end || *end != '\0' || code < 0 || code > 100) return 0;
    return (int)code;
}

static int query_page(httpd_req_t *req) {
    char query[32];
    char value[8];
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= sizeof(query)) return 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return 0;
    if (httpd_query_key_value(query, "page", value, sizeof(value)) != ESP_OK) return 0;
    char *end = NULL;
    long page = strtol(value, &end, 10);
    if (!end || *end != '\0' || page < 0 || page > 100000) return 0;
    return (int)page;
}

static const char *err_line(int code) {
    if (code == COMPANION_ERR_FULL) return "空间不够。";
    if (code == COMPANION_ERR_EXISTS) return "这个分类已经有了。";
    if (code == COMPANION_ERR_BUSY) return "这个分类里还有记录。";
    if (code == COMPANION_ERR_TOO_BIG) return "内容太长。";
    if (code == 0) return NULL;
    return "没有保存成功。";
}

static bool page_head(httpd_req_t *req, int code) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!chunk(req,
               "<!DOCTYPE html><meta charset=utf-8>"
               "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
               "<title>AI Passport</title><style>"
               "body{font-family:sans-serif;background:#1c1916;color:#f6f1e8;margin:16px}"
               "input,textarea,button{font:16px sans-serif;width:100%;box-sizing:border-box;margin:4px 0}"
               "button{background:#e39b4a;color:#1c1916;border:0;padding:10px}"
               "a{color:#e7c8a0}section{margin:20px 0}p{line-height:1.4}"
               "</style><h1>AI Passport</h1>")) {
        return false;
    }
    const char *line = err_line(code);
    if (line && !chunk(req, "<p>") ) return false;
    if (line && !chunk(req, line)) return false;
    if (line && !chunk(req, "</p>")) return false;
    return chunk(req,
                 "<p>连不上家里的网时，用微信小程序「蓝牙配网-FoloToy AI PASSPORT」，"
                 "找设备 BLUFI_FoloPassport。也可以连开放热点 AI-Passport，打开 "
                 "http://192.168.4.1 。</p>");
}

static bool page_persona(httpd_req_t *req) {
    size_t len = 0;
    s_body[0] = '\0';
    if (!companion_store_ready() ||
        companion_store_persona_get(s_body, sizeof(s_body), &len) != COMPANION_OK) {
        len = 0;
    }
    if (!chunk(req, "<section><h2>人设</h2><form method=post action=/persona>"
                    "<textarea name=persona rows=6>")) {
        return false;
    }
    if (!chunk_escape(req, s_body, len)) return false;
    return chunk(req, "</textarea><button>保存人设</button></form></section>");
}

static bool page_key(httpd_req_t *req) {
    const char *state = companion_secret_is_set() ? "已设置" : "未设置";
    if (!chunk(req, "<section><h2>模型密钥</h2><p>")) return false;
    if (!chunk(req, state)) return false;
    return chunk(req, "</p><form method=post action=/apikey>"
                      "<input name=key type=password autocomplete=off>"
                      "<button>保存密钥</button></form></section>");
}

static bool page_categories(httpd_req_t *req) {
    if (!chunk(req, "<section><h2>分类</h2>")) return false;
    size_t count = 0;
    if (!companion_store_ready() || companion_store_category_count(&count) != COMPANION_OK) {
        return chunk(req, "<p>存储不可用。</p></section>");
    }
    for (size_t i = 0; i < count; i++) {
        companion_category_t cat;
        if (companion_store_category_at(i, &cat) != COMPANION_OK) continue;
        char form[96];
        snprintf(form, sizeof(form),
                 "<form method=post action=/category/rename>"
                 "<input type=hidden name=id value=%u>",
                 (unsigned)cat.id);
        if (!chunk(req, form)) return false;
        if (!chunk(req, "<input name=name value=\"")) return false;
        if (!chunk_escape(req, cat.name, strlen(cat.name))) return false;
        if (!chunk(req, "\"><button>改名</button></form>")) return false;
    }
    return chunk(req, "<form method=post action=/category>"
                      "<input name=name placeholder=新分类>"
                      "<button>添加分类</button></form></section>");
}

static bool page_records(httpd_req_t *req, int page) {
    if (!chunk(req, "<section><h2>记录</h2>")) return false;
    size_t count = 0;
    if (!companion_store_ready() || companion_store_record_count(0, &count) != COMPANION_OK) {
        return chunk(req, "<p>存储不可用。</p></section>");
    }
    if (count == 0) return chunk(req, "<p>还没有记录。</p></section>");
    int pages = (int)((count + 4) / 5);
    if (page >= pages) page = pages - 1;
    size_t begin = (size_t)page * 5u;
    for (size_t n = 0; n < 5 && begin + n < count; n++) {
        size_t index = count - 1 - (begin + n);
        companion_record_info_t info;
        size_t text_len = 0;
        memset(&info, 0, sizeof(info));
        companion_err_t err = companion_store_record_at(0, index, &info, s_export,
                                                        sizeof(s_export), &text_len);
        char stamp[24];
        companion_category_t cat;
        cat.name[0] = '\0';
        companion_store_category_find(info.category_id, &cat);
        companion_note_stamp(stamp, sizeof(stamp), info.created_unix);
        if (!chunk(req, "<p>")) return false;
        if (!chunk_escape(req, stamp, strlen(stamp))) return false;
        if (!chunk_escape(req, cat.name, strlen(cat.name))) return false;
        if (!chunk(req, "<br>")) return false;
        if (err == COMPANION_OK) {
            size_t show = companion_nav_utf8_prefix(s_export, text_len, 180);
            if (!chunk_escape(req, s_export, show)) return false;
        }
        if (!chunk(req, "</p>")) return false;
    }
    char links[80];
    snprintf(links, sizeof(links), "<p>");
    if (!chunk(req, links)) return false;
    if (page > 0) {
        snprintf(links, sizeof(links), "<a href=\"/?page=%d\">上一页</a> ", page - 1);
        if (!chunk(req, links)) return false;
    }
    if (page + 1 < pages) {
        snprintf(links, sizeof(links), "<a href=\"/?page=%d\">下一页</a>", page + 1);
        if (!chunk(req, links)) return false;
    }
    return chunk(req, "</p></section>");
}

static bool page_storage(httpd_req_t *req) {
    uint32_t live = 0;
    uint32_t limit = 0;
    if (!companion_store_ready() || companion_store_usage(&live, &limit) != COMPANION_OK) {
        return chunk(req, "<section><h2>空间</h2><p>存储不可用。</p></section>");
    }
    char line[64];
    snprintf(line, sizeof(line), "<section><h2>空间</h2><p>%u / %u KB</p></section>",
             (unsigned)(live / 1024u), (unsigned)(limit / 1024u));
    return chunk(req, line);
}

static bool page_wifi(httpd_req_t *req) {
    return chunk(req,
                 "<section><h2>Wi-Fi</h2>"
                 "<form method=post action=/wifi>"
                 "<input name=ssid placeholder=名称 autocapitalize=none>"
                 "<input name=password type=password placeholder=密码>"
                 "<button>连接</button></form>"
                 "<form method=post action=/wifi/forget><button>忘记网络</button></form>"
                 "</section><p><a href=/export>下载笔记</a></p>");
}

static esp_err_t handle_index(httpd_req_t *req) {
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    bool ok = page_head(req, query_code(req)) && page_persona(req) && page_key(req) &&
              page_categories(req) && page_records(req, query_page(req)) &&
              page_storage(req) && page_wifi(req);
    if (ok) httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreGive(s_mu);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t handle_persona(httpd_req_t *req) {
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    esp_err_t read_err = read_body(req);
    if (read_err != ESP_OK) {
        xSemaphoreGive(s_mu);
        return read_err;
    }
    size_t len = 0;
    int code = COMPANION_ERR_ARG;
    if (take_field("persona", sizeof(s_field), &len)) {
        code = (int)companion_store_persona_set(s_field, len);
        ESP_LOGI(TAG, "persona save bytes=%u code=%d", (unsigned)len, code);
    }
    memset(s_field, 0, sizeof(s_field));
    memset(s_body, 0, s_body_len);
    esp_err_t err = redirect(req, code);
    xSemaphoreGive(s_mu);
    return err;
}

static esp_err_t handle_apikey(httpd_req_t *req) {
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    esp_err_t read_err = read_body(req);
    if (read_err != ESP_OK) {
        xSemaphoreGive(s_mu);
        return read_err;
    }
    size_t len = 0;
    int code = COMPANION_ERR_ARG;
    if (take_field("key", 129, &len) && len > 0) {
        code = companion_secret_set(s_field, len) ? 0 : COMPANION_ERR_IO;
    }
    memset(s_field, 0, sizeof(s_field));
    memset(s_body, 0, s_body_len);
    esp_err_t err = redirect(req, code);
    xSemaphoreGive(s_mu);
    return err;
}

static esp_err_t handle_category(httpd_req_t *req) {
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    esp_err_t read_err = read_body(req);
    if (read_err != ESP_OK) {
        xSemaphoreGive(s_mu);
        return read_err;
    }
    size_t len = 0;
    int code = COMPANION_ERR_ARG;
    if (take_field("name", COMPANION_CATEGORY_NAME_MAX + 1, &len) && len > 0) {
        uint32_t id = 0;
        code = (int)companion_store_category_create(s_field, len, &id);
        ESP_LOGI(TAG, "category create code=%d", code);
    }
    memset(s_field, 0, sizeof(s_field));
    memset(s_body, 0, s_body_len);
    esp_err_t err = redirect(req, code);
    xSemaphoreGive(s_mu);
    return err;
}

static esp_err_t handle_rename(httpd_req_t *req) {
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    esp_err_t read_err = read_body(req);
    if (read_err != ESP_OK) {
        xSemaphoreGive(s_mu);
        return read_err;
    }
    size_t name_len = 0;
    char id_text[16];
    size_t id_len = 0;
    int code = COMPANION_ERR_ARG;
    bool have_id = companion_form_get(s_body, s_body_len, "id", id_text, sizeof(id_text), &id_len);
    if (have_id && take_field("name", COMPANION_CATEGORY_NAME_MAX + 1, &name_len) && name_len > 0) {
        char *end = NULL;
        unsigned long id = strtoul(id_text, &end, 10);
        if (end && *end == '\0' && id > 0 && id <= UINT32_MAX) {
            code = (int)companion_store_category_rename((uint32_t)id, s_field, name_len);
            ESP_LOGI(TAG, "category rename id=%lu code=%d", id, code);
        }
    }
    memset(s_field, 0, sizeof(s_field));
    memset(s_body, 0, s_body_len);
    esp_err_t err = redirect(req, code);
    xSemaphoreGive(s_mu);
    return err;
}

static esp_err_t handle_wifi(httpd_req_t *req) {
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    esp_err_t read_err = read_body(req);
    if (read_err != ESP_OK) {
        xSemaphoreGive(s_mu);
        return read_err;
    }
    char ssid[33];
    size_t ssid_len = 0;
    size_t pass_len = 0;
    int code = COMPANION_ERR_ARG;
    bool have_ssid = companion_form_get(s_body, s_body_len, "ssid", ssid, sizeof(ssid), &ssid_len);
    if (have_ssid && ssid_len > 0 && take_field("password", 64, &pass_len)) {
        code = companion_net_set_sta(ssid, ssid_len, s_field, pass_len) ? 0 : COMPANION_ERR_IO;
    }
    memset(ssid, 0, sizeof(ssid));
    memset(s_field, 0, sizeof(s_field));
    memset(s_body, 0, s_body_len);
    esp_err_t err = redirect(req, code);
    xSemaphoreGive(s_mu);
    return err;
}

static esp_err_t handle_forget(httpd_req_t *req) {
    companion_net_forget();
    return redirect(req, 0);
}

static esp_err_t handle_export(httpd_req_t *req) {
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(8000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    char name[40];
    char disp[80];
    uint32_t now = companion_net_unix_time();
    if (!companion_note_filename(name, sizeof(name), now)) {
        memcpy(name, "passport-notes.txt", 19);
    }
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    bool ok = httpd_resp_send_chunk(req, "\xEF\xBB\xBF", 3) == ESP_OK;
    size_t cats = 0;
    if (ok && companion_store_ready() && companion_store_category_count(&cats) == COMPANION_OK) {
        for (size_t c = 0; ok && c < cats; c++) {
            companion_category_t cat;
            if (companion_store_category_at(c, &cat) != COMPANION_OK) continue;
            size_t records = 0;
            if (companion_store_record_count(cat.id, &records) != COMPANION_OK || records == 0) {
                continue;
            }
            char heading[80];
            if (!companion_note_heading(heading, sizeof(heading), cat.name)) continue;
            ok = chunk(req, heading);
            for (size_t i = 0; ok && i < records; i++) {
                companion_record_info_t info;
                size_t text_len = 0;
                memset(&info, 0, sizeof(info));
                companion_err_t err = companion_store_record_at(cat.id, i, &info, s_export,
                                                                sizeof(s_export), &text_len);
                char stamp[24];
                if (!companion_note_stamp(stamp, sizeof(stamp), info.created_unix)) continue;
                ok = chunk(req, stamp);
                if (ok && err == COMPANION_OK) ok = httpd_resp_send_chunk(req, s_export, text_len) == ESP_OK;
                if (ok) ok = chunk(req, "\n");
            }
            if (ok) ok = chunk(req, "\n");
        }
    }
    if (ok) httpd_resp_send_chunk(req, NULL, 0);
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "export done");
    return ok ? ESP_OK : ESP_FAIL;
}

void companion_web_start(void) {
    if (s_httpd) return;
    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) {
        ESP_LOGE(TAG, "mutex failed");
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;
    cfg.max_open_sockets = 3;
    cfg.stack_size = 5120;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "http start failed");
        s_httpd = NULL;
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_index},
        {.uri = "/persona", .method = HTTP_POST, .handler = handle_persona},
        {.uri = "/apikey", .method = HTTP_POST, .handler = handle_apikey},
        {.uri = "/category", .method = HTTP_POST, .handler = handle_category},
        {.uri = "/category/rename", .method = HTTP_POST, .handler = handle_rename},
        {.uri = "/wifi", .method = HTTP_POST, .handler = handle_wifi},
        {.uri = "/wifi/forget", .method = HTTP_POST, .handler = handle_forget},
        {.uri = "/export", .method = HTTP_GET, .handler = handle_export},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(s_httpd, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "route failed");
        }
    }
    ESP_LOGI(TAG, "http ready");
}

// Original companion screens: home, listen, confirm, browse.
//
// The figure is a circle with two eyes. It breathes, blinks, leans while
// listening, and nods after a saved note or a finished chat turn. This is not
// the baseline pixel mascot or test menu.
//
// Chinese uses the LVGL built-in Source Han Sans SC 16 CJK subset. That face
// does not cover every character a transcript may contain. Missing glyphs keep
// the LVGL placeholder; they are not hidden. Digits, the level bars, the link
// dot, and the face motion stay readable when a glyph is absent.
//
// Lock order: never call the store or NVS while holding the LVGL lock.
// Logs use ids, lengths, and error codes only.

#include "companion_ui.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "companion_ai.h"
#include "companion_nav.h"
#include "companion_net.h"
#include "companion_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui";

enum {
    COL_BG = 0x141210,
    COL_FACE = 0xF3E6D0,
    COL_FLASH = 0xE39B4A,
    COL_EYE = 0x1C1916,
    COL_EAR = 0xE7C8A0,
    COL_BUBBLE = 0x2A241C,
    COL_TEXT = 0xF6F1E8,
    COL_MUTED = 0xB7AA9A,
    COL_WARN = 0xE0704A,
    COL_OFF = 0xC45A3A,
    COL_ON = 0x7DAB6A,
};

enum { MSG_KEY = 1, MSG_LINK = 2, MSG_REPLY = 3, MSG_FAIL = 4 };

typedef struct {
    uint8_t type;
    uint8_t btn;
    uint8_t ev;
    uint8_t online;
    char ip[16];
    uint32_t gen;
} ui_msg_t;

static const char *const k_off_chat = "离线，无法聊天";
static const char *const k_off_rec = "离线，无法记录";
static const char *const k_busy = "请先说完";
static const char *const k_listen = "正在听";
static const char *const k_wait = "...";
static const char *const k_proc = "识别中...";
static const char *const k_empty = "没有记录";
static const char *const k_nostore = "存储不可用";
static const char *const k_saved = "已保存";
static const char *const k_drop = "已丢弃";
static const char *const k_fail = "识别失败";
static const char *const k_daily = "日常";

static const lv_font_t *const k_font = &lv_font_source_han_sans_sc_16_cjk;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_mu;
static volatile bool s_ready;
static companion_nav_t s_nav;

static char s_text[4096];
static size_t s_text_len;
static bool s_text_ready;
static bool s_latched;
static bool s_text_is_new;
static uint32_t s_text_category;
static uint32_t s_text_generation;
static uint32_t s_generation;
static char s_new_name[COMPANION_CATEGORY_NAME_MAX + 1];
static char s_record[4096];
static char s_view[1600];
static char s_meta[128];
static char s_index[32];
static uint32_t s_order[32];
static size_t s_order_n;
static const char *s_banner;
static bool s_err_hint;
static char s_link_ip[16];
static char s_reply[400];
static size_t s_reply_len;
static bool s_reply_ready;
static uint32_t s_reply_gen;
static char s_shown[400];

_Static_assert(sizeof(s_text) <= 8192, "draft buffer exceeds 8KB");
_Static_assert(sizeof(s_record) <= 8192, "browse buffer exceeds 8KB");

static lv_obj_t *s_scr;
static lv_obj_t *s_face;
static lv_obj_t *s_eye_l;
static lv_obj_t *s_eye_r;
static lv_obj_t *s_ear;
static lv_obj_t *s_bubble;
static lv_obj_t *s_body;
static lv_obj_t *s_meta_label;
static lv_obj_t *s_index_label;
static lv_obj_t *s_title;
static lv_obj_t *s_battery;
static lv_obj_t *s_link;
static lv_obj_t *s_dot;
static lv_obj_t *s_bar[7];

static int s_anim;
static int s_flash;
static int s_nod;
static int s_shown_sec = -1;
static int s_soc = -2;
static uint8_t s_light;
static bool s_saw_hold;
static int s_release_hits;
static TickType_t s_active;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_ready || !s_q) {
        return;
    }
    ui_msg_t msg = {
        .type = MSG_KEY,
        .btn = (uint8_t)btn,
        .ev = (uint8_t)ev,
    };
    (void)xQueueSend(s_q, &msg, 0);
}

static bool map_key(const ui_msg_t *msg, companion_ev_t *out) {
    if (msg->ev == BSP_BTN_PRESS || msg->ev == BSP_BTN_DOUBLE) {
        return false;
    }
    if (msg->btn == BSP_BTN_OK && msg->ev == BSP_BTN_CLICK) {
        *out = COMPANION_EV_OK_SHORT;
        return true;
    }
    if (msg->btn == BSP_BTN_OK && msg->ev == BSP_BTN_LONG) {
        *out = COMPANION_EV_OK_LONG;
        return true;
    }
    if (msg->btn == BSP_BTN_UP && msg->ev == BSP_BTN_CLICK) {
        *out = COMPANION_EV_UP;
        return true;
    }
    if (msg->btn == BSP_BTN_DOWN && msg->ev == BSP_BTN_CLICK) {
        *out = COMPANION_EV_DOWN;
        return true;
    }
    return false;
}

static bool ok_is_held(int mv) {
    static const uint16_t windows[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;
    return mv >= windows[BSP_BTN_OK][0] && mv < windows[BSP_BTN_OK][1];
}

static bool poll_release(void) {
    int mv = bsp_button_read_mv();
    if (mv < 0) {
        return false;
    }
    if (ok_is_held(mv)) {
        s_saw_hold = true;
        s_release_hits = 0;
        return false;
    }
    // The long-press event arrives while the key is down, but the task may not
    // sample until after the release. A few released samples still end capture.
    s_release_hits++;
    return s_release_hits >= (s_saw_hold ? 2 : 5);
}

static void lock_mu(void) {
    if (s_mu) {
        (void)xSemaphoreTake(s_mu, portMAX_DELAY);
    }
}

static void unlock_mu(void) {
    if (s_mu) {
        (void)xSemaphoreGive(s_mu);
    }
}

static void clear_draft(void) {
    lock_mu();
    if (s_text_len > 0) {
        memset(s_text, 0, s_text_len);
    }
    s_text_len = 0;
    s_text_ready = false;
    s_latched = false;
    s_text_is_new = false;
    s_text_category = 0;
    s_text_generation = 0;
    memset(s_new_name, 0, sizeof(s_new_name));
    unlock_mu();
}

static void bump_generation(void) {
    lock_mu();
    s_generation++;
    if (s_generation == 0) {
        s_generation = 1;
    }
    unlock_mu();
}

static void copy_show(char *dst, size_t cap, const char *src, size_t len, size_t limit) {
    size_t room = cap > 0 ? cap - 1 : 0;
    bool cut = false;
    if (limit < room) {
        if (len > limit) {
            cut = true;
            room = limit;
        }
    }
    size_t n = companion_nav_utf8_prefix(src ? src : "", src ? len : 0, room);
    if (n > 0 && src) {
        memcpy(dst, src, n);
    }
    if (cut && n + 3 < cap) {
        memcpy(dst + n, "...", 3);
        n += 3;
    }
    dst[n] = '\0';
}

static uint32_t category_by_name(const char *name) {
    size_t count = 0;
    if (!name || companion_store_category_count(&count) != COMPANION_OK) {
        return 0;
    }
    if (count > 32) {
        count = 32;
    }
    for (size_t i = 0; i < count; i++) {
        companion_category_t cat;
        if (companion_store_category_at(i, &cat) != COMPANION_OK) {
            continue;
        }
        if (strcmp(cat.name, name) == 0) {
            return cat.id;
        }
    }
    return 0;
}

static void category_label(uint32_t id, bool is_new, char *dst, size_t cap) {
    if (is_new && s_new_name[0] != '\0') {
        copy_show(dst, cap, s_new_name, strlen(s_new_name), cap - 1);
        return;
    }
    companion_category_t cat;
    if (id != 0 && companion_store_category_find(id, &cat) == COMPANION_OK) {
        copy_show(dst, cap, cat.name, strlen(cat.name), cap - 1);
        return;
    }
    copy_show(dst, cap, "--", 2, 2);
}

static void seed_categories(void) {
    static const char *const names[] = {"日常", "网球", "读书"};
    size_t count = 0;
    if (!companion_store_ready() ||
        companion_store_category_count(&count) != COMPANION_OK || count != 0) {
        return;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        uint32_t id = 0;
        companion_err_t err = companion_store_category_create(
            names[i], strlen(names[i]), &id);
        if (err != COMPANION_OK) {
            ESP_LOGE(TAG, "seed failed: %d", (int)err);
        }
    }
}

static void load_order(void) {
    uint32_t file_ids[32];
    size_t file_n = 0;
    size_t count = 0;
    if (companion_store_ready() && companion_store_category_count(&count) == COMPANION_OK) {
        if (count > 32) {
            count = 32;
        }
        for (size_t i = 0; i < count; i++) {
            companion_category_t cat;
            if (companion_store_category_at(i, &cat) == COMPANION_OK) {
                file_ids[file_n++] = cat.id;
            }
        }
    }
    uint32_t saved[32];
    size_t saved_n = 0;
    nvs_handle_t handle;
    if (nvs_open("companion", NVS_READONLY, &handle) == ESP_OK) {
        size_t bytes = sizeof(saved);
        if (nvs_get_blob(handle, "catord", saved, &bytes) == ESP_OK &&
            bytes % sizeof(uint32_t) == 0) {
            saved_n = bytes / sizeof(uint32_t);
        }
        nvs_close(handle);
    }
    s_order_n = companion_nav_apply_order(s_order, 32, file_ids, file_n, saved, saved_n);
}

static void save_order(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("companion", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "order open failed: %d", (int)err);
        return;
    }
    err = nvs_set_blob(handle, "catord", s_order, s_order_n * sizeof(uint32_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "order save failed: %d", (int)err);
    }
    nvs_close(handle);
}

static void refresh_count(void) {
    size_t count = 0;
    if (!companion_store_ready() ||
        companion_store_record_count(0, &count) != COMPANION_OK) {
        companion_nav_set_browse_count(&s_nav, 0);
        return;
    }
    if (count > 100000u) {
        count = 100000u;
    }
    companion_nav_set_browse_count(&s_nav, (int)count);
}

static bool save_draft(void) {
    if (!s_latched || s_text_len == 0 || !companion_store_ready()) {
        return false;
    }
    const companion_choice_t *choice = companion_nav_choice(&s_nav);
    if (!choice) {
        return false;
    }
    uint32_t category = choice->id;
    if (choice->is_new) {
        uint32_t id = 0;
        size_t name_len = strlen(s_new_name);
        companion_err_t err = companion_store_category_create(s_new_name, name_len, &id);
        if (err == COMPANION_ERR_EXISTS) {
            id = category_by_name(s_new_name);
        } else if (err != COMPANION_OK) {
            ESP_LOGE(TAG, "category create failed: %d", (int)err);
            return false;
        }
        if (id == 0) {
            return false;
        }
        category = id;
    }
    if (category == 0) {
        return false;
    }
    uint32_t record_id = 0;
    companion_err_t err = companion_store_record_append(
        category, companion_net_unix_time(), s_text, s_text_len, &record_id);
    if (err != COMPANION_OK) {
        ESP_LOGE(TAG, "record save failed: %d", (int)err);
        return false;
    }
    ESP_LOGI(TAG, "record saved id=%u category=%u bytes=%u",
             record_id, category, (unsigned)s_text_len);
    companion_nav_order_note(s_order, &s_order_n, 32, category);
    save_order();
    return true;
}

static void accept_draft(void) {
    bool take = false;
    lock_mu();
    if (s_text_ready && s_text_generation != s_generation) {
        s_text_ready = false;
    }
    if (s_text_ready && s_nav.page == COMPANION_PAGE_LISTEN &&
        s_nav.listen_mode == COMPANION_LISTEN_PROCESS) {
        s_text_ready = false;
        s_latched = true;
        take = s_text_len > 0;
    }
    unlock_mu();
    if (!take) {
        return;
    }
    if (!s_text_is_new && s_text_category == 0) {
        s_text_category = category_by_name(k_daily);
    }
    companion_choice_t choices[COMPANION_NAV_CATEGORY_MAX];
    int count = 0;
    int selected = 0;
    if (s_text_is_new && s_new_name[0] != '\0' && count < COMPANION_NAV_CATEGORY_MAX) {
        choices[count++] = (companion_choice_t){.id = 0, .is_new = true};
    }
    for (size_t i = 0; i < s_order_n && count < COMPANION_NAV_CATEGORY_MAX; i++) {
        if (!s_text_is_new && s_order[i] == s_text_category) {
            selected = count;
        }
        choices[count++] = (companion_choice_t){.id = s_order[i], .is_new = false};
    }
    companion_nav_set_choices(&s_nav, choices, count, selected);
    (void)companion_nav_process_done(&s_nav, true);
    ESP_LOGI(TAG, "draft ready bytes=%u", (unsigned)s_text_len);
}

static void fill_time(uint32_t unix_time, char *dst, size_t cap) {
    if (unix_time == 0) {
        copy_show(dst, cap, "--", 2, 2);
        return;
    }
    char raw[20];
    companion_format_unix_utc(unix_time, raw);
    copy_show(dst, cap, raw, 16, 16);
}

static void rebuild_text(void) {
    s_view[0] = '\0';
    s_meta[0] = '\0';
    s_index[0] = '\0';
    if (s_nav.page == COMPANION_PAGE_HOME) {
        if (s_banner) {
            copy_show(s_view, sizeof(s_view), s_banner, strlen(s_banner), 120);
        } else if (s_nav.online && s_link_ip[0] != '\0') {
            snprintf(s_view, sizeof(s_view), "http://%s", s_link_ip);
        } else {
            copy_show(s_view, sizeof(s_view), "BLUFI_FoloPassport", 18, 120);
        }
        return;
    }
    if (s_nav.page == COMPANION_PAGE_LISTEN) {
        const char *line = s_nav.listen_mode == COMPANION_LISTEN_PROCESS ? k_proc : k_listen;
        copy_show(s_view, sizeof(s_view), line, strlen(line), 80);
        unsigned sec = s_nav.listen_ms / 1000u;
        if (sec > 60u) {
            sec = 60u;
        }
        snprintf(s_meta, sizeof(s_meta), "%02u:%02u", sec / 60u, sec % 60u);
        if (s_nav.truncated) {
            snprintf(s_index, sizeof(s_index), "60s");
        }
        return;
    }
    if (s_nav.page == COMPANION_PAGE_CONFIRM) {
        copy_show(s_view, sizeof(s_view), s_text, s_text_len, 1400);
        const companion_choice_t *choice = companion_nav_choice(&s_nav);
        if (choice) {
            category_label(choice->id, choice->is_new, s_meta, sizeof(s_meta));
        }
        if (s_err_hint) {
            snprintf(s_index, sizeof(s_index), "ERR");
        } else if (s_nav.choice_count > 0) {
            snprintf(s_index, sizeof(s_index), "%d/%d%s",
                     s_nav.choice_index + 1, s_nav.choice_count,
                     s_nav.truncated ? " 60s" : "");
        }
        return;
    }
    int file_index = companion_nav_browse_file_index(&s_nav);
    if (!companion_store_ready()) {
        copy_show(s_view, sizeof(s_view), k_nostore, strlen(k_nostore), 80);
        return;
    }
    if (file_index < 0) {
        copy_show(s_view, sizeof(s_view), k_empty, strlen(k_empty), 80);
        snprintf(s_index, sizeof(s_index), "0/0");
        return;
    }
    companion_record_info_t info;
    size_t text_len = 0;
    memset(&info, 0, sizeof(info));
    companion_err_t err = companion_store_record_at(
        0, (size_t)file_index, &info, s_record, sizeof(s_record), &text_len);
    if (err == COMPANION_ERR_TOO_BIG) {
        copy_show(s_view, sizeof(s_view), "...", 3, 3);
    } else if (err != COMPANION_OK) {
        copy_show(s_view, sizeof(s_view), k_nostore, strlen(k_nostore), 80);
        ESP_LOGE(TAG, "browse read failed: %d", (int)err);
        return;
    } else {
        size_t limit = s_nav.expanded ? 1400u : 96u;
        copy_show(s_view, sizeof(s_view), s_record, text_len, limit);
    }
    char name[COMPANION_CATEGORY_NAME_MAX + 1];
    char when[20];
    category_label(info.category_id, false, name, sizeof(name));
    fill_time(info.created_unix, when, sizeof(when));
    snprintf(s_meta, sizeof(s_meta), "%s  %s", name, when);
    snprintf(s_index, sizeof(s_index), "%d/%d", s_nav.browse_pos + 1, s_nav.browse_count);
}

static void show(lv_obj_t *obj, bool visible) {
    if (!obj) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_box(lv_obj_t *parent, int w, int h, uint32_t color, int radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, k_font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, "");
    return label;
}

static bool create_widgets(void) {
    s_scr = lv_obj_create(NULL);
    if (!s_scr) {
        return false;
    }
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_text_font(s_scr, k_font, 0);
    lv_obj_set_style_text_color(s_scr, lv_color_hex(COL_TEXT), 0);

    s_title = make_label(s_scr, COL_MUTED);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 20, 28);
    s_index_label = make_label(s_scr, COL_TEXT);
    lv_obj_align(s_index_label, LV_ALIGN_TOP_MID, 0, 28);
    s_link = make_label(s_scr, COL_MUTED);
    lv_obj_align(s_link, LV_ALIGN_TOP_LEFT, 20, 52);
    s_battery = make_label(s_scr, COL_TEXT);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -34, 26);
    s_dot = make_box(s_scr, 10, 10, COL_OFF, LV_RADIUS_CIRCLE);
    lv_obj_align(s_dot, LV_ALIGN_TOP_RIGHT, -18, 32);

    s_face = make_box(s_scr, 96, 96, COL_FACE, LV_RADIUS_CIRCLE);
    s_eye_l = make_box(s_face, 14, 8, COL_EYE, 2);
    s_eye_r = make_box(s_face, 14, 8, COL_EYE, 2);
    s_ear = make_box(s_face, 16, 22, COL_EAR, LV_RADIUS_CIRCLE);
    show(s_ear, false);

    for (int i = 0; i < 7; i++) {
        s_bar[i] = make_box(s_scr, 10, 8, COL_FLASH, 3);
        show(s_bar[i], false);
    }

    s_meta_label = make_label(s_scr, COL_MUTED);
    lv_obj_set_width(s_meta_label, 200);
    lv_obj_set_style_text_align(s_meta_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_meta_label, LV_ALIGN_BOTTOM_MID, 0, -128);

    s_bubble = make_box(s_scr, 204, 88, COL_BUBBLE, 18);
    lv_obj_align(s_bubble, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_pad_all(s_bubble, 10, 0);
    s_body = make_label(s_bubble, COL_TEXT);
    lv_obj_set_width(s_body, 184);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_screen_load(s_scr);
    if (!s_face || !s_body || !s_battery || !s_link || !s_dot) {
        return false;
    }
    for (int i = 0; i < 7; i++) {
        if (!s_bar[i]) {
            return false;
        }
    }
    return true;
}

static void apply_text(void) {
    const char *title = "";
    bool tall = false;
    if (s_nav.page == COMPANION_PAGE_LISTEN) {
        title = "倾听";
    } else if (s_nav.page == COMPANION_PAGE_CONFIRM) {
        title = "确认";
        tall = true;
    } else if (s_nav.page == COMPANION_PAGE_BROWSE) {
        title = "浏览";
        tall = true;
    }
    lv_label_set_text(s_title, title);
    lv_label_set_text(s_body, s_view);
    lv_label_set_text(s_meta_label, s_meta);
    lv_label_set_text(s_index_label, s_index);
    show(s_title, title[0] != '\0');
    show(s_meta_label, s_meta[0] != '\0');
    show(s_index_label, s_index[0] != '\0');
    lv_obj_set_height(s_bubble, tall ? 150 : 88);
    lv_obj_align(s_meta_label, LV_ALIGN_BOTTOM_MID, 0, tall ? -176 : -120);
    lv_label_set_text(s_link, s_nav.online ? "在线" : "离线");
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(s_nav.online ? COL_ON : COL_OFF), 0);
}

static void apply_battery(void) {
    if (s_soc < 0) {
        show(s_battery, false);
        return;
    }
    unsigned pct = (unsigned)s_soc;
    if (pct > 100u) {
        pct = 100u;
    }
    char line[16];
    snprintf(line, sizeof(line), "%u%%", pct);
    lv_label_set_text(s_battery, line);
    lv_obj_set_style_text_color(s_battery,
                                lv_color_hex(s_soc < 20 ? COL_WARN : COL_TEXT), 0);
    show(s_battery, true);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -34, 26);
}

static void apply_pose(void) {
    bool listen_pose = (s_nav.page == COMPANION_PAGE_LISTEN &&
                        s_nav.listen_mode == COMPANION_LISTEN_CAPTURE) ||
                       (s_nav.page == COMPANION_PAGE_HOME &&
                        s_nav.home_mode == COMPANION_HOME_CHAT);
    int tri = s_anim < 100 ? s_anim : 200 - s_anim;
    int size = 90 + tri / 8;
    bool blink = s_anim < 6;
    int lean = listen_pose ? 12 : 0;
    int nod = s_nod > 0 ? 8 : 0;
    lv_obj_set_size(s_face, size, size);
    lv_obj_set_style_bg_color(s_face, lv_color_hex(s_flash > 0 ? COL_FLASH : COL_FACE), 0);
    lv_obj_align(s_face, LV_ALIGN_CENTER, lean, -28 + nod);
    lv_obj_set_size(s_eye_l, 14, blink ? 2 : 8);
    lv_obj_set_size(s_eye_r, 14, blink ? 2 : 8);
    lv_obj_align(s_eye_l, LV_ALIGN_CENTER, -16, -6);
    lv_obj_align(s_eye_r, LV_ALIGN_CENTER, 16, -6);
    show(s_ear, listen_pose);
    lv_obj_align(s_ear, LV_ALIGN_RIGHT_MID, 4, -8);

    bool bars = s_nav.page == COMPANION_PAGE_LISTEN;
    bool live = bars && s_nav.listen_mode == COMPANION_LISTEN_CAPTURE;
    for (int i = 0; i < 7; i++) {
        int h = 8;
        if (live) {
            int wave = (s_anim + i * 5) % 20;
            h = 8 + (wave < 10 ? wave : 20 - wave);
        }
        lv_obj_set_size(s_bar[i], 10, h);
        lv_obj_set_pos(s_bar[i], 64 + i * 16, 208 - h);
        show(s_bar[i], bars);
    }
    if (s_flash > 0) {
        s_flash--;
    }
    if (s_nod > 0) {
        s_nod--;
    }
    s_anim = (s_anim + 1) % 200;
}

static void note_action(companion_act_t act) {
    if (act == COMPANION_ACT_NONE) {
        return;
    }
    s_flash = 8;
    if (act == COMPANION_ACT_LISTEN_START) {
        s_saw_hold = false;
        s_release_hits = 0;
        s_shown_sec = -1;
        s_banner = NULL;
        s_err_hint = false;
        clear_draft();
        bump_generation();
    } else if (act == COMPANION_ACT_OFFLINE_CHAT) {
        s_banner = k_off_chat;
    } else if (act == COMPANION_ACT_OFFLINE_RECORD) {
        s_banner = k_off_rec;
    } else if (act == COMPANION_ACT_BUSY && s_nav.page == COMPANION_PAGE_HOME) {
        s_banner = k_busy;
    } else if (act == COMPANION_ACT_CHAT_START) {
        bump_generation();
        s_banner = k_listen;
    } else if (act == COMPANION_ACT_CHAT_END) {
        s_banner = k_wait;
        s_nod = 12;
    } else if (act == COMPANION_ACT_SAVED) {
        s_banner = k_saved;
        s_nod = 12;
        s_err_hint = false;
        clear_draft();
    } else if (act == COMPANION_ACT_DISCARD) {
        s_banner = k_drop;
        s_err_hint = false;
        clear_draft();
    } else if (act == COMPANION_ACT_FAIL) {
        s_banner = k_fail;
        clear_draft();
    } else if (act == COMPANION_ACT_SAVE_FAIL) {
        s_err_hint = true;
    } else if (act == COMPANION_ACT_REFRESH || act == COMPANION_ACT_CONFIRM) {
        s_err_hint = false;
    }
    ESP_LOGI(TAG, "page %d act %d", (int)s_nav.page, (int)act);
    if (act == COMPANION_ACT_LISTEN_START || act == COMPANION_ACT_CHAT_START ||
        act == COMPANION_ACT_PROCESS || act == COMPANION_ACT_TRUNCATED ||
        act == COMPANION_ACT_CHAT_END) {
        companion_ai_note(act, s_generation);
    }
}

static void handle_msg(const ui_msg_t *msg) {
    s_active = xTaskGetTickCount();
    if (msg->type == MSG_LINK) {
        bool was_online = s_nav.online;
        companion_nav_set_online(&s_nav, msg->online != 0);
        if (msg->online) {
            snprintf(s_link_ip, sizeof(s_link_ip), "%s", msg->ip);
            if (!was_online) s_banner = NULL;
        } else {
            s_link_ip[0] = '\0';
        }
        note_action(COMPANION_ACT_REFRESH);
        return;
    }
    if (msg->type == MSG_FAIL) {
        if (msg->gen != s_generation) return;
        if (s_nav.page == COMPANION_PAGE_LISTEN &&
            s_nav.listen_mode == COMPANION_LISTEN_PROCESS) {
            note_action(companion_nav_process_done(&s_nav, false));
        } else {
            s_banner = k_fail;
        }
        return;
    }
    if (msg->type == MSG_REPLY) {
        bool take = false;
        lock_mu();
        if (s_reply_ready && s_reply_gen == s_generation && msg->gen == s_generation) {
            memcpy(s_shown, s_reply, s_reply_len);
            s_shown[s_reply_len] = '\0';
            s_reply_ready = false;
            take = true;
        }
        unlock_mu();
        if (!take) return;
        s_banner = s_shown;
        s_nod = 12;
        return;
    }
    companion_ev_t ev;
    if (!map_key(msg, &ev)) {
        return;
    }
    if (s_nav.page == COMPANION_PAGE_HOME || s_nav.page == COMPANION_PAGE_BROWSE) {
        refresh_count();
    }
    companion_act_t act = companion_nav_handle(&s_nav, ev);
    if (act == COMPANION_ACT_SAVE) {
        bool ok = save_draft();
        act = companion_nav_finish_save(&s_nav, ok);
    }
    note_action(act);
}

static void paint(bool content_dirty, bool battery_dirty) {
    if (content_dirty) {
        rebuild_text();
    }
    if (!bsp_lvgl_lock(40)) {
        return;
    }
    if (content_dirty) {
        apply_text();
    }
    if (battery_dirty) {
        apply_battery();
    }
    apply_pose();
    bsp_lvgl_unlock();
}

static void ui_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    s_active = last;
    bool content_dirty = true;
    bool battery_dirty = true;
    TickType_t battery_at = last;
    for (;;) {
        ui_msg_t msg;
        if (xQueueReceive(s_q, &msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            do {
                handle_msg(&msg);
                content_dirty = true;
            } while (xQueueReceive(s_q, &msg, 0) == pdTRUE);
        }
        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed = (uint32_t)(now - last) * (uint32_t)portTICK_PERIOD_MS;
        last = now;

        if (s_nav.page == COMPANION_PAGE_LISTEN &&
            s_nav.listen_mode == COMPANION_LISTEN_CAPTURE) {
            if (poll_release()) {
                note_action(companion_nav_handle(&s_nav, COMPANION_EV_OK_RELEASE));
                content_dirty = true;
            }
            companion_act_t act = companion_nav_tick(&s_nav, elapsed);
            if (act != COMPANION_ACT_NONE) {
                note_action(act);
                content_dirty = true;
            }
            int sec = (int)(s_nav.listen_ms / 1000u);
            if (sec != s_shown_sec) {
                s_shown_sec = sec;
                content_dirty = true;
            }
        }
        if (s_nav.page == COMPANION_PAGE_LISTEN &&
            s_nav.listen_mode == COMPANION_LISTEN_PROCESS) {
            companion_page_t before = s_nav.page;
            accept_draft();
            if (s_nav.page != before) {
                note_action(COMPANION_ACT_CONFIRM);
                content_dirty = true;
            }
        }

        bool holding = (s_nav.page == COMPANION_PAGE_LISTEN) ||
                       (s_nav.page == COMPANION_PAGE_HOME &&
                        s_nav.home_mode == COMPANION_HOME_CHAT);
        uint32_t idle_ms = (uint32_t)(now - s_active) * (uint32_t)portTICK_PERIOD_MS;
        uint32_t limit = s_nav.page == COMPANION_PAGE_CONFIRM ? 20000u : 30000u;
        uint8_t want = (!holding && idle_ms > limit) ? 12 : 80;
        if (want != s_light) {
            bsp_display_backlight(want);
            s_light = want;
        }
        if ((uint32_t)(now - battery_at) * (uint32_t)portTICK_PERIOD_MS >= 1000u) {
            int soc = bsp_battery_soc();
            battery_at = now;
            if (soc != s_soc) {
                s_soc = soc;
                battery_dirty = true;
            }
        }
        paint(content_dirty, battery_dirty);
        content_dirty = false;
        battery_dirty = false;
    }
}

static void init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs reset: %d", (int)err);
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %d", (int)err);
    }
}

void companion_ui_start(void) {
    init_nvs();
    seed_categories();
    load_order();
    companion_nav_init(&s_nav);
    s_mu = xSemaphoreCreateMutex();
    s_q = xQueueCreate(12, sizeof(ui_msg_t));
    if (!s_mu || !s_q) {
        ESP_LOGE(TAG, "ui queue failed");
        return;
    }
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "lvgl lock failed");
        return;
    }
    bool widgets = create_widgets();
    bsp_lvgl_unlock();
    if (!widgets) {
        ESP_LOGE(TAG, "screen create failed");
        return;
    }
    s_ready = true;
    if (xTaskCreate(ui_task, "companion", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ui task failed");
        s_ready = false;
        return;
    }
    esp_err_t err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %d", (int)err);
    }
    ESP_LOGI(TAG, "ui ready");
}

bool companion_ui_set_link(bool online, const char *ip) {
    if (!s_q) {
        return false;
    }
    ui_msg_t msg = {0};
    msg.type = MSG_LINK;
    msg.online = online ? 1 : 0;
    if (online && ip) {
        snprintf(msg.ip, sizeof(msg.ip), "%.15s", ip);
    }
    return xQueueSend(s_q, &msg, 0) == pdTRUE;
}

uint32_t companion_ui_capture_generation(void) {
    uint32_t generation = 0;
    lock_mu();
    generation = s_generation;
    unlock_mu();
    return generation;
}

bool companion_ui_post_draft(const char *text, size_t len, uint32_t category_id,
                             bool is_new, const char *new_name, uint32_t generation) {
    if (!s_mu || !text || len == 0 || len > sizeof(s_text) || generation == 0) {
        return false;
    }
    if (companion_utf8_check(text, len, false, true) != COMPANION_OK) {
        return false;
    }
    char name[COMPANION_CATEGORY_NAME_MAX + 1];
    memset(name, 0, sizeof(name));
    if (is_new) {
        if (!new_name) {
            return false;
        }
        size_t name_len = strlen(new_name);
        if (name_len == 0 || name_len > COMPANION_CATEGORY_NAME_MAX ||
            companion_utf8_check(new_name, name_len, false, false) != COMPANION_OK) {
            return false;
        }
        memcpy(name, new_name, name_len);
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool ok = false;
    if (!s_latched && !s_text_ready && generation == s_generation) {
        memcpy(s_text, text, len);
        s_text_len = len;
        s_text_is_new = is_new;
        s_text_category = category_id;
        s_text_generation = generation;
        memcpy(s_new_name, name, sizeof(s_new_name));
        s_text_ready = true;
        ok = true;
        ESP_LOGI(TAG, "draft queued bytes=%u", (unsigned)len);
    }
    unlock_mu();
    return ok;
}

bool companion_ui_post_reply(const char *text, size_t len, uint32_t generation) {
    if (!s_mu || !s_q || !text || len == 0 || generation == 0) return false;
    size_t show = companion_nav_utf8_prefix(text, len, sizeof(s_reply) - 1);
    if (show == 0) return false;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool ok = false;
    if (generation == s_generation) {
        memcpy(s_reply, text, show);
        s_reply[show] = '\0';
        s_reply_len = show;
        s_reply_gen = generation;
        s_reply_ready = true;
        ok = true;
    }
    unlock_mu();
    if (!ok) return false;
    ui_msg_t msg = {0};
    msg.type = MSG_REPLY;
    msg.gen = generation;
    return xQueueSend(s_q, &msg, 0) == pdTRUE;
}

bool companion_ui_post_fail(uint32_t generation) {
    if (!s_q || generation == 0) return false;
    ui_msg_t msg = {0};
    msg.type = MSG_FAIL;
    msg.gen = generation;
    return xQueueSend(s_q, &msg, 0) == pdTRUE;
}

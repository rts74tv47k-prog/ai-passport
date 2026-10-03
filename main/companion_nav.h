// Button map for the companion. Pure logic: no ESP-IDF, LVGL, or storage I/O.
//
// Pages are home, listen, confirm, and browse. Chat listening is a home mode,
// and "recognizing" is a listen mode, so the device still has four screens.
// Confirm never advances on a timer. A save stays on confirm until
// companion_nav_finish_save() so a failed write can be retried.
//
// OK_RELEASE is a separate event because the board reports long-press start
// while the key is still down and does not report the release itself.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COMPANION_NAV_CATEGORY_MAX 33
#define COMPANION_LISTEN_LIMIT_MS 60000u

typedef enum {
    COMPANION_PAGE_HOME = 0,
    COMPANION_PAGE_LISTEN,
    COMPANION_PAGE_CONFIRM,
    COMPANION_PAGE_BROWSE,
} companion_page_t;

typedef enum {
    COMPANION_HOME_IDLE = 0,
    COMPANION_HOME_CHAT,
} companion_home_mode_t;

typedef enum {
    COMPANION_LISTEN_CAPTURE = 0,
    COMPANION_LISTEN_PROCESS,
} companion_listen_mode_t;

typedef enum {
    COMPANION_EV_OK_SHORT = 0,
    COMPANION_EV_OK_LONG,
    COMPANION_EV_OK_RELEASE,
    COMPANION_EV_UP,
    COMPANION_EV_DOWN,
} companion_ev_t;

typedef enum {
    COMPANION_ACT_NONE = 0,
    COMPANION_ACT_REFRESH,
    COMPANION_ACT_OFFLINE_CHAT,
    COMPANION_ACT_OFFLINE_RECORD,
    COMPANION_ACT_BUSY,
    COMPANION_ACT_CHAT_START,
    COMPANION_ACT_CHAT_END,
    COMPANION_ACT_LISTEN_START,
    COMPANION_ACT_PROCESS,
    COMPANION_ACT_TRUNCATED,
    COMPANION_ACT_CONFIRM,
    COMPANION_ACT_SAVE,
    COMPANION_ACT_DISCARD,
    COMPANION_ACT_SAVED,
    COMPANION_ACT_SAVE_FAIL,
    COMPANION_ACT_FAIL,
    COMPANION_ACT_EDGE,
} companion_act_t;

typedef struct {
    uint32_t id;
    bool is_new;
} companion_choice_t;

typedef struct {
    companion_page_t page;
    companion_home_mode_t home_mode;
    companion_listen_mode_t listen_mode;
    bool online;
    bool truncated;
    bool expanded;
    bool save_pending;
    uint32_t listen_ms;
    int browse_count;
    int browse_pos;
    int choice_count;
    int choice_index;
    companion_choice_t choices[COMPANION_NAV_CATEGORY_MAX];
    uint8_t pulse;
} companion_nav_t;

void companion_nav_init(companion_nav_t *nav);
void companion_nav_set_online(companion_nav_t *nav, bool online);
void companion_nav_set_browse_count(companion_nav_t *nav, int count);
void companion_nav_set_choices(companion_nav_t *nav, const companion_choice_t *items,
                               int count, int selected);

companion_act_t companion_nav_handle(companion_nav_t *nav, companion_ev_t ev);
companion_act_t companion_nav_tick(companion_nav_t *nav, uint32_t elapsed_ms);
companion_act_t companion_nav_process_done(companion_nav_t *nav, bool has_draft);
companion_act_t companion_nav_finish_save(companion_nav_t *nav, bool ok);

int companion_nav_browse_file_index(const companion_nav_t *nav);
const companion_choice_t *companion_nav_choice(const companion_nav_t *nav);

// Move used to the front, or insert it if missing. used == 0 is ignored.
void companion_nav_order_note(uint32_t *ids, size_t *count, size_t cap, uint32_t used);

// saved order first (ids that still exist), then any file ids not listed.
size_t companion_nav_apply_order(uint32_t *dst, size_t dst_cap,
                                 const uint32_t *file_ids, size_t file_n,
                                 const uint32_t *saved, size_t saved_n);

// Largest UTF-8 prefix that fits in max_bytes and does not split a code point.
size_t companion_nav_utf8_prefix(const char *text, size_t len, size_t max_bytes);

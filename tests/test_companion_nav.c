#include <assert.h>
#include <string.h>

#include "companion_nav.h"

static void test_offline_blocks_entry_and_browse_still_works(void) {
    companion_nav_t nav;
    companion_nav_init(&nav);
    companion_nav_set_browse_count(&nav, 3);

    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_OFFLINE_CHAT);
    assert(nav.page == COMPANION_PAGE_HOME);
    assert(nav.home_mode == COMPANION_HOME_IDLE);

    assert(companion_nav_handle(&nav, COMPANION_EV_OK_LONG) == COMPANION_ACT_OFFLINE_RECORD);
    assert(nav.page == COMPANION_PAGE_HOME);

    assert(companion_nav_handle(&nav, COMPANION_EV_UP) == COMPANION_ACT_REFRESH);
    assert(nav.page == COMPANION_PAGE_BROWSE);
    assert(nav.browse_pos == 0);
    assert(companion_nav_browse_file_index(&nav) == 2);

    assert(companion_nav_handle(&nav, COMPANION_EV_UP) == COMPANION_ACT_REFRESH);
    assert(companion_nav_browse_file_index(&nav) == 1);
    assert(companion_nav_handle(&nav, COMPANION_EV_UP) == COMPANION_ACT_REFRESH);
    assert(companion_nav_browse_file_index(&nav) == 0);
    assert(companion_nav_handle(&nav, COMPANION_EV_UP) == COMPANION_ACT_EDGE);
    assert(nav.browse_pos == 2);

    assert(companion_nav_handle(&nav, COMPANION_EV_DOWN) == COMPANION_ACT_REFRESH);
    assert(nav.expanded == false);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_REFRESH);
    assert(nav.expanded == true);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_REFRESH);
    assert(nav.expanded == false);

    assert(companion_nav_handle(&nav, COMPANION_EV_OK_LONG) == COMPANION_ACT_REFRESH);
    assert(nav.page == COMPANION_PAGE_HOME);
}

static void test_empty_browse_and_edges(void) {
    companion_nav_t nav;
    companion_nav_init(&nav);
    assert(companion_nav_handle(&nav, COMPANION_EV_DOWN) == COMPANION_ACT_REFRESH);
    assert(nav.page == COMPANION_PAGE_BROWSE);
    assert(companion_nav_browse_file_index(&nav) == -1);
    assert(companion_nav_handle(&nav, COMPANION_EV_DOWN) == COMPANION_ACT_EDGE);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_EDGE);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_LONG) == COMPANION_ACT_REFRESH);
    assert(nav.page == COMPANION_PAGE_HOME);
}

static void test_chat_excludes_record_and_browse(void) {
    companion_nav_t nav;
    companion_nav_init(&nav);
    companion_nav_set_online(&nav, true);

    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_CHAT_START);
    assert(nav.home_mode == COMPANION_HOME_CHAT);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_LONG) == COMPANION_ACT_BUSY);
    assert(nav.page == COMPANION_PAGE_HOME);
    assert(companion_nav_handle(&nav, COMPANION_EV_UP) == COMPANION_ACT_BUSY);
    assert(nav.page == COMPANION_PAGE_HOME);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_CHAT_END);
    assert(nav.home_mode == COMPANION_HOME_IDLE);

    companion_nav_set_online(&nav, false);
    nav.home_mode = COMPANION_HOME_CHAT;
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_CHAT_END);
}

static void test_record_confirm_save_and_discard(void) {
    companion_nav_t nav;
    companion_choice_t choices[3] = {
        {.id = 0, .is_new = true},
        {.id = 4, .is_new = false},
        {.id = 2, .is_new = false},
    };
    companion_nav_init(&nav);
    companion_nav_set_online(&nav, true);

    assert(companion_nav_handle(&nav, COMPANION_EV_OK_LONG) == COMPANION_ACT_LISTEN_START);
    assert(nav.page == COMPANION_PAGE_LISTEN);
    assert(companion_nav_tick(&nav, 1000) == COMPANION_ACT_NONE);
    assert(nav.listen_ms == 1000);
    assert(companion_nav_handle(&nav, COMPANION_EV_UP) == COMPANION_ACT_BUSY);
    assert(nav.page == COMPANION_PAGE_LISTEN);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_RELEASE) == COMPANION_ACT_PROCESS);
    assert(nav.listen_mode == COMPANION_LISTEN_PROCESS);
    assert(companion_nav_tick(&nav, 5000) == COMPANION_ACT_NONE);
    assert(nav.page == COMPANION_PAGE_LISTEN);

    companion_nav_set_choices(&nav, choices, 3, 0);
    assert(companion_nav_process_done(&nav, true) == COMPANION_ACT_CONFIRM);
    assert(nav.page == COMPANION_PAGE_CONFIRM);
    assert(companion_nav_choice(&nav)->is_new);
    assert(companion_nav_handle(&nav, COMPANION_EV_DOWN) == COMPANION_ACT_REFRESH);
    assert(companion_nav_choice(&nav)->id == 4);
    assert(companion_nav_handle(&nav, COMPANION_EV_DOWN) == COMPANION_ACT_REFRESH);
    assert(companion_nav_handle(&nav, COMPANION_EV_DOWN) == COMPANION_ACT_REFRESH);
    assert(companion_nav_choice(&nav)->is_new);
    assert(companion_nav_handle(&nav, COMPANION_EV_UP) == COMPANION_ACT_REFRESH);
    assert(companion_nav_choice(&nav)->id == 2);

    assert(companion_nav_tick(&nav, 100000) == COMPANION_ACT_NONE);
    assert(nav.page == COMPANION_PAGE_CONFIRM);

    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_SAVE);
    assert(nav.page == COMPANION_PAGE_CONFIRM);
    assert(nav.save_pending);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_NONE);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_LONG) == COMPANION_ACT_BUSY);
    assert(nav.page == COMPANION_PAGE_CONFIRM);
    assert(companion_nav_finish_save(&nav, false) == COMPANION_ACT_SAVE_FAIL);
    assert(nav.page == COMPANION_PAGE_CONFIRM);
    assert(!nav.save_pending);

    assert(companion_nav_handle(&nav, COMPANION_EV_OK_SHORT) == COMPANION_ACT_SAVE);
    assert(companion_nav_finish_save(&nav, true) == COMPANION_ACT_SAVED);
    assert(nav.page == COMPANION_PAGE_HOME);

    assert(companion_nav_handle(&nav, COMPANION_EV_OK_LONG) == COMPANION_ACT_LISTEN_START);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_RELEASE) == COMPANION_ACT_PROCESS);
    assert(companion_nav_process_done(&nav, true) == COMPANION_ACT_CONFIRM);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_LONG) == COMPANION_ACT_DISCARD);
    assert(nav.page == COMPANION_PAGE_HOME);
    assert(!nav.save_pending);
}

static void test_capture_limit_and_failed_process(void) {
    companion_nav_t nav;
    companion_nav_init(&nav);
    companion_nav_set_online(&nav, true);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_LONG) == COMPANION_ACT_LISTEN_START);
    assert(companion_nav_tick(&nav, 59999) == COMPANION_ACT_NONE);
    assert(nav.listen_mode == COMPANION_LISTEN_CAPTURE);
    assert(companion_nav_tick(&nav, 1) == COMPANION_ACT_TRUNCATED);
    assert(nav.truncated);
    assert(nav.listen_ms == COMPANION_LISTEN_LIMIT_MS);
    assert(nav.listen_mode == COMPANION_LISTEN_PROCESS);
    assert(companion_nav_handle(&nav, COMPANION_EV_OK_RELEASE) == COMPANION_ACT_NONE);
    assert(companion_nav_process_done(&nav, false) == COMPANION_ACT_FAIL);
    assert(nav.page == COMPANION_PAGE_HOME);
    assert(companion_nav_process_done(&nav, true) == COMPANION_ACT_NONE);
}

static void test_order_and_utf8(void) {
    uint32_t ids[2] = {1, 2};
    size_t count = 2;
    companion_nav_order_note(ids, &count, 2, 2);
    assert(count == 2);
    assert(ids[0] == 2 && ids[1] == 1);
    companion_nav_order_note(ids, &count, 2, 9);
    assert(count == 2);
    assert(ids[0] == 9 && ids[1] == 2);

    uint32_t file_ids[3] = {1, 2, 3};
    uint32_t saved[4] = {3, 9, 1, 3};
    uint32_t dst[3] = {0, 0, 0};
    size_t n = companion_nav_apply_order(dst, 3, file_ids, 3, saved, 4);
    assert(n == 3);
    assert(dst[0] == 3 && dst[1] == 1 && dst[2] == 2);

    assert(companion_nav_utf8_prefix(NULL, 4, 4) == 0);
    assert(companion_nav_utf8_prefix("abc", 3, 2) == 2);
    const char nihao[] = "\xe4\xbd\xa0\xe5\xa5\xbd";
    assert(companion_nav_utf8_prefix(nihao, 6, 4) == 3);
    assert(companion_nav_utf8_prefix(nihao, 6, 6) == 6);
    assert(companion_nav_utf8_prefix(nihao, 6, 0) == 0);
}

int main(void) {
    test_offline_blocks_entry_and_browse_still_works();
    test_empty_browse_and_edges();
    test_chat_excludes_record_and_browse();
    test_record_confirm_save_and_discard();
    test_capture_limit_and_failed_process();
    test_order_and_utf8();
    return 0;
}

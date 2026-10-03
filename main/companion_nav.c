#include "companion_nav.h"

#include <string.h>

static companion_act_t feedback(companion_nav_t *nav, companion_act_t act) {
    if (act != COMPANION_ACT_NONE) {
        nav->pulse++;
    }
    return act;
}

static void go_home(companion_nav_t *nav) {
    nav->page = COMPANION_PAGE_HOME;
    nav->home_mode = COMPANION_HOME_IDLE;
    nav->listen_mode = COMPANION_LISTEN_CAPTURE;
    nav->listen_ms = 0;
    nav->truncated = false;
    nav->save_pending = false;
    nav->expanded = false;
}

static companion_act_t enter_listen(companion_nav_t *nav) {
    nav->page = COMPANION_PAGE_LISTEN;
    nav->listen_mode = COMPANION_LISTEN_CAPTURE;
    nav->listen_ms = 0;
    nav->truncated = false;
    nav->save_pending = false;
    return feedback(nav, COMPANION_ACT_LISTEN_START);
}

static companion_act_t enter_browse(companion_nav_t *nav) {
    nav->page = COMPANION_PAGE_BROWSE;
    nav->browse_pos = 0;
    nav->expanded = false;
    return feedback(nav, COMPANION_ACT_REFRESH);
}

static companion_act_t enter_process(companion_nav_t *nav, bool truncated) {
    nav->listen_mode = COMPANION_LISTEN_PROCESS;
    if (truncated) {
        nav->listen_ms = COMPANION_LISTEN_LIMIT_MS;
        nav->truncated = true;
        return feedback(nav, COMPANION_ACT_TRUNCATED);
    }
    return feedback(nav, COMPANION_ACT_PROCESS);
}

static companion_act_t on_home(companion_nav_t *nav, companion_ev_t ev) {
    if (ev == COMPANION_EV_OK_SHORT) {
        if (nav->home_mode == COMPANION_HOME_CHAT) {
            nav->home_mode = COMPANION_HOME_IDLE;
            return feedback(nav, COMPANION_ACT_CHAT_END);
        }
        if (!nav->online) {
            return feedback(nav, COMPANION_ACT_OFFLINE_CHAT);
        }
        nav->home_mode = COMPANION_HOME_CHAT;
        return feedback(nav, COMPANION_ACT_CHAT_START);
    }
    if (ev == COMPANION_EV_OK_LONG) {
        if (nav->home_mode == COMPANION_HOME_CHAT) {
            return feedback(nav, COMPANION_ACT_BUSY);
        }
        if (!nav->online) {
            return feedback(nav, COMPANION_ACT_OFFLINE_RECORD);
        }
        return enter_listen(nav);
    }
    if (ev == COMPANION_EV_UP || ev == COMPANION_EV_DOWN) {
        if (nav->home_mode == COMPANION_HOME_CHAT) {
            return feedback(nav, COMPANION_ACT_BUSY);
        }
        return enter_browse(nav);
    }
    return COMPANION_ACT_NONE;
}

static companion_act_t on_listen(companion_nav_t *nav, companion_ev_t ev) {
    if (nav->listen_mode == COMPANION_LISTEN_PROCESS) {
        if (ev == COMPANION_EV_OK_RELEASE) {
            return COMPANION_ACT_NONE;
        }
        return feedback(nav, COMPANION_ACT_BUSY);
    }
    if (ev == COMPANION_EV_OK_RELEASE) {
        return enter_process(nav, false);
    }
    if (ev == COMPANION_EV_OK_LONG) {
        return COMPANION_ACT_NONE;
    }
    return feedback(nav, COMPANION_ACT_BUSY);
}

static companion_act_t on_confirm(companion_nav_t *nav, companion_ev_t ev) {
    if (ev == COMPANION_EV_UP || ev == COMPANION_EV_DOWN) {
        if (nav->choice_count <= 0) {
            return feedback(nav, COMPANION_ACT_EDGE);
        }
        if (ev == COMPANION_EV_UP) {
            nav->choice_index = (nav->choice_index + nav->choice_count - 1) % nav->choice_count;
        } else {
            nav->choice_index = (nav->choice_index + 1) % nav->choice_count;
        }
        return feedback(nav, COMPANION_ACT_REFRESH);
    }
    if (ev == COMPANION_EV_OK_SHORT) {
        if (nav->save_pending) {
            return COMPANION_ACT_NONE;
        }
        if (nav->choice_count <= 0) {
            return feedback(nav, COMPANION_ACT_SAVE_FAIL);
        }
        nav->save_pending = true;
        return feedback(nav, COMPANION_ACT_SAVE);
    }
    if (ev == COMPANION_EV_OK_LONG) {
        if (nav->save_pending) {
            return feedback(nav, COMPANION_ACT_BUSY);
        }
        go_home(nav);
        return feedback(nav, COMPANION_ACT_DISCARD);
    }
    return COMPANION_ACT_NONE;
}

static companion_act_t on_browse(companion_nav_t *nav, companion_ev_t ev) {
    if (ev == COMPANION_EV_UP) {
        if (nav->browse_pos + 1 >= nav->browse_count) {
            return feedback(nav, COMPANION_ACT_EDGE);
        }
        nav->browse_pos++;
        nav->expanded = false;
        return feedback(nav, COMPANION_ACT_REFRESH);
    }
    if (ev == COMPANION_EV_DOWN) {
        if (nav->browse_pos <= 0) {
            return feedback(nav, COMPANION_ACT_EDGE);
        }
        nav->browse_pos--;
        nav->expanded = false;
        return feedback(nav, COMPANION_ACT_REFRESH);
    }
    if (ev == COMPANION_EV_OK_SHORT) {
        if (nav->browse_count <= 0) {
            return feedback(nav, COMPANION_ACT_EDGE);
        }
        nav->expanded = !nav->expanded;
        return feedback(nav, COMPANION_ACT_REFRESH);
    }
    if (ev == COMPANION_EV_OK_LONG) {
        go_home(nav);
        return feedback(nav, COMPANION_ACT_REFRESH);
    }
    return COMPANION_ACT_NONE;
}

void companion_nav_init(companion_nav_t *nav) {
    if (!nav) {
        return;
    }
    memset(nav, 0, sizeof(*nav));
}

void companion_nav_set_online(companion_nav_t *nav, bool online) {
    if (nav) {
        nav->online = online;
    }
}

void companion_nav_set_browse_count(companion_nav_t *nav, int count) {
    if (!nav) {
        return;
    }
    if (count < 0) {
        count = 0;
    }
    nav->browse_count = count;
    if (count == 0 || nav->browse_pos < 0) {
        nav->browse_pos = 0;
    } else if (nav->browse_pos >= count) {
        nav->browse_pos = count - 1;
    }
}

void companion_nav_set_choices(companion_nav_t *nav, const companion_choice_t *items,
                               int count, int selected) {
    if (!nav) {
        return;
    }
    if (!items || count < 0) {
        count = 0;
    }
    if (count > COMPANION_NAV_CATEGORY_MAX) {
        count = COMPANION_NAV_CATEGORY_MAX;
    }
    for (int i = 0; i < count; i++) {
        nav->choices[i] = items[i];
    }
    nav->choice_count = count;
    if (count == 0 || selected < 0 || selected >= count) {
        nav->choice_index = 0;
    } else {
        nav->choice_index = selected;
    }
}

companion_act_t companion_nav_handle(companion_nav_t *nav, companion_ev_t ev) {
    if (!nav) {
        return COMPANION_ACT_NONE;
    }
    switch (nav->page) {
    case COMPANION_PAGE_HOME:
        return on_home(nav, ev);
    case COMPANION_PAGE_LISTEN:
        return on_listen(nav, ev);
    case COMPANION_PAGE_CONFIRM:
        return on_confirm(nav, ev);
    case COMPANION_PAGE_BROWSE:
        return on_browse(nav, ev);
    }
    return COMPANION_ACT_NONE;
}

companion_act_t companion_nav_tick(companion_nav_t *nav, uint32_t elapsed_ms) {
    if (!nav || nav->page != COMPANION_PAGE_LISTEN ||
        nav->listen_mode != COMPANION_LISTEN_CAPTURE || elapsed_ms == 0) {
        return COMPANION_ACT_NONE;
    }
    if (elapsed_ms > COMPANION_LISTEN_LIMIT_MS) {
        elapsed_ms = COMPANION_LISTEN_LIMIT_MS;
    }
    if (nav->listen_ms >= COMPANION_LISTEN_LIMIT_MS ||
        elapsed_ms >= COMPANION_LISTEN_LIMIT_MS - nav->listen_ms) {
        return enter_process(nav, true);
    }
    nav->listen_ms += elapsed_ms;
    return COMPANION_ACT_NONE;
}

companion_act_t companion_nav_process_done(companion_nav_t *nav, bool has_draft) {
    if (!nav || nav->page != COMPANION_PAGE_LISTEN ||
        nav->listen_mode != COMPANION_LISTEN_PROCESS) {
        return COMPANION_ACT_NONE;
    }
    if (!has_draft) {
        go_home(nav);
        return feedback(nav, COMPANION_ACT_FAIL);
    }
    nav->page = COMPANION_PAGE_CONFIRM;
    nav->save_pending = false;
    nav->listen_mode = COMPANION_LISTEN_CAPTURE;
    nav->listen_ms = 0;
    return feedback(nav, COMPANION_ACT_CONFIRM);
}

companion_act_t companion_nav_finish_save(companion_nav_t *nav, bool ok) {
    if (!nav || nav->page != COMPANION_PAGE_CONFIRM || !nav->save_pending) {
        return COMPANION_ACT_NONE;
    }
    nav->save_pending = false;
    if (!ok) {
        return feedback(nav, COMPANION_ACT_SAVE_FAIL);
    }
    go_home(nav);
    return feedback(nav, COMPANION_ACT_SAVED);
}

int companion_nav_browse_file_index(const companion_nav_t *nav) {
    if (!nav || nav->browse_count <= 0 || nav->browse_pos < 0 ||
        nav->browse_pos >= nav->browse_count) {
        return -1;
    }
    return nav->browse_count - 1 - nav->browse_pos;
}

const companion_choice_t *companion_nav_choice(const companion_nav_t *nav) {
    if (!nav || nav->choice_index < 0 || nav->choice_index >= nav->choice_count) {
        return NULL;
    }
    return &nav->choices[nav->choice_index];
}

static bool contains_id(const uint32_t *ids, size_t count, uint32_t id) {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

void companion_nav_order_note(uint32_t *ids, size_t *count, size_t cap, uint32_t used) {
    if (!ids || !count || cap == 0 || used == 0) {
        return;
    }
    size_t n = *count;
    if (n > cap) {
        n = cap;
    }
    size_t found = n;
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == used) {
            found = i;
            break;
        }
    }
    if (found == n) {
        if (n == cap) {
            n = cap - 1;
        }
        memmove(&ids[1], &ids[0], n * sizeof(uint32_t));
        ids[0] = used;
        *count = n + 1;
        return;
    }
    if (found == 0) {
        *count = n;
        return;
    }
    uint32_t id = ids[found];
    memmove(&ids[1], &ids[0], found * sizeof(uint32_t));
    ids[0] = id;
    *count = n;
}

size_t companion_nav_apply_order(uint32_t *dst, size_t dst_cap,
                                 const uint32_t *file_ids, size_t file_n,
                                 const uint32_t *saved, size_t saved_n) {
    size_t n = 0;
    if (!dst || dst_cap == 0) {
        return 0;
    }
    if (saved) {
        for (size_t i = 0; i < saved_n && n < dst_cap; i++) {
            if (saved[i] == 0) {
                continue;
            }
            if (!file_ids || !contains_id(file_ids, file_n, saved[i])) {
                continue;
            }
            if (contains_id(dst, n, saved[i])) {
                continue;
            }
            dst[n++] = saved[i];
        }
    }
    if (file_ids) {
        for (size_t i = 0; i < file_n && n < dst_cap; i++) {
            if (file_ids[i] == 0 || contains_id(dst, n, file_ids[i])) {
                continue;
            }
            dst[n++] = file_ids[i];
        }
    }
    return n;
}

size_t companion_nav_utf8_prefix(const char *text, size_t len, size_t max_bytes) {
    size_t i = 0;
    if (!text) {
        return 0;
    }
    while (i < len && i < max_bytes) {
        unsigned char c = (unsigned char)text[i];
        size_t need = 1;
        if (c >= 0xF0) {
            need = 4;
        } else if (c >= 0xE0) {
            need = 3;
        } else if (c >= 0xC0) {
            need = 2;
        } else if (c >= 0x80) {
            break;
        }
        if (i + need > len || i + need > max_bytes) {
            break;
        }
        i += need;
    }
    return i;
}

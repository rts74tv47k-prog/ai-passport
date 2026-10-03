// Companion screens. Call companion_ui_start() once, after LVGL is up.
//
// companion_ui_set_link() and companion_ui_post_draft() are safe from other
// tasks. They queue work and do not touch LVGL objects. The button callback
// only enqueues. Draft text is never written to the log.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void companion_ui_start(void);

bool companion_ui_set_link(bool online);

// Generation of the capture that is in progress. Zero means none.
uint32_t companion_ui_capture_generation(void);

// Hand a transcript to the confirm page. generation must match the capture
// that is still in the recognizing state. is_new puts that name first.
bool companion_ui_post_draft(const char *text, size_t len, uint32_t category_id,
                             bool is_new, const char *new_name, uint32_t generation);

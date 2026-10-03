// Cloud speech and chat. Call companion_ai_note() from the UI task only.
// The worker never touches LVGL and never logs transcripts, replies, or the API key.

#pragma once

#include "companion_nav.h"

#include <stdint.h>

void companion_ai_start(void);
void companion_ai_note(companion_act_t act, uint32_t generation);

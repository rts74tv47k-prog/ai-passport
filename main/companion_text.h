// Host-tested text helpers for the LAN page and the notes export.
// Callers must not log decoded form values: they can be a persona or a password.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Decode one application/x-www-form-urlencoded field into dst.
// Returns false when the key is absent or the decoded value does not fit.
bool companion_form_get(const char *body, size_t len, const char *key,
                        char *dst, size_t cap, size_t *out_len);

// Escape &, <, >, and " for an HTML text or attribute context.
bool companion_html_escape(const char *src, size_t len, char *dst, size_t cap);

// "[MM-DD HH:MM] " or "[--] " when the clock is unset.
bool companion_note_stamp(char *dst, size_t cap, uint32_t unix_time);

// "== name ==\n". Rejects a name that contains a newline.
bool companion_note_heading(char *dst, size_t cap, const char *name);

// passport-notes-YYYY-MM-DD.txt, or passport-notes.txt when the clock is unset.
bool companion_note_filename(char *dst, size_t cap, uint32_t unix_time);

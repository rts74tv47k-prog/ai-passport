// Pull text and a classification object out of a model JSON response.
// The input is not logged by this module.

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    COMPANION_CLASS_DEFAULT = 0,
    COMPANION_CLASS_EXISTING = 1,
    COMPANION_CLASS_NEW = 2,
} companion_class_kind_t;

// Prefers a "content" string, then a "text" string. Decodes JSON escapes.
bool companion_ai_parse_reply(const char *json, size_t len, char *dst, size_t cap, size_t *out_len);

// kind is existing, new, or default. name is empty when the object has none.
bool companion_ai_parse_class(const char *json, size_t len, companion_class_kind_t *kind,
                              char *name, size_t name_cap, size_t *out_name_len);

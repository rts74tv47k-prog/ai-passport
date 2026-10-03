// API key in NVS. The value is never written to the log.

#pragma once

#include <stdbool.h>
#include <stddef.h>

bool companion_secret_set(const char *key, size_t len);
bool companion_secret_get(char *dst, size_t cap, size_t *out_len);
bool companion_secret_is_set(void);

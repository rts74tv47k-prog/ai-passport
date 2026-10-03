#include "companion_text.h"

#include "companion_store.h"

#include <stdio.h>
#include <string.h>

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool url_decode(const char *src, size_t len, char *dst, size_t cap, size_t *out_len) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%') {
            if (i + 2 >= len) return false;
            int hi = hex_nibble(src[i + 1]);
            int lo = hex_nibble(src[i + 2]);
            if (hi < 0 || lo < 0) return false;
            ch = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (ch == 0) return false;
        if (o + 1 >= cap) return false;
        dst[o++] = (char)ch;
    }
    dst[o] = '\0';
    if (out_len) *out_len = o;
    return true;
}

bool companion_form_get(const char *body, size_t len, const char *key,
                        char *dst, size_t cap, size_t *out_len) {
    if (!body || !key || !dst || cap == 0) return false;
    size_t key_len = strlen(key);
    if (key_len == 0) return false;
    size_t i = 0;
    while (i < len) {
        size_t name = i;
        while (i < len && body[i] != '=' && body[i] != '&') i++;
        bool match = (i < len && body[i] == '=' && (i - name) == key_len &&
                      memcmp(body + name, key, key_len) == 0);
        if (i < len && body[i] == '=') i++;
        size_t val = i;
        while (i < len && body[i] != '&') i++;
        if (match) {
            return url_decode(body + val, i - val, dst, cap, out_len);
        }
        if (i < len && body[i] == '&') i++;
    }
    return false;
}

bool companion_html_escape(const char *src, size_t len, char *dst, size_t cap) {
    if (!src || !dst || cap == 0) return false;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        const char *rep = NULL;
        size_t n = 1;
        char one[2];
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
        } else {
            one[0] = src[i];
            one[1] = '\0';
            rep = one;
        }
        if (o + n >= cap) return false;
        memcpy(dst + o, rep, n);
        o += n;
    }
    dst[o] = '\0';
    return true;
}

bool companion_note_stamp(char *dst, size_t cap, uint32_t unix_time) {
    if (!dst || cap < 6) return false;
    if (unix_time == 0) {
        memcpy(dst, "[--] ", 6);
        return true;
    }
    char raw[20];
    companion_format_unix_utc(unix_time, raw);
    if (raw[4] != '-' || strlen(raw) < 16) {
        memcpy(dst, "[--] ", 6);
        return true;
    }
    int n = snprintf(dst, cap, "[%.5s %.5s] ", raw + 5, raw + 11);
    return n == 14 && (size_t)n < cap;
}

bool companion_note_heading(char *dst, size_t cap, const char *name) {
    if (!dst || !name || name[0] == '\0') return false;
    if (strchr(name, '\n') || strchr(name, '\r')) return false;
    int n = snprintf(dst, cap, "== %s ==\n", name);
    return n > 6 && (size_t)n < cap;
}

bool companion_note_filename(char *dst, size_t cap, uint32_t unix_time) {
    if (!dst || cap < 32) return false;
    if (unix_time == 0) {
        memcpy(dst, "passport-notes.txt", 19);
        return true;
    }
    char raw[20];
    companion_format_unix_utc(unix_time, raw);
    if (raw[4] != '-' || strlen(raw) < 10) {
        memcpy(dst, "passport-notes.txt", 19);
        return true;
    }
    int n = snprintf(dst, cap, "passport-notes-%.10s.txt", raw);
    return n > 0 && (size_t)n < cap;
}

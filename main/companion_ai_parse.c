#include "companion_ai_parse.h"

#include <string.h>

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int utf8_put(unsigned cp, char *dst) {
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp > 0xFFFF) return 0;
    dst[0] = (char)(0xE0 | (cp >> 12));
    dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static int find_key(const char *json, size_t len, const char *key) {
    size_t key_len = strlen(key);
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < len; i++) {
        char c = json[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }
        if (c != '"') continue;
        if (i + key_len + 1 < len && memcmp(json + i + 1, key, key_len) == 0 &&
            json[i + 1 + key_len] == '"') {
            size_t j = i + key_len + 2;
            while (j < len && (json[j] == ' ' || json[j] == '\n' || json[j] == '\r' || json[j] == '\t')) {
                j++;
            }
            if (j < len && json[j] == ':') return (int)(j + 1);
        }
        in_string = true;
    }
    return -1;
}

static bool decode_string(const char *json, size_t len, size_t i, char *dst, size_t cap, size_t *out_len) {
    while (i < len && json[i] != '"') {
        if (json[i] != ' ' && json[i] != '\n' && json[i] != '\r' && json[i] != '\t') return false;
        i++;
    }
    if (i >= len || json[i] != '"') return false;
    i++;
    size_t o = 0;
    while (i < len) {
        unsigned char c = (unsigned char)json[i++];
        if (c == '"') {
            if (o >= cap) return false;
            dst[o] = '\0';
            if (out_len) *out_len = o;
            return true;
        }
        if (c == '\\') {
            if (i >= len) return false;
            char esc = json[i++];
            if (esc == 'n') c = '\n';
            else if (esc == 'r') c = '\r';
            else if (esc == 't') c = '\t';
            else if (esc == '"' || esc == '\\' || esc == '/') c = (unsigned char)esc;
            else if (esc == 'u') {
                if (i + 4 > len) return false;
                unsigned cp = 0;
                for (int n = 0; n < 4; n++) {
                    int h = hex_nibble(json[i++]);
                    if (h < 0) return false;
                    cp = (cp << 4) | (unsigned)h;
                }
                char encoded[3];
                int n = utf8_put(cp, encoded);
                if (n <= 0 || o + (size_t)n >= cap) return false;
                memcpy(dst + o, encoded, (size_t)n);
                o += (size_t)n;
                continue;
            } else {
                return false;
            }
        } else if (c < 0x20) {
            return false;
        }
        if (o + 1 >= cap) return false;
        dst[o++] = (char)c;
    }
    return false;
}

bool companion_ai_parse_reply(const char *json, size_t len, char *dst, size_t cap, size_t *out_len) {
    if (!json || !dst || cap == 0) return false;
    int at = find_key(json, len, "content");
    if (at < 0) at = find_key(json, len, "text");
    if (at < 0) return false;
    return decode_string(json, len, (size_t)at, dst, cap, out_len);
}

bool companion_ai_parse_class(const char *json, size_t len, companion_class_kind_t *kind,
                              char *name, size_t name_cap, size_t *out_name_len) {
    if (!json || !kind || !name || name_cap == 0) return false;
    name[0] = '\0';
    if (out_name_len) *out_name_len = 0;
    int at = find_key(json, len, "kind");
    if (at < 0) return false;
    char kind_text[16];
    size_t kind_len = 0;
    if (!decode_string(json, len, (size_t)at, kind_text, sizeof(kind_text), &kind_len)) return false;
    if (strcmp(kind_text, "existing") == 0) *kind = COMPANION_CLASS_EXISTING;
    else if (strcmp(kind_text, "new") == 0) *kind = COMPANION_CLASS_NEW;
    else if (strcmp(kind_text, "default") == 0) *kind = COMPANION_CLASS_DEFAULT;
    else return false;
    at = find_key(json, len, "name");
    if (at < 0) return true;
    size_t name_len = 0;
    if (!decode_string(json, len, (size_t)at, name, name_cap, &name_len)) return false;
    if (out_name_len) *out_name_len = name_len;
    return true;
}

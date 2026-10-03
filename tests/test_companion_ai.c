#include "companion_ai_parse.h"
#include "companion_wav.h"

#include <assert.h>
#include <string.h>

int main(void) {
    uint8_t hdr[44];
    companion_wav_header(hdr, 32000, 16000);
    companion_wav_info_t info;
    assert(companion_wav_parse(hdr, sizeof(hdr), &info));
    assert(info.sample_rate == 16000 && info.channels == 1 && info.bits == 16);
    assert(info.data_offset == 44 && info.data_bytes == 32000);
    hdr[0] = 'X';
    assert(!companion_wav_parse(hdr, sizeof(hdr), &info));

    char text[64];
    size_t n = 0;
    const char *reply =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\\u4f60\\u597d\"}}]}";
    assert(companion_ai_parse_reply(reply, strlen(reply), text, sizeof(text), &n));
    assert(n == 6 && strcmp(text, "你好") == 0);

    const char *plain = "{\"text\":\"hello\"}";
    assert(companion_ai_parse_reply(plain, strlen(plain), text, sizeof(text), &n));
    assert(strcmp(text, "hello") == 0);

    companion_class_kind_t kind = COMPANION_CLASS_DEFAULT;
    const char *made = "```json\n{\"kind\":\"new\",\"name\":\"健身\"}\n```";
    assert(companion_ai_parse_class(made, strlen(made), &kind, text, sizeof(text), &n));
    assert(kind == COMPANION_CLASS_NEW && strcmp(text, "健身") == 0);

    const char *kept = "{\"kind\":\"existing\",\"name\":\"网球\"}";
    assert(companion_ai_parse_class(kept, strlen(kept), &kind, text, sizeof(text), &n));
    assert(kind == COMPANION_CLASS_EXISTING && strcmp(text, "网球") == 0);

    const char *daily = "{\"kind\":\"default\"}";
    assert(companion_ai_parse_class(daily, strlen(daily), &kind, text, sizeof(text), &n));
    assert(kind == COMPANION_CLASS_DEFAULT && n == 0);
    return 0;
}

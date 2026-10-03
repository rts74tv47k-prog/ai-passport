#include "companion_text.h"

#include <assert.h>
#include <string.h>

int main(void) {
    char out[128];
    size_t n = 0;

    const char *hello = "a=1&b=hello%20x";
    assert(companion_form_get(hello, strlen(hello), "b", out, sizeof(out), &n));
    assert(n == 7 && strcmp(out, "hello x") == 0);
    const char *plus = "q=a%2Bb";
    assert(companion_form_get(plus, strlen(plus), "q", out, sizeof(out), &n));
    assert(strcmp(out, "a+b") == 0);
    const char *daily = "name=%E6%97%A5%E5%B8%B8";
    assert(companion_form_get(daily, strlen(daily), "name", out, sizeof(out), &n));
    assert(n == 6 && strcmp(out, "日常") == 0);
    assert(!companion_form_get("a=1", 3, "missing", out, sizeof(out), &n));
    assert(!companion_form_get("k=%00", 5, "k", out, sizeof(out), &n));
    assert(!companion_form_get("k=abcdef", 8, "k", out, 4, &n));
    assert(companion_form_get("empty=", 6, "empty", out, sizeof(out), &n));
    assert(n == 0 && out[0] == '\0');

    assert(companion_html_escape("a<b>&\"", 6, out, sizeof(out)));
    assert(strcmp(out, "a&lt;b&gt;&amp;&quot;") == 0);
    assert(!companion_html_escape("<<<<", 4, out, 8));

    assert(companion_note_stamp(out, sizeof(out), 0));
    assert(strcmp(out, "[--] ") == 0);
    assert(companion_note_stamp(out, sizeof(out), 1696118400u));
    assert(strcmp(out, "[10-01 00:00] ") == 0);

    assert(companion_note_heading(out, sizeof(out), "日常"));
    assert(strcmp(out, "== 日常 ==\n") == 0);
    assert(!companion_note_heading(out, sizeof(out), "a\nb"));

    assert(companion_note_filename(out, sizeof(out), 0));
    assert(strcmp(out, "passport-notes.txt") == 0);
    assert(companion_note_filename(out, sizeof(out), 1696118400u));
    assert(strcmp(out, "passport-notes-2023-10-01.txt") == 0);
    return 0;
}

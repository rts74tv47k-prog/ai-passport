// Records to a WAV file, asks Zhipu for text, then deletes the audio.
// Chat keeps five turns in RAM only. Nothing here is written to the log
// except status codes, byte counts, and error codes.
#include "companion_ai.h"

#include "companion_ai_parse.h"
#include "companion_nav.h"
#include "companion_secret.h"
#include "companion_store.h"
#include "companion_ui.h"
#include "companion_wav.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ai";
static const char *CAP_PATH = "/store/CAP.WAV";
static const char *TTS_PATH = "/store/TTS.WAV";
static const char *ASR_URL = "https://open.bigmodel.cn/api/paas/v4/audio/transcriptions";
static const char *CHAT_URL = "https://open.bigmodel.cn/api/paas/v4/chat/completions";
static const char *TTS_URL = "https://open.bigmodel.cn/api/paas/v4/audio/speech";

#define SAMPLE_RATE 16000
#define PCM_CHUNK 1024
#define MAX_PCM_BYTES (SAMPLE_RATE * 2 * 60)
#define HIST_SLOTS 10
#define HIST_BYTES 400

typedef struct {
    bool user;
    char text[HIST_BYTES];
    size_t len;
} hist_t;

static QueueHandle_t s_q;
static volatile uint32_t s_gen;
static volatile bool s_chat;
static volatile bool s_cancel;
static bool s_audio_ready;
static hist_t s_hist[HIST_SLOTS];
static int s_hist_n;
static char s_http[4096];
static char s_text[4096];
static char s_persona[4097];
static companion_category_t s_names[COMPANION_CATEGORY_MAX];
static int16_t s_pcm[PCM_CHUNK];

_Static_assert(sizeof(s_http) <= 8192, "http buffer exceeds 8KB");
_Static_assert(sizeof(s_text) <= 8192, "text buffer exceeds 8KB");
_Static_assert(sizeof(s_pcm) <= 8192, "pcm buffer exceeds 8KB");
_Static_assert(sizeof(s_hist) <= 8192, "history exceeds 8KB");
_Static_assert(sizeof(s_persona) <= 8192, "persona buffer exceeds 8KB");
_Static_assert(sizeof(s_names) <= 8192, "category cache exceeds 8KB");

static void drop_file(const char *path) {
    remove(path);
}

static bool audio_open(uint32_t hz) {
    if (!s_audio_ready) {
        if (bsp_audio_init() != ESP_OK) return false;
        s_audio_ready = true;
    } else if (bsp_audio_wake() != ESP_OK) {
        return false;
    }
    return bsp_audio_set_format(hz, 16, 1) == ESP_OK;
}

static void audio_rest(void) {
    if (s_audio_ready) bsp_audio_sleep();
}

static size_t record_wav(uint32_t gen) {
    drop_file(CAP_PATH);
    if (!audio_open(SAMPLE_RATE)) {
        ESP_LOGW(TAG, "audio open failed");
        return 0;
    }
    FILE *file = fopen(CAP_PATH, "wb+");
    if (!file) return 0;
    uint8_t hdr[44];
    companion_wav_header(hdr, 0, SAMPLE_RATE);
    if (fwrite(hdr, 1, sizeof(hdr), file) != sizeof(hdr)) {
        fclose(file);
        drop_file(CAP_PATH);
        return 0;
    }
    size_t pcm = 0;
    TickType_t start = xTaskGetTickCount();
    while (pcm + 2 <= MAX_PCM_BYTES && !s_cancel && s_gen == gen) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(62000)) break;
        size_t samples = PCM_CHUNK;
        size_t remain = (MAX_PCM_BYTES - pcm) / 2u;
        if (samples > remain) samples = remain;
        if (bsp_audio_read(s_pcm, samples * 2u) != ESP_OK) break;
        size_t wrote = fwrite(s_pcm, 1, samples * 2u, file);
        pcm += wrote;
        if (wrote != samples * 2u) break;
    }
    companion_wav_header(hdr, (uint32_t)pcm, SAMPLE_RATE);
    if (fseek(file, 0, SEEK_SET) != 0 || fwrite(hdr, 1, sizeof(hdr), file) != sizeof(hdr)) {
        pcm = 0;
    }
    fflush(file);
    fclose(file);
    ESP_LOGI(TAG, "capture bytes=%u", (unsigned)pcm);
    if (pcm == 0) drop_file(CAP_PATH);
    return pcm;
}

static size_t escaped_len(const char *text, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') n += 2;
        else if (c < 0x20) n += 6;
        else n += 1;
    }
    return n;
}

static bool write_raw(esp_http_client_handle_t client, const void *data, size_t len) {
    const char *bytes = data;
    size_t off = 0;
    while (off < len) {
        int n = esp_http_client_write(client, bytes + off, (int)(len - off));
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static bool write_escaped(esp_http_client_handle_t client, const char *text, size_t len) {
    char buf[64];
    size_t used = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        char one[8];
        size_t n = 1;
        one[0] = (char)c;
        if (c == '"' || c == '\\') {
            one[0] = '\\';
            one[1] = (char)c;
            n = 2;
        } else if (c == '\n') {
            one[0] = '\\';
            one[1] = 'n';
            n = 2;
        } else if (c == '\r') {
            one[0] = '\\';
            one[1] = 'r';
            n = 2;
        } else if (c == '\t') {
            one[0] = '\\';
            one[1] = 't';
            n = 2;
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            one[0] = '\\';
            one[1] = 'u';
            one[2] = '0';
            one[3] = '0';
            one[4] = hex[c >> 4];
            one[5] = hex[c & 0x0f];
            n = 6;
        }
        if (used + n > sizeof(buf)) {
            if (!write_raw(client, buf, used)) return false;
            used = 0;
        }
        memcpy(buf + used, one, n);
        used += n;
    }
    return used == 0 || write_raw(client, buf, used);
}

static bool write_quoted(esp_http_client_handle_t client, const char *text, size_t len) {
    return write_raw(client, "\"", 1) && write_escaped(client, text, len) && write_raw(client, "\"", 1);
}

static esp_http_client_handle_t open_https(const char *url, const char *content_type, int body_len) {
    char key[129];
    size_t key_len = 0;
    if (!companion_secret_get(key, sizeof(key), &key_len)) {
        ESP_LOGW(TAG, "api key missing");
        return NULL;
    }
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    memset(key, 0, sizeof(key));
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        memset(auth, 0, sizeof(auth));
        return NULL;
    }
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", content_type);
    memset(auth, 0, sizeof(auth));
    if (esp_http_client_open(client, body_len) != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

static int finish_json(esp_http_client_handle_t client, size_t *out_len) {
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    size_t got = 0;
    while (got + 1 < sizeof(s_http)) {
        int n = esp_http_client_read(client, s_http + got, (int)(sizeof(s_http) - 1 - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    s_http[got] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (out_len) *out_len = got;
    return status;
}

static bool transcribe(size_t *out_len) {
    *out_len = 0;
    FILE *file = fopen(CAP_PATH, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long file_len = ftell(file);
    if (file_len < 44 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    static const char prefix[] =
        "--CompanionBoundary7k\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "glm-asr\r\n"
        "--CompanionBoundary7k\r\n"
        "Content-Disposition: form-data; name=\"stream\"\r\n\r\n"
        "false\r\n"
        "--CompanionBoundary7k\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"cap.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    static const char suffix[] = "\r\n--CompanionBoundary7k--\r\n";
    int body = (int)(sizeof(prefix) - 1 + (size_t)file_len + sizeof(suffix) - 1);
    esp_http_client_handle_t client = open_https(ASR_URL, "multipart/form-data; boundary=CompanionBoundary7k", body);
    if (!client) {
        fclose(file);
        return false;
    }
    bool ok = write_raw(client, prefix, sizeof(prefix) - 1);
    while (ok) {
        size_t n = fread(s_pcm, 1, sizeof(s_pcm), file);
        if (n == 0) break;
        ok = write_raw(client, s_pcm, n);
    }
    fclose(file);
    if (!ok || !write_raw(client, suffix, sizeof(suffix) - 1)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    size_t got = 0;
    int status = finish_json(client, &got);
    ESP_LOGI(TAG, "asr http=%d bytes=%u", status, (unsigned)got);
    if (status != 200) return false;
    return companion_ai_parse_reply(s_http, got, s_text, sizeof(s_text), out_len) && *out_len > 0;
}

static void remember(bool user, const char *text, size_t len) {
    if (s_hist_n == HIST_SLOTS) {
        memmove(&s_hist[0], &s_hist[1], sizeof(s_hist[0]) * (HIST_SLOTS - 1));
        s_hist_n--;
    }
    hist_t *slot = &s_hist[s_hist_n++];
    slot->user = user;
    slot->len = companion_nav_utf8_prefix(text, len, HIST_BYTES - 1);
    memcpy(slot->text, text, slot->len);
    slot->text[slot->len] = '\0';
}

static size_t load_persona(void) {
    size_t len = 0;
    s_persona[0] = '\0';
    if (!companion_store_ready() ||
        companion_store_persona_get(s_persona, sizeof(s_persona), &len) != COMPANION_OK) {
        len = 0;
    }
    if (len >= sizeof(s_persona)) len = sizeof(s_persona) - 1;
    len = companion_nav_utf8_prefix(s_persona, len, sizeof(s_persona) - 1);
    s_persona[len] = '\0';
    return len;
}

static bool write_message(esp_http_client_handle_t client, const char *role, const char *text, size_t len) {
    return write_raw(client, "{\"role\":", 8) && write_quoted(client, role, strlen(role)) &&
           write_raw(client, ",\"content\":", 11) && write_quoted(client, text, len) &&
           write_raw(client, "}", 1);
}

static size_t message_len(const char *role, const char *text, size_t len) {
    return 8 + (2 + escaped_len(role, strlen(role))) + 11 + (2 + escaped_len(text, len)) + 1;
}

static bool chat_reply(size_t transcript_len, char *reply, size_t reply_cap, size_t *reply_len) {
    *reply_len = 0;
    if (!reply || reply_cap < 2) return false;
    size_t persona_len = load_persona();
    const char *system = persona_len > 0 ? s_persona : "用简短的中文陪伴对话。";
    size_t system_len = persona_len > 0 ? persona_len : strlen(system);
    size_t user_len = companion_nav_utf8_prefix(s_text, transcript_len, 1000);
    static const char head[] = "{\"model\":\"glm-4-flash\",\"messages\":[";
    static const char tail[] = "],\"max_tokens\":180}";
    size_t body = sizeof(head) - 1;
    body += message_len("system", system, system_len);
    for (int i = 0; i < s_hist_n; i++) {
        body += 1 + message_len(s_hist[i].user ? "user" : "assistant", s_hist[i].text, s_hist[i].len);
    }
    body += 1 + message_len("user", s_text, user_len);
    body += sizeof(tail) - 1;
    esp_http_client_handle_t client = open_https(CHAT_URL, "application/json", (int)body);
    if (!client) return false;
    bool ok = write_raw(client, head, sizeof(head) - 1);
    ok = ok && write_message(client, "system", system, system_len);
    for (int i = 0; ok && i < s_hist_n; i++) {
        ok = write_raw(client, ",", 1) &&
             write_message(client, s_hist[i].user ? "user" : "assistant", s_hist[i].text, s_hist[i].len);
    }
    ok = ok && write_raw(client, ",", 1) && write_message(client, "user", s_text, user_len);
    ok = ok && write_raw(client, tail, sizeof(tail) - 1);
    if (!ok) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    size_t got = 0;
    int status = finish_json(client, &got);
    ESP_LOGI(TAG, "chat http=%d bytes=%u", status, (unsigned)got);
    memset(s_persona, 0, sizeof(s_persona));
    if (status != 200) return false;
    return companion_ai_parse_reply(s_http, got, reply, reply_cap, reply_len) && *reply_len > 0;
}

static bool classify(uint32_t *category_id, bool *is_new, char *new_name, size_t name_cap) {
    *category_id = 0;
    *is_new = false;
    if (new_name && name_cap > 0) new_name[0] = '\0';
    size_t count = 0;
    if (companion_store_ready() && companion_store_category_count(&count) == COMPANION_OK) {
        if (count > COMPANION_CATEGORY_MAX) count = COMPANION_CATEGORY_MAX;
        for (size_t i = 0; i < count; i++) {
            if (companion_store_category_at(i, &s_names[i]) != COMPANION_OK) {
                count = i;
                break;
            }
        }
    }
    static const char head[] = "{\"model\":\"glm-4-flash\",\"messages\":[{\"role\":\"user\",\"content\":";
    static const char tail[] = "}],\"max_tokens\":80}";
    static const char lead[] = "只输出一个JSON对象。kind只能是existing、new或default。已有分类：\n";
    static const char mid[] = "转写：\n";
    static const char end[] = "\n拿不准就用default。";
    size_t text_len = strlen(s_text);
    if (text_len > 1000) text_len = companion_nav_utf8_prefix(s_text, text_len, 1000);
    size_t quoted = escaped_len(lead, sizeof(lead) - 1) + escaped_len(mid, sizeof(mid) - 1) +
                    escaped_len(end, sizeof(end) - 1) + escaped_len(s_text, text_len);
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(s_names[i].name);
        quoted += escaped_len(s_names[i].name, n) + escaped_len("\n", 1);
    }
    size_t body = sizeof(head) - 1 + 2 + quoted + sizeof(tail) - 1;
    esp_http_client_handle_t client = open_https(CHAT_URL, "application/json", (int)body);
    if (!client) return false;
    bool ok = write_raw(client, head, sizeof(head) - 1) && write_raw(client, "\"", 1);
    ok = ok && write_escaped(client, lead, sizeof(lead) - 1);
    for (size_t i = 0; ok && i < count; i++) {
        ok = write_escaped(client, s_names[i].name, strlen(s_names[i].name)) && write_escaped(client, "\n", 1);
    }
    ok = ok && write_escaped(client, mid, sizeof(mid) - 1);
    ok = ok && write_escaped(client, s_text, text_len);
    ok = ok && write_escaped(client, end, sizeof(end) - 1);
    ok = ok && write_raw(client, "\"", 1) && write_raw(client, tail, sizeof(tail) - 1);
    if (!ok) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    size_t got = 0;
    int status = finish_json(client, &got);
    ESP_LOGI(TAG, "class http=%d bytes=%u", status, (unsigned)got);
    if (status != 200) return false;
    companion_class_kind_t kind = COMPANION_CLASS_DEFAULT;
    char name[64];
    size_t name_len = 0;
    if (!companion_ai_parse_class(s_http, got, &kind, name, sizeof(name), &name_len)) return false;
    if (kind == COMPANION_CLASS_EXISTING) {
        for (size_t i = 0; i < count; i++) {
            if (strcmp(s_names[i].name, name) == 0) {
                *category_id = s_names[i].id;
                return true;
            }
        }
    } else if (kind == COMPANION_CLASS_NEW && new_name && name_len > 0 && name_len < name_cap &&
               companion_utf8_check(name, name_len, false, false) == COMPANION_OK) {
        bool dup = false;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(s_names[i].name, name) == 0) dup = true;
        }
        if (!dup) {
            memcpy(new_name, name, name_len);
            new_name[name_len] = '\0';
            *is_new = true;
        }
    }
    return true;
}

static void play_wav(uint32_t gen) {
    FILE *file = fopen(TTS_PATH, "rb");
    if (!file) return;
    uint8_t hdr[128];
    size_t hdr_n = fread(hdr, 1, sizeof(hdr), file);
    companion_wav_info_t info;
    if (!companion_wav_parse(hdr, hdr_n, &info) || info.channels != 1 || info.bits != 16 ||
        info.sample_rate < 8000 || info.sample_rate > 48000) {
        fclose(file);
        ESP_LOGI(TAG, "tts skipped");
        return;
    }
    if (fseek(file, (long)info.data_offset, SEEK_SET) != 0 || !audio_open(info.sample_rate)) {
        fclose(file);
        return;
    }
    bsp_audio_set_volume(80);
    size_t left = info.data_bytes;
    while (left > 0 && s_gen == gen) {
        size_t n = left < sizeof(s_pcm) ? left : sizeof(s_pcm);
        size_t got = fread(s_pcm, 1, n, file);
        if (got == 0) break;
        if (bsp_audio_write(s_pcm, got) != ESP_OK) break;
        left -= got;
    }
    fclose(file);
}

static bool speak(const char *text, size_t len, uint32_t gen) {
    drop_file(TTS_PATH);
    len = companion_nav_utf8_prefix(text, len, 300);
    if (len == 0) return false;
    static const char head[] = "{\"model\":\"cogtts\",\"voice\":\"tongtong\",\"input\":";
    static const char tail[] = "}";
    size_t body = sizeof(head) - 1 + 2 + escaped_len(text, len) + sizeof(tail) - 1;
    esp_http_client_handle_t client = open_https(TTS_URL, "application/json", (int)body);
    if (!client) return false;
    bool ok = write_raw(client, head, sizeof(head) - 1) && write_quoted(client, text, len) &&
              write_raw(client, tail, sizeof(tail) - 1);
    if (!ok) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "tts http=%d", status);
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    FILE *file = fopen(TTS_PATH, "wb");
    if (!file) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    size_t stored = 0;
    bool wav = false;
    while (stored < 700000 && s_gen == gen) {
        int n = esp_http_client_read(client, (char *)s_pcm, (int)sizeof(s_pcm));
        if (n <= 0) break;
        if (stored == 0) wav = n >= 4 && memcmp(s_pcm, "RIFF", 4) == 0;
        if (!wav) break;
        fwrite(s_pcm, 1, (size_t)n, file);
        stored += (size_t)n;
    }
    fclose(file);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!wav) {
        drop_file(TTS_PATH);
        return false;
    }
    play_wav(gen);
    drop_file(TTS_PATH);
    return true;
}

static void run_job(void) {
    uint32_t gen = s_gen;
    bool chat = s_chat;
    if (s_gen != gen) return;
    size_t pcm = record_wav(gen);
    if (s_gen != gen) {
        drop_file(CAP_PATH);
        audio_rest();
        return;
    }
    size_t text_len = 0;
    bool heard = pcm >= 1600 && transcribe(&text_len);
    drop_file(CAP_PATH);
    memset(s_pcm, 0, sizeof(s_pcm));
    if (s_gen != gen) {
        audio_rest();
        return;
    }
    if (!heard) {
        companion_ui_post_fail(gen);
        audio_rest();
        memset(s_http, 0, sizeof(s_http));
        memset(s_text, 0, sizeof(s_text));
        return;
    }
    if (chat) {
        char reply[HIST_BYTES];
        size_t reply_len = 0;
        bool ok = chat_reply(text_len, reply, sizeof(reply), &reply_len);
        if (s_gen == gen && ok) {
            remember(true, s_text, text_len);
            remember(false, reply, reply_len);
            companion_ui_post_reply(reply, reply_len, gen);
            if (s_gen == gen) speak(reply, reply_len, gen);
        } else if (s_gen == gen) {
            companion_ui_post_fail(gen);
        }
    } else {
        uint32_t category = 0;
        bool is_new = false;
        char new_name[64];
        new_name[0] = '\0';
        classify(&category, &is_new, new_name, sizeof(new_name));
        if (s_gen == gen &&
            !companion_ui_post_draft(s_text, text_len, category, is_new, new_name, gen)) {
            companion_ui_post_fail(gen);
        }
    }
    memset(s_http, 0, sizeof(s_http));
    memset(s_text, 0, sizeof(s_text));
    audio_rest();
}

static void ai_task(void *arg) {
    (void)arg;
    for (;;) {
        uint8_t wake = 0;
        if (xQueueReceive(s_q, &wake, portMAX_DELAY) != pdTRUE) continue;
        run_job();
    }
}

void companion_ai_start(void) {
    if (s_q) return;
    s_q = xQueueCreate(1, sizeof(uint8_t));
    if (!s_q || xTaskCreate(ai_task, "companion-ai", 12288, NULL, 4, NULL) != pdPASS) {
        if (s_q) vQueueDelete(s_q);
        s_q = NULL;
        ESP_LOGE(TAG, "ai task failed");
    }
}

void companion_ai_note(companion_act_t act, uint32_t generation) {
    if (!s_q) return;
    if (act == COMPANION_ACT_LISTEN_START || act == COMPANION_ACT_CHAT_START) {
        s_chat = act == COMPANION_ACT_CHAT_START;
        s_gen = generation;
        s_cancel = false;
        uint8_t wake = 1;
        xQueueSend(s_q, &wake, 0);
    } else if (act == COMPANION_ACT_PROCESS || act == COMPANION_ACT_TRUNCATED ||
               act == COMPANION_ACT_CHAT_END) {
        s_cancel = true;
    }
}

#include "odin.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "net.h"
#include "settings.h"
#include "board.h"
#include "ui.h"
#include "wav.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include "esp_lvgl_port.h"
#include "remote.h"
#include "microphone.h"

static const char *TAG = "odin";

static _Atomic odin_state_t s_state = ODIN_STANDBY;
static atomic_bool s_abort;
static atomic_bool s_stop_recording;
static atomic_bool s_llm_online;
static atomic_int s_tts_state;     /* 0 checking, 1 ok, 2 unreachable */
static TaskHandle_t  s_monitor;

#define ODIN_MAX_MODELS 10
static char s_models[ODIN_MAX_MODELS][96];
static int  s_model_count;
static atomic_bool s_fetch_models;
static atomic_bool s_test_tts;
static atomic_uint s_service_revision;
static char s_tts_detail[96] = "Not tested";
static void set_state(odin_state_t st);
static bool tts_speak(const char *text);

bool odin_llm_online(void) { return s_llm_online; }
int  odin_tts_state(void)  { return s_tts_state; }
void odin_tts_detail(char *out, size_t size)
{
    settings_lock(); strlcpy(out, s_tts_detail, size); settings_unlock();
}
static void tts_result(int state, const char *detail)
{
    settings_lock(); strlcpy(s_tts_detail, detail, sizeof s_tts_detail); settings_unlock();
    s_tts_state = state;
}
int odin_state(void) { return atomic_load(&s_state); }
void odin_tts_settings_changed(void)
{
    tts_result(0, "Not tested");
    atomic_fetch_add(&s_service_revision, 1);
    if (s_monitor) xTaskNotifyGive(s_monitor);
}

int odin_model_count(void) { return s_model_count; }
const char *odin_model_name(int idx)
{
    if (idx < 0 || idx >= s_model_count) return "";
    return s_models[idx];
}
const char *odin_selected_model(void)
{
    return g_cfg.model[0] ? g_cfg.model : "(no model)";
}
void odin_select_model(int idx)
{
    if (idx < 0 || idx >= s_model_count) return;
    settings_lock();
    g_cfg.model_idx = (uint8_t)idx;
    strlcpy(g_cfg.model, s_models[idx], sizeof g_cfg.model);
    strlcpy(g_cfg.providers[g_cfg.provider].model, g_cfg.model, sizeof g_cfg.providers[0].model);
    settings_save(); settings_unlock();
    atomic_fetch_add(&s_service_revision, 1);
    if (s_monitor) xTaskNotifyGive(s_monitor);
}
void odin_fetch_models(void) { s_fetch_models = true; if (s_monitor) xTaskNotifyGive(s_monitor); }

static bool fetch_models_list(void)
{
    odin_settings_t *cfg = malloc(sizeof *cfg);
    if (!cfg) return false;
    settings_snapshot(cfg);
    odin_provider_t *provider = &cfg->providers[cfg->provider];
    char url[256];
    bool ok = false;
    if (!remote_url(url, sizeof url, provider->url, "/models")) goto done;
    int status;
    cJSON *root = remote_json(url, provider->key, NULL, &status);
    cJSON *data = cJSON_GetObjectItem(root, "data");
    char models[ODIN_MAX_MODELS][96] = {{0}};
    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, data) {
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id) || !id->valuestring || strstr(id->valuestring, "embed")) continue;
        if (count < ODIN_MAX_MODELS) strlcpy(models[count++], id->valuestring, sizeof models[0]);
    }
    cJSON_Delete(root);
    if (!count) goto done;
    lvgl_port_lock(0);
    settings_lock();
    if (cfg->provider == g_cfg.provider && !strcmp(provider->url, g_cfg.providers[g_cfg.provider].url)) {
        memcpy(s_models, models, sizeof s_models); s_model_count = count;
        bool found = false;
        for (int i = 0; i < count; i++) if (!strcmp(models[i], g_cfg.model)) { g_cfg.model_idx = i; found = true; break; }
        if (!found && (!g_cfg.model[0] || g_cfg.model[0] == '(')) {
            g_cfg.model_idx = 0;
            strlcpy(g_cfg.model, models[0], sizeof g_cfg.model);
            strlcpy(g_cfg.providers[g_cfg.provider].model, g_cfg.model, sizeof g_cfg.providers[0].model);
        }
        ok = true;
    }
    settings_unlock();
    if (ok) ui_refresh_model_list();
    lvgl_port_unlock();
done:
    free(cfg); return ok;
}

void odin_settings_changed(void)
{
    atomic_fetch_add(&s_service_revision, 1);
    lvgl_port_lock(0);
    s_model_count = 0; memset(s_models, 0, sizeof s_models);
    ui_refresh_model_list();
    lvgl_port_unlock();
    s_llm_online = false;
    odin_fetch_models();
}
/* Called only by the monitor; use the same session reservation as TALK and
 * manual TTS tests so automatic speech cannot overlap recording or playback. */
static void announce_services(void)
{
    static unsigned revision;
    static bool announced_tts, announced_llm, verified_llm;
    static int64_t retry_at;
    unsigned current = atomic_load(&s_service_revision);
    if (current != revision) {
        revision = current;
        announced_tts = announced_llm = verified_llm = false;
        retry_at = 0;
    }
    if ((announced_tts && announced_llm) || esp_timer_get_time() < retry_at || !board_get_volume()) return;
    lvgl_port_lock(0);
    odin_state_t expected = ODIN_STANDBY;
    if (!atomic_compare_exchange_strong(&s_state, &expected, ODIN_PROCESSING)) {
        lvgl_port_unlock(); return;
    }
    s_abort = false;
    ui_set_state(ODIN_PROCESSING);
    lvgl_port_unlock();
    /* A model listing alone does not establish that inference works. */
    if (!announced_llm) {
        char reply[128];
        verified_llm = odin_chat_test("Reply with OK.", reply, sizeof reply);
    }
    lvgl_port_lock(0);
    bool current_settings = revision == atomic_load(&s_service_revision);
    if (current_settings) set_state(ODIN_RESPONDING);
    lvgl_port_unlock();
    if (current_settings && (!announced_tts || (verified_llm && !announced_llm))) {
        tts_result(0, "Confirming services with speech...");
        const char *text = verified_llm ?
            "The language model is online. The text to speech server is online. Odin is ready." :
            "The text to speech server is online.";
        bool played = tts_speak(text);
        if (played && board_get_volume()) {
            announced_tts = true;
            if (verified_llm) announced_llm = true;
        } else if (s_tts_state == 0) tts_result(2, "Could not play service confirmation");
    }
    /* Retry failed services at most once per five minutes. Successful
     * confirmations are not repeated until reboot or settings change. */
    retry_at = esp_timer_get_time() + 300000000;
    set_state(ODIN_STANDBY);
}

static void monitor_task(void *arg)
{
    for (;;) {
        if (net_connected()) {
            if (atomic_exchange(&s_test_tts, false)) {
                tts_result(0, "Generating test speech...");
                bool ok = tts_speak("Odin speech test. The speech server is working.");
                if (!ok && s_tts_state == 0) tts_result(2, "Speech test failed");
                set_state(ODIN_STANDBY);
            }
            if (atomic_exchange(&s_fetch_models, false) || s_model_count == 0) fetch_models_list();
            announce_services();
        } else {
            s_llm_online = false;
            if (atomic_exchange(&s_test_tts, false)) {
                tts_result(2, "Connect to Wi-Fi before testing speech");
                set_state(ODIN_STANDBY);
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
    }
}

/* ------------------------------------------------------------- TTS speak */

/* Fetch WAV audio from Unsloth (POST /v1/audio/speech {"input":...}).
 * Returns a heap buffer holding mono 16-bit PCM and sets rate + frames, or
 * NULL on failure. Handles WAV fmt 1 (PCM16) and fmt 3 (IEEE float32). */
static int16_t *tts_fetch_pcm(const char *text, int *rate, size_t *frames)
{
    odin_settings_t *cfg = malloc(sizeof *cfg);
    if (!cfg) return NULL;
    settings_snapshot(cfg);
    char url[256];
    size_t endpoint_len = strlen(cfg->tts_url);
    while (endpoint_len && cfg->tts_url[endpoint_len - 1] == '/') endpoint_len--;
    bool legacy = endpoint_len >= 4 && !strncmp(cfg->tts_url + endpoint_len - 4, "/tts", 4);
    if (!remote_url(url, sizeof url, cfg->tts_url, legacy ? "" : "/v1/audio/speech")) { free(cfg); return NULL; }
    cJSON *root = cJSON_CreateObject();
    if (!root) { free(cfg); return NULL; }
    cJSON_AddStringToObject(root, legacy ? "text" : "input", text);
    if (!legacy) {
        cJSON_AddStringToObject(root, "response_format", "wav");
        if (cfg->tts_model[0]) cJSON_AddStringToObject(root, "model", cfg->tts_model);
        if (cfg->tts_voice[0]) cJSON_AddStringToObject(root, "voice", cfg->tts_voice);
    }
    char *post = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!post) { free(cfg); return NULL; }
    esp_http_client_config_t hc = {.url = url, .method = HTTP_METHOD_POST,
        .timeout_ms = 5000, .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) { free(post); free(cfg); return NULL; }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    remote_auth(c, cfg->tts_key);
    free(cfg);

    uint8_t *pcm = NULL;
    size_t pcm_len = 0, buf_cap = 0;
    if (esp_http_client_open(c, strlen(post)) == ESP_OK) {
        if (!remote_write_all(c, post, strlen(post))) {
            esp_http_client_close(c); esp_http_client_cleanup(c); free(post);
            tts_result(2, "Could not send speech request"); return NULL;
        }
        /* Poll while the server generates speech, including before headers arrive.
         * IDF preserves parser progress but resets status_code on each fetch. */
        esp_http_client_set_timeout_ms(c, 250);
        int64_t header_deadline = esp_timer_get_time() + 60000000;
        int status = -1;
        int64_t headers;
        do {
            headers = esp_http_client_fetch_headers(c);
            int parsed_status = esp_http_client_get_status_code(c);
            if (parsed_status > 0) status = parsed_status;
        } while (headers == -ESP_ERR_HTTP_EAGAIN && !s_abort &&
                 esp_timer_get_time() < header_deadline);
        if (headers < 0 || s_abort) {
            esp_http_client_close(c); esp_http_client_cleanup(c); free(post);
            tts_result(2, s_abort ? "Speech cancelled" : "Timed out or failed receiving speech headers");
            return NULL;
        }
        if (status == 200) {
            /* Poll body reads so cancellation does not wait for the 45s TTS timeout. */
            esp_http_client_set_timeout_ms(c, 250);
            int64_t deadline = esp_timer_get_time() + 45000000;
            char tmp[3072]; int r;
            while (!s_abort && esp_timer_get_time() < deadline) {
                r = esp_http_client_read(c, tmp, sizeof tmp);
                if (r == -ESP_ERR_HTTP_EAGAIN) continue;
                if (r <= 0) break;
                if (pcm_len + (size_t)r > 8 * 1024 * 1024) break;
                if (pcm_len + (size_t)r > buf_cap) {
                    size_t ncap = buf_cap ? buf_cap * 2 : 65536;
                    while (ncap < pcm_len + (size_t)r) ncap *= 2;
                    uint8_t *nb = heap_caps_realloc(pcm, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (!nb) { heap_caps_free(pcm); pcm = NULL; pcm_len = 0; break; }
                    pcm = nb; buf_cap = ncap;
                }
                memcpy(pcm + pcm_len, tmp, (size_t)r);
                pcm_len += (size_t)r;
            }
            ESP_LOGI(TAG, "tts: %u bytes of audio", (unsigned)pcm_len);
        } else {
            char message[96]; snprintf(message, sizeof message, "Speech server returned HTTP %d", status);
            char error_body[512];
            int error_len = esp_http_client_read_response(c, error_body, sizeof error_body - 1);
            if (error_len > 0) {
                error_body[error_len] = 0;
                cJSON *error_json = cJSON_Parse(error_body);
                cJSON *detail = cJSON_GetObjectItem(cJSON_GetObjectItem(error_json, "error"), "message");
                if (cJSON_IsString(detail) && !strcmp(detail->valuestring, "No model loaded."))
                    strlcpy(message, "No speech model is loaded on the TTS server", sizeof message);
                cJSON_Delete(error_json);
            }
            if (status == 401 || status == 403) strlcpy(message, "TTS authentication failed; check the API key", sizeof message);
            if (status == 404 || status == 405) strlcpy(message, "TTS endpoint is unavailable; check the server address", sizeof message);
            tts_result(2, message);
            ESP_LOGW(TAG, "tts: HTTP %d", status);
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    free(post);
    if (!pcm || pcm_len < 44) {
        free(pcm);
        if (s_tts_state == 0) tts_result(2, "No WAV audio received from speech server");
        return NULL;
    }

    wav_view_t wav;
    if (s_abort || !wav_parse(pcm, pcm_len, &wav)) {
        tts_result(2, s_abort ? "Speech test cancelled" : "Server returned invalid or unsupported WAV audio");
        heap_caps_free(pcm);
        return NULL;
    }
    *rate = (int)wav.rate;
    *frames = wav.frames;
    if (wav.format == 3) {
        int16_t *out = heap_caps_malloc(wav.frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) { heap_caps_free(pcm); return NULL; }
        for (size_t i = 0; i < wav.frames; i++) {
            float sample = 0.0f;
            for (int ch = 0; ch < wav.channels; ch++) {
                float value;
                memcpy(&value, wav.data + (i * wav.channels + ch) * 4, 4);
                if (!isfinite(value)) value = 0.0f;
                sample += value / wav.channels;
            }
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            out[i] = (int16_t)(sample * 32767.0f);
        }
        heap_caps_free(pcm);
        return out;
    }
    /* Copy with memcpy: RIFF chunks need not be aligned for int16_t access. */
    for (size_t i = 0; i < wav.frames; i++) {
        int sum = 0;
        for (int ch = 0; ch < wav.channels; ch++) {
            int16_t value;
            memcpy(&value, wav.data + (i * wav.channels + ch) * 2, 2);
            sum += value;
        }
        int16_t value = (int16_t)(sum / wav.channels);
        memcpy(pcm + i * 2, &value, 2);
    }
    return (int16_t *)pcm;
}

static bool response_cancelled(void) { return s_abort; }

static bool tts_speak(const char *text)
{
    if (s_abort) return false;
    int rate = 24000;
    size_t frames = 0;
    int16_t *pcm = tts_fetch_pcm(text, &rate, &frames);
    if (!pcm || !frames) {
        heap_caps_free(pcm);
        ESP_LOGW(TAG, "tts: no audio");
        return false;
    }
    ESP_LOGI(TAG, "tts: playing %u frames @ %d Hz", (unsigned)frames, rate);
    bool played = board_audio_play(pcm, frames, rate, response_cancelled);
    heap_caps_free(pcm);
    if (s_abort) { tts_result(2, "Speech test cancelled"); return false; }
    if (!played) { tts_result(2, "Speaker playback failed"); return false; }
    tts_result(1, board_get_volume() ? "Working: valid WAV audio received and played" : "Working: valid WAV received; speaker volume is zero");
    return true;
}

bool odin_tts_test(void)
{
    lvgl_port_lock(0);
    odin_state_t expected = ODIN_STANDBY;
    if (!atomic_compare_exchange_strong(&s_state, &expected, ODIN_RESPONDING)) {
        lvgl_port_unlock(); return false;
    }
    s_abort = false;
    ui_set_state(ODIN_RESPONDING);
    tts_result(0, "Speech test queued");
    s_test_tts = true;
    if (s_monitor) xTaskNotifyGive(s_monitor);
    lvgl_port_unlock();
    return true;
}

/* ---------------------------------------------------------- voice session */
static void set_state(odin_state_t st)
{
    s_state = st;
    ui_set_state(st);
}

/* ------------------------------------------------------------- STT + LLM */

static size_t wav_build_header(uint8_t *h, uint32_t pcm_bytes, int rate)
{
    uint32_t file_size = 36 + pcm_bytes;
    memcpy(h + 0, "RIFF", 4);
    memcpy(h + 4, &file_size, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt_sz = 16; uint16_t af = 1, ch = 1, bps = 16;
    uint32_t byte_rate = (uint32_t)rate * 2; uint16_t align = 2;
    memcpy(h + 16, &fmt_sz, 4); memcpy(h + 20, &af, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4); memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2); memcpy(h + 34, &bps, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &pcm_bytes, 4);
    return 44;
}

static bool stt_transcribe(const int16_t *pcm, size_t frames, int rate, char *out, size_t outsz)
{
    out[0] = 0;
    const char *boundary = "----OdinBoundary7MA4YWxk";
    char pre[320];
    int pre_n = snprintf(pre, sizeof pre,
        "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", boundary, boundary);
    char post[64];
    int post_n = snprintf(post, sizeof post, "\r\n--%s--\r\n", boundary);
    uint32_t pcm_bytes = (uint32_t)(frames * sizeof(int16_t));
    uint32_t wav_total = 44 + pcm_bytes;
    int content_len = pre_n + (int)wav_total + post_n;

    odin_settings_t *cfg = malloc(sizeof *cfg);
    if (!cfg) return false;
    settings_snapshot(cfg);
    char url[256];
    if (!remote_url(url, sizeof url, cfg->stt_url, "/v1/audio/transcriptions")) { free(cfg); return false; }
    char ctype[80]; snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", boundary);
    esp_http_client_config_t hc = {.url = url, .method = HTTP_METHOD_POST, .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) { free(cfg); return false; }
    esp_http_client_set_header(c, "Content-Type", ctype);
    esp_http_client_set_header(c, "Accept", "application/json");
    remote_auth(c, cfg->stt_key); free(cfg);

    bool ok = false;
    if (esp_http_client_open(c, content_len) == ESP_OK) {
        uint8_t hdr[44];
        wav_build_header(hdr, pcm_bytes, rate);
        bool sent_ok = remote_write_all(c, pre, pre_n) && remote_write_all(c, hdr, 44);
        const uint8_t *p = (const uint8_t *)pcm;
        size_t sent = 0, total_b = (size_t)pcm_bytes;
        while (sent_ok && sent < total_b) {
            size_t z = total_b - sent; if (z > 4096) z = 4096;
            if (!remote_write_all(c, p + sent, z)) { sent_ok = false; break; }
            sent += z;
        }
        if (!sent_ok || !remote_write_all(c, post, post_n)) {
            esp_http_client_close(c); esp_http_client_cleanup(c); return false;
        }
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        ESP_LOGI(TAG, "stt: HTTP %d", status);
        if (status == 200) {
            char *buf = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buf) {
                int total = 0, r;
                while (total < 4095 && (r = esp_http_client_read(c, buf + total, 4095 - total)) > 0) total += r;
                buf[total] = 0;
                cJSON *resp = cJSON_Parse(buf);
                if (resp) {
                    cJSON *txt = cJSON_GetObjectItem(resp, "text");
                    if (cJSON_IsString(txt) && txt->valuestring) {
                        char *t = txt->valuestring; while (*t==' '||*t=='\n'||*t=='\r') t++;
                        strlcpy(out, t, outsz);
                        ok = out[0] != 0;
                    }
                    cJSON_Delete(resp);
                }
                heap_caps_free(buf);
            }
        }
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    if (ok) ESP_LOGI(TAG, "stt: \"%s\"", out);
    return ok;
}

bool odin_chat_test(const char *user_text, char *out, size_t outsz)
{
    out[0] = 0;
    odin_settings_t *cfg = malloc(sizeof *cfg);
    if (!cfg) return false;
    settings_snapshot(cfg);
    odin_provider_t *provider = &cfg->providers[cfg->provider];
    char url[256];
    if (!provider->model[0] || !remote_url(url, sizeof url, provider->url,
        provider->protocol ? "/responses" : "/chat/completions")) { free(cfg); return false; }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", provider->model);
    if (provider->protocol) cJSON_AddNumberToObject(root, "max_output_tokens", 1024);
    else if (cfg->provider == 1) cJSON_AddNumberToObject(root, "max_completion_tokens", 1024);
    else cJSON_AddNumberToObject(root, "max_tokens", 512);
    cJSON *messages = cJSON_AddArrayToObject(root, provider->protocol ? "input" : "messages");
    cJSON *system = cJSON_CreateObject(), *user = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", "You are Odin, a concise helpful voice assistant. Reply in plain text, 1-3 short sentences, no markdown.");
    cJSON_AddItemToArray(messages, system);
    cJSON_AddStringToObject(user, "role", "user"); cJSON_AddStringToObject(user, "content", user_text);
    cJSON_AddItemToArray(messages, user);
    char *post = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!post) { free(cfg); return false; }
    int status = 0;
    cJSON *response = remote_json(url, provider->key, post, &status);
    free(post);
    ESP_LOGI(TAG, "llm: HTTP %d (%s)", status, settings_provider_name(cfg->provider));
    if (provider->protocol) {
        cJSON *item;
        cJSON_ArrayForEach(item, cJSON_GetObjectItem(response, "output")) {
            cJSON *part;
            cJSON_ArrayForEach(part, cJSON_GetObjectItem(item, "content")) {
                cJSON *text = cJSON_GetObjectItem(part, "text");
                if (cJSON_IsString(text)) strlcat(out, text->valuestring, outsz);
            }
        }
    } else {
        cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(response, "choices"), 0);
        cJSON *content = cJSON_GetObjectItem(cJSON_GetObjectItem(choice, "message"), "content");
        if (cJSON_IsString(content)) strlcpy(out, content->valuestring, outsz);
    }
    bool ok = status >= 200 && status < 300 && out[0];
    s_llm_online = ok;
    cJSON_Delete(response); free(cfg);
    return ok;
}

static void session_task(void *arg)
{
    ui_show_query("Listening... release TALK to send (30s maximum)");

    const int rate = 16000;
    const int max_sec = 30;
    const int max_samples = rate * max_sec;
    int16_t *pcm = heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int got = 0;
    if (!pcm) {
        ui_show_query("Mic: out of memory");
        set_state(ODIN_STANDBY);
        vTaskDelete(NULL);
        return;
    }

    if (!microphone_acquire(MIC_ASSISTANT)) {
        heap_caps_free(pcm);
        ui_show_query("Microphone unavailable. Hold TALK to retry.");
        set_state(ODIN_STANDBY);
        vTaskDelete(NULL);
        return;
    }
    bool mic_error = false;
    int64_t next_level_update = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)max_sec * 1000000;
    while (got < max_samples && !s_abort && !s_stop_recording && esp_timer_get_time() < deadline) {
        size_t remaining = (size_t)(max_samples - got);
        int n = microphone_read(MIC_ASSISTANT, pcm + got, remaining < 512 ? remaining : 512, 50);
        if (n > 0) {
            got += n;
            /* Capture every sample; calculate and display the level at most
             * ten times per second so recording does less UI work. */
            int64_t now = esp_timer_get_time();
            if (now >= next_level_update) {
                int64_t sum = 0;
                for (int i = 0; i < n; i++) { int64_t v = pcm[got - n + i]; sum += v * v; }
                int rms = (int)sqrtf((float)sum / n);
                char q[72];
                snprintf(q, sizeof q, "Listening... level %d (release to send)", rms);
                ui_show_query(q);
                next_level_update = now + 100000;
            }
        } else if (n < 0) {
            mic_error = true;
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    microphone_release(MIC_ASSISTANT);

    if (s_abort || mic_error || got < rate / 3) {
        heap_caps_free(pcm);
        if (!s_abort) ui_show_query(mic_error ? "Microphone read failed. Hold TALK to retry." :
                                             "Didn't hear anything. Hold TALK and speak.");
        set_state(ODIN_STANDBY);
        vTaskDelete(NULL);
        return;
    }

    set_state(ODIN_PROCESSING);
    ESP_LOGI(TAG, "recorded %d samples (%d ms)", got, got * 1000 / rate);
    ui_show_query("Transcribing...");
    char *text = heap_caps_calloc(1, 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *reply = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text || !reply) {
        free(pcm); free(text); free(reply);
        set_state(ODIN_STANDBY); vTaskDelete(NULL); return;
    }

    if (!stt_transcribe(pcm, (size_t)got, rate, text, 1024) || !text[0]) {
        heap_caps_free(pcm);
        ui_show_query("Transcription failed. Check the speech-to-text server and model.");
        set_state(ODIN_RESPONDING);
        tts_speak("Speech transcription failed. Please check the speech to text service.");
        goto done;
    }
    heap_caps_free(pcm); pcm = NULL;
    ui_show_query(text);
    ESP_LOGI(TAG, "you: %s", text);

    ui_show_response("Thinking...");
    if (!odin_chat_test(text, reply, 2048) || !reply[0]) {
        strlcpy(reply, "Sorry, I couldn't get a response from the language model.", 2048);
    }
    ui_show_response(reply);
    set_state(ODIN_RESPONDING);
    tts_speak(reply);

done:
    free(text); free(reply);
    if (!s_abort) ui_show_query("Hold TALK to speak; release to send");
    set_state(ODIN_STANDBY);
    vTaskDelete(NULL);
}

void odin_talk_pressed(void)
{
    odin_state_t expected = ODIN_STANDBY;
    if (atomic_compare_exchange_strong(&s_state, &expected, ODIN_LISTENING)) {
        /* Reserve the session before creating its task. A quick release must
         * remain set even if the worker has not started yet. */
        s_abort = false;
        s_stop_recording = false;
        set_state(ODIN_LISTENING);
        if (xTaskCreate(session_task, "odin_sess", 12288, NULL, 5, NULL) != pdPASS) {
            set_state(ODIN_STANDBY);
            ui_show_query("Could not start recording. Hold TALK to retry.");
        }
    } else if (expected == ODIN_RESPONDING) {
        s_abort = true;            /* cancel response */
    }
}

void odin_talk_released(void)
{
    if (s_state == ODIN_LISTENING) s_stop_recording = true;
}

void odin_init(void)
{
    s_tts_state = 0;
    s_fetch_models = true;
    settings_lock();
    if (!g_cfg.model[0]) strlcpy(g_cfg.model, "(loading...)", sizeof g_cfg.model);
    settings_unlock();
    xTaskCreate(monitor_task, "odin_mon", 12288, NULL, 3, &s_monitor);
    ESP_LOGI(TAG, "ready, model %s", odin_selected_model());
}

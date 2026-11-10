#include "web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "esp_lvgl_port.h"
#include "settings.h"
#include "camera.h"
#include "microphone.h"
#include "net.h"
#include "odin.h"
#include "ui.h"
#include "cJSON.h"

static httpd_handle_t s_server;
static atomic_bool s_audio_client;
extern const unsigned char web_html_start[] asm("_binary_web_html_start");
extern const unsigned char web_html_end[] asm("_binary_web_html_end");

static esp_err_t json_send(httpd_req_t *req, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json); cJSON_Delete(json);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body); free(body); return err;
}
static esp_err_t message(httpd_req_t *req, const char *status, const char *detail)
{
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject(); cJSON_AddStringToObject(root, "message", detail);
    return json_send(req, root);
}
static bool authorized(httpd_req_t *req)
{
    char header[192], expected[80], decoded[128]; size_t length = 0;
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof header) != ESP_OK || strncmp(header, "Basic ", 6)) goto denied;
    if (mbedtls_base64_decode((unsigned char *)decoded, sizeof decoded - 1, &length,
                            (unsigned char *)header + 6, strlen(header + 6)) != 0) goto denied;
    decoded[length] = 0;
    settings_lock(); snprintf(expected, sizeof expected, "admin:%s", g_cfg.web_password); settings_unlock();
    if (length != strlen(expected)) goto denied;
    unsigned diff = 0;
    for (size_t i = 0; i < length; i++) diff |= (unsigned char)decoded[i] ^ (unsigned char)expected[i];
    if (diff) goto denied;
    if (req->method != HTTP_GET) {
        char origin[192], host[96], wanted[192], content_type[80];
        if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) == ESP_OK) {
            if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK) goto denied;
            snprintf(wanted, sizeof wanted, "http://%s", host);
            if (strcmp(origin, wanted)) goto denied;
        }
        if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof content_type) != ESP_OK ||
            strncmp(content_type, "application/json", 16)) {
            message(req, "415 Unsupported Media Type", "Send application/json"); return false;
        }
    }
    return true;
denied:
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Odin\"");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "Sign in as admin with your portal password. The initial password is shown in Settings on the board.");
    return false;
}
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 8192) {
        message(req, "400 Bad Request", "Expected JSON body, at most 8192 bytes"); return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) { message(req, "500 Internal Server Error", "Out of memory"); return NULL; }
    int read = 0;
    while (read < req->content_len) {
        int n = httpd_req_recv(req, body + read, req->content_len - read);
        if (n <= 0) { free(body); message(req, "400 Bad Request", "Incomplete request body"); return NULL; }
        read += n;
    }
    body[read] = 0; cJSON *json = cJSON_ParseWithLength(body, read); free(body);
    if (!cJSON_IsObject(json)) { cJSON_Delete(json); message(req, "400 Bad Request", "Expected JSON object"); return NULL; }
    return json;
}
static bool query(httpd_req_t *req, const char *name, char *out, size_t size)
{
    char text[768]; out[0] = 0;
    return httpd_req_get_url_query_str(req, text, sizeof text) == ESP_OK &&
           httpd_query_key_value(text, name, out, size) == ESP_OK;
}
static void url_decode(char *s)
{
    char *out = s;
    for (char *p = s; *p; p++) {
        unsigned v;
        if (*p == '%' && p[1] && p[2] && sscanf(p + 1, "%2x", &v) == 1 && v != 0) { *out++ = v; p += 2; }
        else *out++ = *p == '+' ? ' ' : *p;
    }
    *out = 0;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, (const char *)web_html_start, web_html_end - web_html_start);
}

static cJSON *camera_json(const odin_settings_t *cfg)
{
    cJSON *camera = cJSON_CreateObject();
    cJSON_AddBoolToObject(camera, "available", camera_available());
    cJSON_AddNumberToObject(camera, "resolution", cfg->cam_res);
    cJSON_AddBoolToObject(camera, "audio", cfg->cam_audio);
    cJSON_AddNumberToObject(camera, "fps", cfg->stream_fps);
    cJSON_AddNumberToObject(camera, "quality", cfg->stream_quality);
    cJSON_AddNumberToObject(camera, "clients", camera_stream_clients());
    cJSON *resolutions = cJSON_AddArrayToObject(camera, "resolutions");
    for (unsigned i = 0; i < CAMERA_RES_COUNT; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", i); cJSON_AddStringToObject(r, "name", camera_res_name(i));
        cJSON_AddNumberToObject(r, "width", camera_res_width(i)); cJSON_AddNumberToObject(r, "height", camera_res_height(i));
        cJSON_AddItemToArray(resolutions, r);
    }
    return camera;
}
static esp_err_t settings_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    odin_settings_t *cfg = malloc(sizeof *cfg);
    if (!cfg) return message(req, "500 Internal Server Error", "Out of memory");
    settings_snapshot(cfg);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "volume", cfg->volume); cJSON_AddNumberToObject(root, "brightness", cfg->brightness);
    cJSON_AddBoolToObject(root, "power_save", cfg->power_save);
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "setup_ssid", net_setup_ssid());
    cJSON_AddNumberToObject(root, "provider", cfg->provider);
    cJSON *providers = cJSON_AddArrayToObject(root, "providers");
    for (int i = 0; i < ODIN_PROVIDER_COUNT; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "id", i); cJSON_AddStringToObject(p, "name", settings_provider_name(i));
        cJSON_AddStringToObject(p, "url", cfg->providers[i].url); cJSON_AddStringToObject(p, "model", cfg->providers[i].model);
        cJSON_AddNumberToObject(p, "protocol", cfg->providers[i].protocol);
        cJSON_AddBoolToObject(p, "key_set", cfg->providers[i].key[0]); cJSON_AddItemToArray(providers, p);
    }
    cJSON *weather = cJSON_AddObjectToObject(root, "weather");
    cJSON_AddStringToObject(weather, "city", cfg->weather_city); cJSON_AddStringToObject(weather, "country", cfg->weather_country);
    cJSON_AddStringToObject(weather, "country_code", cfg->weather_country_code);
    cJSON_AddNumberToObject(weather, "latitude", cfg->weather_lat); cJSON_AddNumberToObject(weather, "longitude", cfg->weather_lon);
    cJSON *tts = cJSON_AddObjectToObject(root, "tts");
    cJSON_AddStringToObject(tts, "url", cfg->tts_url); cJSON_AddBoolToObject(tts, "key_set", cfg->tts_key[0]);
    cJSON_AddStringToObject(tts, "model", cfg->tts_model); cJSON_AddStringToObject(tts, "voice", cfg->tts_voice);
    cJSON *stt = cJSON_AddObjectToObject(root, "stt");
    cJSON_AddStringToObject(stt, "url", cfg->stt_url); cJSON_AddBoolToObject(stt, "key_set", cfg->stt_key[0]);
    cJSON_AddItemToObject(root, "camera", camera_json(cfg));
    free(cfg); return json_send(req, root);
}
static bool patch_string(cJSON *root, const char *name, char *out, size_t size, bool secret)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!v) return true;
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= size || strpbrk(v->valuestring, "\r\n")) return false;
    if (!secret || v->valuestring[0]) strlcpy(out, v->valuestring, size);
    return true;
}
static bool patch_int(cJSON *root, const char *name, uint8_t *out, int low, int high)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!v) return true;
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble != floor(v->valuedouble) ||
        v->valuedouble < low || v->valuedouble > high) return false;
    *out = v->valueint; return true;
}
static bool patch_bool(cJSON *root, const char *name, bool *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!v) return true;
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v); return true;
}
static bool valid_url(const char *s)
{
    return ((!strncmp(s, "http://", 7) && s[7]) || (!strncmp(s, "https://", 8) && s[8])) &&
           !strpbrk(s, "\r\n \t#");
}
static bool patch_speech(cJSON *object, char *url, size_t url_size, char *key, size_t key_size)
{
    if (!cJSON_IsObject(object) || !patch_string(object, "url", url, url_size, false) ||
        !patch_string(object, "key", key, key_size, true) || !valid_url(url)) return false;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, "clear_key"))) key[0] = 0;
    return true;
}
static esp_err_t settings_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *root = read_json(req); if (!root) return ESP_OK;
    odin_settings_t *cfg = malloc(sizeof *cfg);
    if (!cfg) { cJSON_Delete(root); return message(req, "500 Internal Server Error", "Out of memory"); }
    bool ok = true, weather_changed = false, provider_changed = false;
    bool tts_changed = cJSON_GetObjectItemCaseSensitive(root, "tts") != NULL;
    lvgl_port_lock(0); settings_lock(); settings_snapshot(cfg);
    ok = patch_int(root, "volume", &cfg->volume, 0, 100) && patch_int(root, "brightness", &cfg->brightness, 20, 100) &&
         patch_bool(root, "power_save", &cfg->power_save) && patch_int(root, "provider", &cfg->provider, 0, ODIN_PROVIDER_COUNT - 1);
    provider_changed = cfg->provider != g_cfg.provider;
    cJSON *providers = cJSON_GetObjectItemCaseSensitive(root, "providers");
    if (providers) {
        if (!cJSON_IsArray(providers)) ok = false;
        cJSON *p;
        cJSON_ArrayForEach(p, providers) {
            uint8_t id = 255;
            if (!patch_int(p, "id", &id, 0, ODIN_PROVIDER_COUNT - 1) || id >= ODIN_PROVIDER_COUNT) { ok = false; break; }
            odin_provider_t *profile = &cfg->providers[id];
            if (!patch_string(p, "url", profile->url, sizeof profile->url, false) ||
                !patch_string(p, "model", profile->model, sizeof profile->model, false) ||
                !patch_string(p, "key", profile->key, sizeof profile->key, true) ||
                !patch_int(p, "protocol", &profile->protocol, 0, 1) || !valid_url(profile->url)) ok = false;
            if (cJSON_IsTrue(cJSON_GetObjectItem(p, "clear_key"))) profile->key[0] = 0;
            if (id == cfg->provider) provider_changed = true;
        }
    }
    if (provider_changed) { strlcpy(cfg->model, cfg->providers[cfg->provider].model, sizeof cfg->model); cfg->model_idx = 0; }
    cJSON *weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    if (weather) {
        cJSON *lat = cJSON_GetObjectItemCaseSensitive(weather, "latitude"), *lon = cJSON_GetObjectItemCaseSensitive(weather, "longitude");
        if (!cJSON_IsObject(weather) || !patch_string(weather, "city", cfg->weather_city, sizeof cfg->weather_city, false) ||
            !patch_string(weather, "country", cfg->weather_country, sizeof cfg->weather_country, false) ||
            !patch_string(weather, "country_code", cfg->weather_country_code, sizeof cfg->weather_country_code, false) ||
            !cfg->weather_city[0] || !cfg->weather_country[0] || strlen(cfg->weather_country_code) != 2 ||
            !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) || !isfinite(lat->valuedouble) || !isfinite(lon->valuedouble) ||
            lat->valuedouble < -90 || lat->valuedouble > 90 || lon->valuedouble < -180 || lon->valuedouble > 180) ok = false;
        else { cfg->weather_lat = lat->valuedouble; cfg->weather_lon = lon->valuedouble; weather_changed = true; }
    }
    cJSON *tts = cJSON_GetObjectItemCaseSensitive(root, "tts"), *stt = cJSON_GetObjectItemCaseSensitive(root, "stt");
    if (tts && (!patch_speech(tts, cfg->tts_url, sizeof cfg->tts_url, cfg->tts_key, sizeof cfg->tts_key) ||
        !patch_string(tts, "model", cfg->tts_model, sizeof cfg->tts_model, false) ||
        !patch_string(tts, "voice", cfg->tts_voice, sizeof cfg->tts_voice, false))) ok = false;
    if (stt && !patch_speech(stt, cfg->stt_url, sizeof cfg->stt_url, cfg->stt_key, sizeof cfg->stt_key)) ok = false;
    cJSON *camera = cJSON_GetObjectItemCaseSensitive(root, "camera");
    if (camera && (!cJSON_IsObject(camera) || !patch_int(camera, "resolution", &cfg->cam_res, 0, CAMERA_RES_COUNT - 1) ||
        !patch_int(camera, "fps", &cfg->stream_fps, 1, 15) || !patch_int(camera, "quality", &cfg->stream_quality, 20, 90) ||
        !patch_bool(camera, "audio", &cfg->cam_audio))) ok = false;
    esp_err_t err = ok ? (tts_changed && odin_state() != ODIN_STANDBY ? ESP_ERR_INVALID_STATE : settings_commit(cfg)) : ESP_ERR_INVALID_ARG;
    settings_unlock();
    if (err == ESP_OK) {
        ui_settings_refresh();
        if (weather_changed) ui_set_weather(false, 0, 0, 0, 0, "");
    }
    lvgl_port_unlock();
    free(cfg); cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_STATE) return message(req, "409 Conflict", "Wait for the assistant to finish before changing its speech server");
    if (err != ESP_OK) return message(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error",
                                      err == ESP_ERR_INVALID_ARG ? "Invalid settings; nothing was saved" : "Could not save settings");
    camera_apply();
    if (tts_changed) odin_tts_settings_changed();
    if (weather_changed) net_weather_refresh();
    if (provider_changed) odin_settings_changed();
    return message(req, "200 OK", "Settings saved");
}
static esp_err_t password_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *root = read_json(req); if (!root) return ESP_OK;
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    lvgl_port_lock(0);
    if (cJSON_IsString(password) && cJSON_IsString(confirm) &&
        !strcmp(password->valuestring, confirm->valuestring))
        err = settings_change_web_password(password->valuestring);
    if (err == ESP_OK) ui_settings_refresh();
    lvgl_port_unlock();
    cJSON_Delete(root);
    if (err != ESP_OK) return message(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error",
        err == ESP_ERR_INVALID_ARG ? "Passwords must match and contain 8-32 printable ASCII characters" : "Could not save password");
    return message(req, "200 OK", "Password changed. Sign in as admin with your new password. Setup Wi-Fi uses the new password after a reboot.");
}
static esp_err_t status_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddBoolToObject(r, "connected", net_connected()); cJSON_AddStringToObject(r, "ip", net_ip());
    cJSON_AddNumberToObject(r, "rssi", net_rssi()); cJSON_AddNumberToObject(r, "assistant", odin_state());
    cJSON_AddBoolToObject(r, "llm_online", odin_llm_online());
    cJSON_AddNumberToObject(r, "tts_state", odin_tts_state());
    char detail[96]; odin_tts_detail(detail, sizeof detail); cJSON_AddStringToObject(r, "tts_detail", detail);
    cJSON_AddNumberToObject(r, "microphone_owner", microphone_owner());
    cJSON_AddBoolToObject(r, "camera_available", camera_available());
    settings_lock();
    cJSON_AddNumberToObject(r, "camera_resolution", g_cfg.cam_res);
    settings_unlock();
    cJSON_AddNumberToObject(r, "video_clients", camera_stream_clients());
    cJSON_AddNumberToObject(r, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(r, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return json_send(req, r);
}
static esp_err_t wifi_scan(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (req->method == HTTP_POST) net_scan_start();
    return json_send(req, net_scan_results());
}
static esp_err_t wifi_connect(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *r = read_json(req); if (!r) return ESP_OK;
    cJSON *ssid = cJSON_GetObjectItem(r, "ssid"), *password = cJSON_GetObjectItem(r, "password");
    bool ok = cJSON_IsString(ssid) && cJSON_IsString(password) &&
              net_connect_request(ssid->valuestring, password->valuestring) == ESP_OK;
    cJSON_Delete(r);
    return message(req, ok ? "202 Accepted" : "400 Bad Request", ok ? "Connecting. If disconnected, use the Odin setup network." : "Invalid SSID or password");
}
static esp_err_t locations(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char city[240], country[196];
    if (!query(req, "city", city, sizeof city)) return message(req, "400 Bad Request", "City is required");
    query(req, "country", country, sizeof country); url_decode(city); url_decode(country);
    if (strlen(city) < 2 || strlen(city) > 79 || strlen(country) > 63) return message(req, "400 Bad Request", "Invalid city or country");
    cJSON *r = net_location_search(city, country);
    return r ? json_send(req, r) : message(req, "502 Bad Gateway", "Location search failed; check Wi-Fi");
}
static esp_err_t tts_test(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    bool ok = odin_tts_test();
    return message(req, ok ? "202 Accepted" : "409 Conflict", ok ? "Generating and playing test speech" : "Assistant is busy; try again after the reply");
}
static esp_err_t models_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *r = cJSON_CreateObject(), *models = cJSON_AddArrayToObject(r, "models");
    lvgl_port_lock(0);
    for (int i = 0; i < odin_model_count(); i++) cJSON_AddItemToArray(models, cJSON_CreateString(odin_model_name(i)));
    lvgl_port_unlock();
    odin_fetch_models();
    return json_send(req, r);
}
static atomic_bool s_provider_test_running;
static void provider_test_task(void *arg)
{
    httpd_req_t *req = arg;
    char reply[2048];
    bool ok = odin_chat_test("Reply with: Odin connection works.", reply, sizeof reply);
    cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddStringToObject(r, "message", ok ? reply : "Provider request failed. Check endpoint, API key, model, and API format.");
    httpd_resp_set_status(req, ok ? "200 OK" : "502 Bad Gateway");
    json_send(req, r);
    httpd_req_async_handler_complete(req);
    atomic_store(&s_provider_test_running, false);
    vTaskDelete(NULL);
}
static esp_err_t provider_test(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (odin_state() != ODIN_STANDBY) return message(req, "409 Conflict", "Assistant is busy");
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_provider_test_running, &expected, true))
        return message(req, "409 Conflict", "Provider test already running");
    httpd_req_t *async_req;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        atomic_store(&s_provider_test_running, false);
        return message(req, "500 Internal Server Error", "Could not start provider test");
    }
    if (xTaskCreate(provider_test_task, "provider_test", 12288, async_req, 3, NULL) != pdPASS) {
        message(async_req, "500 Internal Server Error", "Could not start provider test");
        httpd_req_async_handler_complete(async_req);
        atomic_store(&s_provider_test_running, false);
    }
    return ESP_OK;
}

typedef struct { httpd_req_t *req; bool audio; } stream_task_t;
static void wav_header(uint8_t h[44])
{
    uint32_t size = 0x7ffff024, data = 0x7ffff000, fmt = 16, rate = 16000, bytes = 32000;
    uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &size, 4); memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmt, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4); memcpy(h + 28, &bytes, 4); memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
}
static void stream_task(void *arg)
{
    stream_task_t *job = arg;
    httpd_req_t *req = job->req; bool audio = job->audio; free(job);
    esp_err_t err = ESP_OK;
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (audio) {
        httpd_resp_set_type(req, "audio/wav");
        uint8_t header[44]; wav_header(header);
        err = httpd_resp_send_chunk(req, (char *)header, sizeof header);
        int16_t pcm[512];
        while (err == ESP_OK) {
            settings_lock(); bool enabled = g_cfg.cam_audio; settings_unlock();
            if (!enabled || !camera_available()) break;
            int n = 0;
            if (!microphone_yield_camera()) {
                if (microphone_owner() != MIC_CAMERA) microphone_acquire(MIC_CAMERA);
                if (microphone_owner() == MIC_CAMERA) n = microphone_read(MIC_CAMERA, pcm, 512, 50);
            }
            if (microphone_yield_camera() || n < 0) microphone_release(MIC_CAMERA);
            if (n <= 0) { memset(pcm, 0, sizeof pcm); n = 512; vTaskDelay(pdMS_TO_TICKS(32)); }
            err = httpd_resp_send_chunk(req, (char *)pcm, n * sizeof(int16_t));
        }
        microphone_release(MIC_CAMERA); atomic_store(&s_audio_client, false);
    } else {
        httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=odinframe");
        uint8_t *jpeg = heap_caps_malloc(CAMERA_JPEG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint32_t sequence = 0; int64_t last_frame = esp_timer_get_time();
        if (!jpeg) err = ESP_ERR_NO_MEM;
        while (err == ESP_OK && camera_available()) {
            size_t length = 0;
            if (camera_copy_jpeg(jpeg, CAMERA_JPEG_CAPACITY, &length, &sequence)) {
                char head[128]; int n = snprintf(head, sizeof head, "\r\n--odinframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)length);
                err = httpd_resp_send_chunk(req, head, n);
                if (err == ESP_OK) err = httpd_resp_send_chunk(req, (char *)jpeg, length);
                last_frame = esp_timer_get_time();
            } else {
                if (esp_timer_get_time() - last_frame > 10000000) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        free(jpeg); camera_stream_close();
    }
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}
static esp_err_t stream_start(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    bool audio = !strncmp(req->uri, "/audio.wav", 10);
    if (!camera_available()) return message(req, "503 Service Unavailable", "Camera unavailable");
    if (audio) {
        settings_lock(); bool enabled = g_cfg.cam_audio; settings_unlock();
        if (!enabled) return message(req, "403 Forbidden", "Enable camera microphone in Camera settings first");
        bool expected = false;
        if (!atomic_compare_exchange_strong(&s_audio_client, &expected, true)) return message(req, "409 Conflict", "Camera audio already has a listener");
    } else if (!camera_stream_open()) return message(req, "503 Service Unavailable", "At most two video viewers can connect");
    stream_task_t *job = malloc(sizeof *job);
    if (!job) goto fail;
    if (httpd_req_async_handler_begin(req, &job->req) != ESP_OK) { free(job); goto fail; }
    job->audio = audio;
    if (xTaskCreate(stream_task, audio ? "web_audio" : "web_video", 6144, job, 2, NULL) != pdPASS) {
        httpd_resp_send_err(job->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start stream");
        httpd_req_async_handler_complete(job->req); free(job);
        if (audio) atomic_store(&s_audio_client, false); else camera_stream_close();
    }
    return ESP_OK;
fail:
    if (audio) atomic_store(&s_audio_client, false); else camera_stream_close();
    return message(req, "500 Internal Server Error", "Out of memory");
}
void web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 14336; config.max_uri_handlers = 20;
    config.max_open_sockets = 7; config.lru_purge_enable = true;
    config.recv_wait_timeout = 5; config.send_wait_timeout = 3;
    if (httpd_start(&s_server, &config) != ESP_OK) { ESP_LOGE("web", "server start failed"); return; }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post},
        {.uri = "/api/password", .method = HTTP_POST, .handler = password_post},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = wifi_scan},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect},
        {.uri = "/api/weather/search", .method = HTTP_GET, .handler = locations},
        {.uri = "/api/tts/test", .method = HTTP_POST, .handler = tts_test},
        {.uri = "/api/models", .method = HTTP_GET, .handler = models_get},
        {.uri = "/api/provider/test", .method = HTTP_POST, .handler = provider_test},
        {.uri = "/stream", .method = HTTP_GET, .handler = stream_start},
        {.uri = "/audio.wav", .method = HTTP_GET, .handler = stream_start},
    };
    for (unsigned i = 0; i < sizeof routes / sizeof routes[0]; i++) ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    ESP_LOGI("web", "settings server started on port 80; sign in as admin with the password shown on the board");
    /* Also available over the serial console for headless administration. */
    settings_lock();
    if (!settings_web_password_custom()) ESP_LOGI("web_access", "initial admin password: %s", g_cfg.web_password);
    settings_unlock();
}

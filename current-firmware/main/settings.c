#include "settings.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "sdkconfig.h"
#include "camera.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAGIC 0x0D1A0005u /* Previous format: separate web_custom key. */
#define MAGIC_INITIAL_PASSWORD 0x0D1A0006u
#define MAGIC_CUSTOM_PASSWORD 0x0D1A0007u
/* Layout used by the firmware before web configuration was added. */
typedef struct {
    uint8_t volume, brightness;
    bool power_save;
    uint8_t model_idx;
    char model[96], tts_url[96];
    uint8_t cam_res;
    bool cam_audio;
} legacy_settings_t;
typedef struct { uint32_t magic; legacy_settings_t cfg; } legacy_blob_t;
typedef struct { uint32_t magic; odin_settings_t cfg; } blob_t;
static SemaphoreHandle_t s_lock;
odin_settings_t g_cfg;
static bool s_web_password_custom;
bool settings_web_password_custom(void)
{
    settings_lock(); bool custom = s_web_password_custom; settings_unlock(); return custom;
}

void settings_lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void settings_unlock(void) { xSemaphoreGiveRecursive(s_lock); }
void settings_snapshot(odin_settings_t *cfg) { settings_lock(); *cfg = g_cfg; settings_unlock(); }
const char *settings_provider_name(unsigned provider)
{
    static const char *names[] = {"LM Studio / Custom", "OpenAI", "OpenCode"};
    return names[provider < ODIN_PROVIDER_COUNT ? provider : 0];
}
const char *settings_provider(void) { return settings_provider_name(g_cfg.provider); }
const char *settings_model_name(uint8_t idx) { (void)idx; return g_cfg.model[0] ? g_cfg.model : "(loading...)"; }

static void defaults(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.volume = 70; g_cfg.brightness = 100;
    g_cfg.cam_res = CAMERA_RES_DEFAULT;
    g_cfg.cam_audio = false;
    g_cfg.stream_fps = 5; g_cfg.stream_quality = 70;
    strlcpy(g_cfg.wifi_ssid, CONFIG_ODIN_WIFI_SSID, sizeof g_cfg.wifi_ssid);
    strlcpy(g_cfg.wifi_password, CONFIG_ODIN_WIFI_PASSWORD, sizeof g_cfg.wifi_password);
    strlcpy(g_cfg.weather_city, "Laval", sizeof g_cfg.weather_city);
    strlcpy(g_cfg.weather_country, "Canada", sizeof g_cfg.weather_country);
    strlcpy(g_cfg.weather_country_code, "CA", sizeof g_cfg.weather_country_code);
    g_cfg.weather_lat = atof(CONFIG_ODIN_WEATHER_LAT);
    g_cfg.weather_lon = atof(CONFIG_ODIN_WEATHER_LON);
    strlcpy(g_cfg.providers[0].url, CONFIG_ODIN_LLM_URL, sizeof g_cfg.providers[0].url);
    strlcpy(g_cfg.providers[1].url, "https://api.openai.com/v1", sizeof g_cfg.providers[1].url);
    strlcpy(g_cfg.providers[2].url, "https://opencode.ai/inference/openai/v1", sizeof g_cfg.providers[2].url);
    strlcpy(g_cfg.tts_url, CONFIG_ODIN_DEFAULT_TTS_URL, sizeof g_cfg.tts_url);
    strlcpy(g_cfg.tts_key, CONFIG_ODIN_TTS_API_KEY, sizeof g_cfg.tts_key);
    strlcpy(g_cfg.tts_model, CONFIG_ODIN_TTS_MODEL, sizeof g_cfg.tts_model);
    strlcpy(g_cfg.tts_voice, CONFIG_ODIN_TTS_VOICE, sizeof g_cfg.tts_voice);
    strlcpy(g_cfg.stt_url, CONFIG_ODIN_STT_URL, sizeof g_cfg.stt_url);
    strlcpy(g_cfg.stt_key, CONFIG_ODIN_STT_API_KEY, sizeof g_cfg.stt_key);
}

static esp_err_t persist(const odin_settings_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("odin", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    blob_t *b = malloc(sizeof *b);
    if (!b) { nvs_close(h); return ESP_ERR_NO_MEM; }
    /* Encode credential visibility in the same atomic NVS blob as the password. */
    b->magic = s_web_password_custom ? MAGIC_CUSTOM_PASSWORD : MAGIC_INITIAL_PASSWORD;
    b->cfg = *cfg;
    err = nvs_set_blob(h, "config_v2", b, sizeof *b);
    if (err == ESP_OK) err = nvs_commit(h);
    free(b); nvs_close(h);
    return err;
}

void settings_load(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_lock);
    defaults();
    nvs_handle_t h;
    if (nvs_open("odin", NVS_READONLY, &h) == ESP_OK) {
        uint8_t custom = 0;
        nvs_get_u8(h, "web_custom", &custom);
        s_web_password_custom = custom == 1;
        blob_t *b = malloc(sizeof *b);
        size_t n = sizeof *b;
        bool loaded = b && nvs_get_blob(h, "config_v2", b, &n) == ESP_OK && n == sizeof *b && (b->magic == MAGIC || b->magic == MAGIC_INITIAL_PASSWORD || b->magic == MAGIC_CUSTOM_PASSWORD);
        if (loaded) {
            g_cfg = b->cfg;
            /* Only the previous format uses the separate migration key. */
            if (b->magic != MAGIC) s_web_password_custom = b->magic == MAGIC_CUSTOM_PASSWORD;
        }
        else {
            s_web_password_custom = false;
            legacy_blob_t old;
            n = sizeof old;
            if (nvs_get_blob(h, "cfg", &old, &n) == ESP_OK && n == sizeof old &&
                (old.magic == 0x0D1A0003u || old.magic == 0x0D1A0004u)) {
                g_cfg.volume = old.cfg.volume; g_cfg.brightness = old.cfg.brightness;
                g_cfg.power_save = old.cfg.power_save; g_cfg.model_idx = old.cfg.model_idx;
                memcpy(g_cfg.model, old.cfg.model, sizeof old.cfg.model);
                g_cfg.model[sizeof g_cfg.model - 1] = 0;
                if (old.magic == 0x0D1A0004u) {
                    memcpy(g_cfg.tts_url, old.cfg.tts_url, sizeof old.cfg.tts_url);
                    g_cfg.tts_url[sizeof old.cfg.tts_url - 1] = 0;
                }
                g_cfg.cam_res = old.cfg.cam_res;
                /* Camera audio requires explicit opt-in in the new web interface. */
                strlcpy(g_cfg.providers[0].model, g_cfg.model, sizeof g_cfg.providers[0].model);
            }
        }
        free(b); nvs_close(h);
    }
    if (g_cfg.volume > 100) g_cfg.volume = 100;
    if (g_cfg.brightness < 20) g_cfg.brightness = 20;
    if (g_cfg.brightness > 100) g_cfg.brightness = 100;
    if (g_cfg.cam_res >= CAMERA_RES_COUNT) g_cfg.cam_res = CAMERA_RES_DEFAULT;
    if (g_cfg.provider >= ODIN_PROVIDER_COUNT) g_cfg.provider = 0;
    if (g_cfg.stream_fps < 1 || g_cfg.stream_fps > 15) g_cfg.stream_fps = 5;
    if (g_cfg.stream_quality < 20 || g_cfg.stream_quality > 90) g_cfg.stream_quality = 70;
    if (!g_cfg.web_password[0]) {
        uint32_t random[2]; esp_fill_random(random, sizeof random);
        snprintf(g_cfg.web_password, sizeof g_cfg.web_password, "%08lx%08lx", (unsigned long)random[0], (unsigned long)random[1]);
    }
    settings_save();
}

void settings_save(void)
{
    settings_lock();
    persist(&g_cfg);
    settings_unlock();
}

esp_err_t settings_commit(const odin_settings_t *cfg)
{
    settings_lock();
    esp_err_t err = persist(cfg);
    if (err == ESP_OK) g_cfg = *cfg;
    settings_unlock();
    return err;
}

esp_err_t settings_change_web_password(const char *password)
{
    size_t n = password ? strlen(password) : 0;
    if (n < 8 || n > 32) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)password[i] < 32 || (unsigned char)password[i] > 126)
            return ESP_ERR_INVALID_ARG;
    odin_settings_t *cfg = malloc(sizeof *cfg);
    if (!cfg) return ESP_ERR_NO_MEM;
    settings_lock();
    *cfg = g_cfg;
    strlcpy(cfg->web_password, password, sizeof cfg->web_password);
    bool previous = s_web_password_custom;
    s_web_password_custom = true;
    esp_err_t err = persist(cfg);
    if (err == ESP_OK) g_cfg = *cfg;
    else s_web_password_custom = previous;
    settings_unlock();
    memset(cfg, 0, sizeof *cfg); free(cfg);
    return err;
}

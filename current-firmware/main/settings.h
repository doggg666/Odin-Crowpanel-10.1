#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define ODIN_PROVIDER_COUNT 3
typedef struct {
    char url[192];
    char key[256];
    char model[96];
    uint8_t protocol; /* 0 = Chat Completions, 1 = Responses */
} odin_provider_t;

typedef struct {
    uint8_t volume, brightness;
    bool power_save;
    uint8_t model_idx;
    char model[96];
    char tts_url[192];
    uint8_t cam_res;
    bool cam_audio;
    char wifi_ssid[33], wifi_password[65];
    char weather_city[80], weather_country[64], weather_country_code[3];
    double weather_lat, weather_lon;
    uint8_t provider;
    odin_provider_t providers[ODIN_PROVIDER_COUNT];
    char tts_key[256], tts_model[96], tts_voice[64];
    char stt_url[192], stt_key[256];
    char web_password[33];
    uint8_t stream_fps, stream_quality;
} odin_settings_t;

extern odin_settings_t g_cfg;
void settings_load(void);
void settings_save(void);
esp_err_t settings_commit(const odin_settings_t *cfg);
void settings_snapshot(odin_settings_t *cfg);
void settings_lock(void);
void settings_unlock(void);
const char *settings_provider(void);
const char *settings_provider_name(unsigned provider);
const char *settings_model_name(uint8_t idx);

bool settings_web_password_custom(void);
esp_err_t settings_change_web_password(const char *password);

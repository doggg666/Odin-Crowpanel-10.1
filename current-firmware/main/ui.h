#pragma once
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include "odin.h"

void ui_init(void);
void ui_apply_display(void);        /* brightness + power-saving -> backlight */

void ui_set_clock(const struct tm *t);                 /* NULL = not synced yet */
void ui_set_wifi(bool connected, int rssi);
void ui_set_ip(const char *ip);
void ui_set_weather(bool valid, float temp, float feels, int humidity,
                    float wind_kmh, const char *desc);
void ui_set_llm(bool online);
void ui_set_battery(int percent, int millivolts);                      /* -1 = unknown, -2 = USB/external (>4.3V) */
void ui_set_tts(int state);                            /* 0 checking, 1 ok, 2 fail */
void ui_set_state(odin_state_t st);
void ui_show_query(const char *text);
void ui_show_response(const char *text);               /* typed out on screen */
void ui_refresh_model_list(void);                      /* rebuild model buttons from odin */
void ui_camera_status(const char *message);
void ui_camera_availability(bool available);
void ui_camera_frame(const uint16_t *pixels, int width, int height);

void ui_settings_refresh(void);
void ui_set_weather_location(const char *city, const char *country);

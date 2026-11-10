#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* CrowPanel Advanced 10.1" ESP32-P4 (1024x600 MIPI-DSI, GT911 touch) */
#define BOARD_H_RES        1024
#define BOARD_V_RES        600

#define BOARD_I2C_PORT     0
#define BOARD_I2C_SDA      45
#define BOARD_I2C_SCL      46
#define BOARD_TOUCH_RST    40
#define BOARD_TOUCH_INT    42
#define BOARD_BLIGHT_GPIO  31
#define BOARD_AMP_GPIO     30   /* amplifier shutdown: 0 = play, 1 = mute */

/* LDOs, I2C, touch, MIPI-DSI panel, LVGL port. Backlight is left off. */
esp_err_t board_init(void);

/* 0..100 */
void board_set_brightness(uint8_t pct);

/* System volume 0..100. Applies PCM gain and mutes the amplifier at zero. */
void board_set_volume(uint8_t pct);
uint8_t board_get_volume(void);

/* I2S speaker output on GPIO23=DOUT, 22=BCLK, 21=LRCLK. */
esp_err_t board_audio_init(void);
/* Play mono PCM; true only when all frames were written without cancellation. */
bool board_audio_play(const int16_t *pcm, size_t frames, int rate, bool (*cancelled)(void));

/* PDM microphone input on GPIO24=CLK, GPIO26=DATA (16 kHz, 16-bit mono PCM). */
esp_err_t board_mic_init(void);
esp_err_t board_mic_start(void); /* enable capture */
void      board_mic_stop(void);  /* disable capture (stops DMA) */
/* Read PCM samples; returns samples read, or -1 on error. */
int board_mic_read(int16_t *dst, size_t samples, uint32_t timeout_ms);

/* Battery/power via on-board STC8H1Kxx MCU (I2C 0x2F).
 * Returns 0..100, -1 unknown, -2 USB/external power (bat_voltage > 4300 mV). */
int board_battery_percent(int *millivolts);

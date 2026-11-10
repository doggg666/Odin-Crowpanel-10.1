#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t frames;
    uint32_t rate;
    uint16_t channels;
    uint16_t format;
} wav_view_t;

/* Accept only complete PCM16 or IEEE float32 mono/stereo RIFF/WAVE files. */
bool wav_parse(const uint8_t *buf, size_t len, wav_view_t *view);

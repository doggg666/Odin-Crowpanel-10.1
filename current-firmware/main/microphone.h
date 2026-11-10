#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef enum { MIC_FREE = 0, MIC_ASSISTANT = 1, MIC_CAMERA = 2 } mic_owner_t;
void microphone_init(void);
bool microphone_acquire(mic_owner_t owner);
void microphone_release(mic_owner_t owner);
int microphone_read(mic_owner_t owner, int16_t *pcm, size_t samples, unsigned timeout_ms);
bool microphone_yield_camera(void);
mic_owner_t microphone_owner(void);

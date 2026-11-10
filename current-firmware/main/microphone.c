#include "microphone.h"
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "board.h"
static SemaphoreHandle_t s_mutex;
static _Atomic mic_owner_t s_owner;
static atomic_bool s_assistant_waiting;
void microphone_init(void) { s_mutex = xSemaphoreCreateMutex(); configASSERT(s_mutex); }
mic_owner_t microphone_owner(void) { return atomic_load(&s_owner); }
bool microphone_yield_camera(void) { return atomic_load(&s_assistant_waiting) || microphone_owner() == MIC_ASSISTANT; }
bool microphone_acquire(mic_owner_t owner)
{
    if (!s_mutex || owner == MIC_FREE) return false;
    if (owner == MIC_ASSISTANT) atomic_store(&s_assistant_waiting, true);
    for (int tries = 0; tries < (owner == MIC_ASSISTANT ? 50 : 1); tries++) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (owner == MIC_ASSISTANT && atomic_load(&s_owner) == MIC_CAMERA) {
            /* The mutex guarantees the camera's DMA read has finished. A slow
             * network client must never keep the assistant waiting for the mic. */
            board_mic_stop(); atomic_store(&s_owner, MIC_FREE);
        }
        bool allowed = atomic_load(&s_owner) == MIC_FREE &&
                       (owner == MIC_ASSISTANT || !atomic_load(&s_assistant_waiting));
        bool ok = allowed && board_mic_start() == ESP_OK;
        if (ok) atomic_store(&s_owner, owner);
        xSemaphoreGive(s_mutex);
        if (ok) {
            if (owner == MIC_ASSISTANT) atomic_store(&s_assistant_waiting, false);
            return true;
        }
        if (owner == MIC_ASSISTANT) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (owner == MIC_ASSISTANT) atomic_store(&s_assistant_waiting, false);
    return false;
}
int microphone_read(mic_owner_t owner, int16_t *pcm, size_t samples, unsigned timeout_ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = -1;
    if (atomic_load(&s_owner) == owner && !(owner == MIC_CAMERA && atomic_load(&s_assistant_waiting)))
        n = board_mic_read(pcm, samples, timeout_ms > 50 ? 50 : timeout_ms);
    xSemaphoreGive(s_mutex);
    return n;
}
void microphone_release(mic_owner_t owner)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (atomic_load(&s_owner) == owner) { board_mic_stop(); atomic_store(&s_owner, MIC_FREE); }
    xSemaphoreGive(s_mutex);
}

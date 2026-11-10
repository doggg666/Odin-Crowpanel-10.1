#define _POSIX_C_SOURCE 200809L
#include "microphone.h"
#include "board.h"
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
static atomic_bool reading, started, fail_start;
static atomic_int stop_count;
esp_err_t board_mic_start(void) { if (fail_start) return ESP_FAIL; assert(!started); started = true; return ESP_OK; }
void board_mic_stop(void) { assert(!reading); assert(started); started = false; stop_count++; }
int board_mic_read(int16_t *dst, size_t count, uint32_t timeout)
{
    (void)timeout; assert(started); assert(!atomic_exchange(&reading, true));
    struct timespec delay = {.tv_nsec = 20000000}; nanosleep(&delay, NULL);
    for (size_t i = 0; i < count; i++) dst[i] = 1234;
    reading = false; return count;
}
static void *camera_read(void *unused) { (void)unused; int16_t pcm[64]; assert(microphone_read(MIC_CAMERA, pcm, 64, 50) == 64); return NULL; }
int main(void)
{
    microphone_init();
    assert(microphone_acquire(MIC_CAMERA));
    assert(!microphone_acquire(MIC_CAMERA)); /* A second camera reader is refused. */
    pthread_t thread; pthread_create(&thread, NULL, camera_read, NULL);
    while (!reading) {}
    /* Assistant preemption must wait for an in-flight physical read to finish. */
    assert(microphone_acquire(MIC_ASSISTANT));
    pthread_join(thread, NULL);
    assert(microphone_owner() == MIC_ASSISTANT);
    int stops = stop_count;
    microphone_release(MIC_CAMERA); assert(stop_count == stops);
    assert(!microphone_acquire(MIC_CAMERA));
    int16_t pcm[32]; assert(microphone_read(MIC_CAMERA, pcm, 32, 50) == -1);
    assert(microphone_read(MIC_ASSISTANT, pcm, 32, 50) == 32);
    microphone_release(MIC_ASSISTANT);
    assert(microphone_acquire(MIC_CAMERA)); /* Audio resumes after the assistant. */
    microphone_release(MIC_CAMERA);
    fail_start = true;
    assert(!microphone_acquire(MIC_ASSISTANT));
    assert(microphone_owner() == MIC_FREE && !microphone_yield_camera());
    fail_start = false;
    assert(microphone_acquire(MIC_CAMERA)); microphone_release(MIC_CAMERA);
    puts("Microphone ownership and preemption tests passed");
}

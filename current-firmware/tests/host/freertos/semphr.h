#pragma once
#include <pthread.h>
#include <stdlib.h>
#include "FreeRTOS.h"
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { pthread_mutex_t *m = malloc(sizeof *m); assert(m); pthread_mutex_init(m, NULL); return m; }
static inline int xSemaphoreTake(SemaphoreHandle_t m, TickType_t timeout) { (void)timeout; return pthread_mutex_lock(m) == 0; }
static inline int xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0; }

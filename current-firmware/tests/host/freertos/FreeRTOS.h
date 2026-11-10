#pragma once
#include <assert.h>
#include <stdint.h>
typedef unsigned TickType_t;
#define portMAX_DELAY ((TickType_t)-1)
#define pdMS_TO_TICKS(ms) (ms)
#define configASSERT(x) assert(x)

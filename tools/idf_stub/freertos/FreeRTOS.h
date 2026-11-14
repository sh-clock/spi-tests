#pragma once
#include "../esp_err.h"
typedef uint32_t TickType_t;
#define portMAX_DELAY ((TickType_t)0xffffffffU)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
static inline void vTaskDelay(TickType_t t) { (void)t; }

#pragma once
#include "../esp_err.h"
#include <stdint.h>
typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;
#define GPIO_MODE_OUTPUT 2
static inline esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
static inline esp_err_t gpio_set_level(int pin, int level) { (void)pin; (void)level; return ESP_OK; }

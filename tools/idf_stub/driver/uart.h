#pragma once
#include "../esp_err.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"
typedef int uart_port_t;
#define UART_NUM_0 0
static inline esp_err_t uart_driver_install(uart_port_t p, int rx, int tx, int q, void *qs, int f) { (void)p;(void)rx;(void)tx;(void)q;(void)qs;(void)f; return ESP_OK; }
static inline int uart_read_bytes(uart_port_t p, void *buf, uint32_t len, TickType_t t) { (void)p;(void)buf;(void)len;(void)t; return 0; }

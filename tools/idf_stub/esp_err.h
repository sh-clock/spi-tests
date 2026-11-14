/* Minimal ESP-IDF API stubs — only for host-side syntax checking of the
 * firmware sources (gcc -fsyntax-only). NOT part of the firmware. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) { } } while (0)
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "ERR"; }
static inline void *esp_err_to_name_r(int e, char *b, size_t n) { (void)e;(void)b;(void)n; return 0; }

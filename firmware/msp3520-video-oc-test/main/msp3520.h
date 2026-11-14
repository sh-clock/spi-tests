/*
 * msp3520.h — ESP-IDF driver for the MSP3520 3.5" SPI display (480x320,
 * ILI9488) tuned for streaming an interlaced video.
 *
 * Same wiring as the msp3520-oc-test project (ESP32-S3-DevKitC-1):
 *   MSP3520 pin          ESP32-S3 GPIO
 *   1  VCC     ->  5V    (module accepts 3.3-5V; logic is 3.3 V TTL)
 *   2  GND     ->  GND
 *   3  CS      ->  10
 *   4  RESET   ->  14
 *   5  DC/RS   ->  9
 *   6  SDI     ->  11    (MOSI, native FSPI IOMUX pin)
 *   7  SCK     ->  12    (native FSPI IOMUX pin)
 *   8  LED     ->  47    (backlight, HIGH = on)
 *   9  SDO     ->  13    (MISO, only used for the ID read)
 *
 * Differences from the msp3520-oc-test driver:
 *  - the D/C line is driven from the SPI driver's pre-transaction callback,
 *    which is how the official esp_lcd component does it. This makes queued
 *    (interrupt-driven) transactions possible: the callback runs right
 *    before each transaction hits the wire, so D/C is always correct even
 *    with several transactions in flight.
 *  - new streaming API for interlaced fields:
 *      msp3520_prepare_stream()   once per panel init (sets column window)
 *      msp3520_field_begin()      CS low
 *      msp3520_queue_line(y, px)  queue PASET+RAMWR+1440 B for one line
 *      msp3520_retire_line()      wait until the oldest queued line is done
 *      msp3520_field_end()        CS high
 *    Keeping two lines "in flight" lets the CPU decode the next line while
 *    DMA is still pushing the previous one.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* --- Pin map (change here, then rebuild) --------------------------------- */
#define LCD_PIN_SCLK   12   /* MSP3520 SCK  (pin 7)  */
#define LCD_PIN_MOSI   11   /* MSP3520 SDI  (pin 6)  */
#define LCD_PIN_MISO   13   /* MSP3520 SDO  (pin 9)  */
#define LCD_PIN_CS     10   /* MSP3520 CS   (pin 3)  */
#define LCD_PIN_DC      9   /* MSP3520 DC/RS(pin 5)  */
#define LCD_PIN_RST    14   /* MSP3520 RESET(pin 4)  */
#define LCD_PIN_BL     47   /* MSP3520 LED  (pin 8)  */

/* --- Panel geometry ------------------------------------------------------ */
#define LCD_H_RES     480
#define LCD_V_RES     320
#define LCD_BPP         3                          /* RGB666: 3 bytes/px  */
#define LCD_LINE_BYTES  (LCD_H_RES * LCD_BPP)      /* 1440                */
#define LCD_MAX_TRANSFER  (LCD_LINE_BYTES + 64)

/* Set to 1 if your panel shows inverted-looking colors (see README). */
#define LCD_INVERT_COLORS 0

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t msp3520_init(void);

/* Re-configure the SPI clock (device is removed and re-added on the bus). */
esp_err_t msp3520_set_clock(uint32_t hz);
uint32_t  msp3520_get_clock(void);

/* Hardware-reset the panel and re-run the init sequence (at a safe 1 MHz).
 * Call before every test frequency so each one starts from a clean panel,
 * then msp3520_set_clock() and msp3520_prepare_stream() again. */
esp_err_t msp3520_reinit_panel(void);

bool msp3520_read_id(uint8_t raw_d3[6], uint8_t raw_04[6]);

void msp3520_backlight(bool on);

/* Fill the whole screen with one color (boot / blank screen). */
esp_err_t msp3520_fill_screen(uint16_t color565);

/* --- interlaced field streaming ----------------------------------------- */

/* Set the column window once (CASET 0..479). After a panel reset this must
 * be called once before the first field; the controller keeps the window
 * for every subsequent line write. */
esp_err_t msp3520_prepare_stream(void);

void msp3520_field_begin(void);

/* Queue one display line (1440 bytes, RGB666) at row `y`. Up to
 * TX_LINES_IN_FLIGHT lines may be queued before a retire is needed. */
esp_err_t msp3520_queue_line(uint16_t y, const uint8_t *line_bytes);

/* Wait until the oldest queued line has fully left the wire, so its buffer
 * can be reused. (Queue and retire strictly in FIFO order.) */
esp_err_t msp3520_retire_line(void);

void msp3520_field_end(void);

/* How many lines the player may queue before retiring (double buffering). */
#define TX_LINES_IN_FLIGHT 2

/* Convert one RGB565 color to the 3-byte RGB666 triplet used on the wire. */
static inline void lcd_rgb565_to_666(uint16_t c565, uint8_t out[3])
{
    out[0] = (c565 >> 8) & 0xF8;   /* red   */
    out[1] = (c565 >> 3) & 0xFC;   /* green */
    out[2] = (c565 << 3) & 0xF8;   /* blue  */
}

#ifdef __cplusplus
}
#endif

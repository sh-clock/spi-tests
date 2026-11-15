/*
 * msp3520.h — ESP-IDF driver for the MSP3520 3.5" SPI display (480x320,
 * ILI9488) tuned for streaming progressive video.
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
 *  - new streaming API for full-frame progressive video:
 *      msp3520_prepare_stream()    once per panel init (CASET+PASET window)
 *      msp3520_field_begin()       CS low + RAMWR (write starts at row 0)
 *      msp3520_queue_block(px)     queue 16 lines of pixel data (one DMA tx)
 *      msp3520_retire_block()      wait until the oldest queued block is done
 *      msp3520_field_end()         CS high
 *    v5 column-interlace adds:
 *      msp3520_prepare_half_stream(h)  set a 240-px half window (h=0 left)
 *      msp3520_queue_half_block(px)    queue 16 half-window lines (one DMA tx)
 *    Keeping two blocks "in flight" lets the CPU decode the next block
 *    while DMA is still pushing the previous one.
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
#define LCD_BPP         3                          /* default wire fmt (666) */
#define LCD_LINE_BYTES  (LCD_H_RES * LCD_BPP)      /* 1440 (RGB666 line)     */
#define LCD_LINE_BYTES_MAX (LCD_H_RES * 3)         /* 1440 (RGB666 line)     */
#define LCD_BLOCK_LINES 16                         /* lines per streamed block */
#define LCD_BLOCK_BYTES (LCD_LINE_BYTES_MAX * LCD_BLOCK_LINES)  /* 23040 */
#define LCD_MAX_TRANSFER  (LCD_BLOCK_BYTES + 64)

/* Set to 1 if your panel shows inverted-looking colors (see README). */
#define LCD_INVERT_COLORS 0

/* EXPERIMENT for 80 MHz stability on long jumper wires: SCK/MOSI GPIO drive
 * capability. -1 = ESP-IDF default (20 mA class); 0..3 = weakest..strongest
 * (gpio_drive_cap_t). Weaker edges (0/1) reduce ringing — but the ILI9488 is
 * 4x out of spec at 80 MHz (datasheet min serial write cycle 50 ns = 20 MHz
 * max), so this is a last-resort experiment, not a fix. */
#define LCD_EDGE_DRIVE_CAP (-1)

/* MADCTL (memory access control). 0x28 = landscape 480x320, origin top-left
 * (MV row/column exchange + BGR). If the picture comes out upside-down on
 * your mounting, use 0xE8 (adds MY|MX = 180-degree rotation). */
#define LCD_MADCTL 0x28

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t msp3520_init(void);

/* Re-configure the SPI clock (device is removed and re-added on the bus). */
esp_err_t msp3520_set_clock(uint32_t hz);
uint32_t  msp3520_get_clock(void);

/* Select the wire pixel format: 2 = RGB565 (COLMOD 0x55) or 3 = RGB666
 * (COLMOD 0x66). IMPORTANT: the ILI9488 supports 18-bit (bpp 3) ONLY on
 * its native 4-wire SPI interface — 16-bit RGB565 is for the parallel
 * interface (verified: TFT_eSPI, LovyanGFX and Arduino_GFX all force
 * 18-bit for ILI9488 SPI; LCDWiki's own MSP3520 demo uses 0x66; setting
 * 0x55 on native SPI yields a blank screen). Block buffers passed to
 * msp3520_queue_block() must hold LCD_BLOCK_BYTES bytes. */
esp_err_t msp3520_set_wire_bpp(int bpp);
int       msp3520_get_wire_bpp(void);

/* Hardware-reset the panel and re-run the init sequence (at a safe 1 MHz).
 * Call before every test frequency so each one starts from a clean panel,
 * then msp3520_set_clock() and msp3520_prepare_stream() again. */
esp_err_t msp3520_reinit_panel(void);

bool msp3520_read_id(uint8_t raw_d3[6], uint8_t raw_04[6]);

void msp3520_backlight(bool on);

/* Live experiment: set SCK/MOSI GPIO drive capability (0=weakest..3=strongest
 * edges, gpio_drive_cap_t). Player hotkey 'd' cycles it during playback —
 * weaker edges may reduce ringing at 65-80 MHz. No effect below 40 MHz. */
void msp3520_set_edge_drive(int cap);

/* Fill the whole screen with one color (boot / blank screen). */
esp_err_t msp3520_fill_screen(uint16_t color565);

/* --- full-frame block streaming ----------------------------------------- */

/* Set the address window to the whole display (CASET 0..479 + PASET 0..319).
 * After a panel reset this must be called once; afterwards every frame is
 * streamed as pure pixel data — the controller wraps rows inside the window,
 * so no per-line PASET/RAMWR addressing is needed on the wire. */
esp_err_t msp3520_prepare_stream(void);

/* --- v5: column-interlaced half-frame streaming --------------------------- */

/* Set the address window to ONE 240-px half of the display (half=0: x 0..239,
 * half=1: x 240..479), full height. With MADCTL MV=1 CASET spans the 480-px
 * display axis, so a "half" is a vertical column band. A half-frame then
 * streams exactly like a full frame: msp3520_field_begin() (RAMWR resets the
 * write pointer to the half-window origin, datasheet §5.2.24) + 20
 * msp3520_queue_half_block() calls + msp3520_field_end(). */
esp_err_t msp3520_prepare_half_stream(int half);

/* Queue one block of LCD_BLOCK_LINES consecutive half-window lines
 * (LCD_BLOCK_LINES * LCD_H_RES/2 * wire_bpp bytes) as one DMA transaction.
 * Same in-flight/retire rules as msp3520_queue_block(). */
esp_err_t msp3520_queue_half_block(const uint8_t *block_bytes);

/* Geometry of one half-frame (for player-side buffers). */
#define LCD_HALF_LINE_BYTES  ((LCD_H_RES / 2) * 3)                  /* 720  */
#define LCD_HALF_BLOCK_BYTES (LCD_HALF_LINE_BYTES * LCD_BLOCK_LINES) /* 11520 */

/* CS low + RAMWR: opens a frame write; the address pointer resets to the
 * window start (row 0) and keeps advancing while CS stays low. */
esp_err_t msp3520_field_begin(void);

/* Queue one block of LCD_BLOCK_LINES consecutive display lines
 * (LCD_BLOCK_LINES * 480 * wire_bpp bytes) as a single DMA transaction.
 * Up to TX_BLOCKS_IN_FLIGHT blocks may be queued before a retire is
 * needed; the caller's buffer must stay untouched until the matching
 * retire. */
esp_err_t msp3520_queue_block(const uint8_t *block_bytes);

/* Wait until the oldest queued block has fully left the wire, so its
 * buffer can be reused. (Queue and retire strictly in FIFO order.) */
esp_err_t msp3520_retire_block(void);

void msp3520_field_end(void);

/* How many blocks the player may queue before retiring (double buffering). */
#define TX_BLOCKS_IN_FLIGHT 2

/* Convert one RGB565 color to the 2-byte big-endian wire format
 * (hi = R4..R0 G5..G3, lo = G2..G0 B4..B0 — ILI9488 16bpp, MSB first). */
static inline void lcd_rgb565_to_be16(uint16_t c565, uint8_t out[2])
{
    out[0] = (uint8_t)(c565 >> 8);
    out[1] = (uint8_t)(c565 & 0xFF);
}

/* Convert one RGB565 color to the 3-byte RGB666 wire format (the only
 * format the ILI9488 accepts over its native 4-wire SPI interface). */
static inline void lcd_rgb565_to_666(uint16_t c565, uint8_t out[3])
{
    out[0] = (c565 >> 8) & 0xF8;   /* red   */
    out[1] = (c565 >> 3) & 0xFC;   /* green */
    out[2] = (c565 << 3) & 0xF8;   /* blue  */
}

#ifdef __cplusplus
}
#endif

/*
 * main.c — msp3520-video-lab: a technique testbed for the MSP3520 display.
 *
 * A new test video (INTRO.mkv, DVD rip, 720x480 anamorphic 29.97 fps) was
 * cut, cropped (desqueeze + letterbox + 540-px center crop to 3:2) and
 * encoded by tools/encode_lab.py as 8-bit indexed frames with a global
 * 256-colour RGB666 palette and 2x2 Bayer dithering (6.29 MB of assets,
 * all embedded — the flash layout is the same single-factory-app scheme
 * as the previous projects).
 *
 * This firmware plays that ONE cut with every technique from the request
 * (interlacing/scanlines/columns/quincunx/deltas/doubling/indexed/pre-
 * interlaced/dual-core), selected from the esp-idf monitor. A technique
 * loops until you press any key, then the menu returns. The 40 MHz SPI
 * clock is the established safe maximum (the S3 GPSPI has no other step
 * between 40 and 80 MHz — see WORKLOG 18); 'c' switches between the real
 * lattice clocks anyway.
 *
 * The panel is brought up lazily on the first selection, so the menu is
 * reachable even with no display wired (and in QEMU, which stalls on the
 * first SPI transaction — WORKLOG 18).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "assets.h"
#include "players.h"
#include "msp3520.h"

#define CONSOLE_UART UART_NUM_0

/* ================= console helpers (v7 style) ========================== */

static void console_init(void)
{
    uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0);
}

static void read_line(const char *prompt, char *buf, size_t buflen)
{
    uint8_t c;
    size_t i = 0;

    printf("%s", prompt);
    for (;;) {
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            break;
        }
        if ((c == 8 || c == 127) && i > 0) {
            i--;
            printf("\b \b");
            continue;
        }
        if (i < buflen - 1 && isprint((int)c)) {
            buf[i++] = (char)c;
            printf("%c", c);
        }
    }
    buf[i] = '\0';
    printf("\n");
}

/* ================= the menu ============================================ */

typedef struct {
    const char *sel;      /* what the user types                          */
    const char *name;     /* menu line                                    */
    void (*fn)(void);
} entry_t;

static const entry_t s_entries[] = {
    { "1",   "pure 4:1 quad interlace       80 rows/field   115.2 KB", play_quad4 },
    { "2",   "static scanlines + 2:1 (CRT)  80 rows/field   115.2 KB", play_scan2 },
    { "3",   "static scanlines + 4:1 (fast) 40 rows/field    57.6 KB", play_scan4 },
    { "4",   "pure 8:1 octuple interlace    40 rows/field    57.6 KB", play_oct8 },
    { "5",   "3:1 triple interlace         ~107 rows/field  153.6 KB", play_tri3 },
    { "6",   "column 4:1 interlace         120 cols/field   115.2 KB", play_col4 },
    { "7a",  "quincunx (half-width lines)  320 half-rows    230.4 KB", play_quincunx_full },
    { "7b",  "quincunx + 2:1 rows          160 half-rows    115.2 KB", play_quincunx_half },
    { "8",   "macroblock 16x16 delta grid  adaptive         variable", play_macro },
    { "9",   "span delta (delta.spans)     adaptive         variable", play_spans },
    { "10",  "LFSR stochastic 4:1          80 random rows   115.2 KB", play_lfsr },
    { "11a", "pixel 2x doubling (240-wide) full frame       460.8 KB", play_double2 },
    { "11b", "pixel 3x doubling (160-wide) full frame       460.8 KB", play_double3 },
    { "12",  "8-bit indexed full frame     256-colour       460.8 KB", play_indexed },
    { "13",  "pre-interlaced 4:1 pack      80 rows/field    115.2 KB", play_prei4 },
    { "14",  "dual-core ping-pong DMA      full frame       460.8 KB", play_pingpong },
    { "15",  "pure 2:1 row interlace (base)160 rows/field   230.4 KB", play_pure2 },
};

static void print_banner(void)
{
    printf("\n=== msp3520-video-lab — INTRO.mkv technique testbed ===\n");
    printf("40 MHz SPI (safe max; 80 MHz needs 4x-out-of-spec wiring)\n");
    printf("technique loops until you press any key\n\n");
    for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {
        printf("  %-4s  %s\n", s_entries[i].sel, s_entries[i].name);
    }
    printf("  c    set SPI clock (10/15/20/26.667/40 MHz lattice)\n");
    printf("  i    asset info\n");
    printf("\n");
}

static void set_clock_menu(void)
{
    char line[32];
    printf("current clock: %lu kHz\n", (unsigned long)lab_clock_khz());
    printf("new clock in kHz (10000, 15000, 20000, 26667, 40000): ");
    read_line("", line, sizeof(line));
    long khz = strtol(line, NULL, 10);
    if (khz < 1000 || khz > 40000) {
        printf("out of range — keeping %lu kHz\n", (unsigned long)lab_clock_khz());
        return;
    }
    if (!lab_panel_ready()) {
        return;
    }
    esp_err_t err = msp3520_set_clock((uint32_t)khz * 1000);
    if (err != ESP_OK) {
        printf("set_clock failed: %s\n", esp_err_to_name(err));
        return;
    }
    lab_set_clock_khz((uint32_t)khz);
    printf("clock set to %ld kHz (next technique runs at this speed)\n", khz);
}

void app_main(void)
{
    char line[32];

    console_init();
    if (!lab_assets_init()) {
        printf("FATAL: embedded assets failed validation — rebuild with"
               " tools/encode_lab.py\n");
        return;
    }
    print_banner();

    for (;;) {
        read_line("select> ", line, sizeof(line));
        if (strcmp(line, "i") == 0) {
            const lab_idx_t *b = lab_base();
            printf("base %uf %ux%u  pre-i4 %uf  half %uf  third %uf"
                   "  delta %uf  palette 256 x RGB666\n",
                   b->frames, b->w, b->h,
                   lab_prei4()->frames, lab_half()->frames,
                   lab_third()->frames, lab_delta()->frames_n);
            continue;
        }
        if (strcmp(line, "c") == 0) {
            set_clock_menu();
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < sizeof(s_entries) / sizeof(s_entries[0]); i++) {
            if (strcmp(line, s_entries[i].sel) == 0) {
                found = true;
                if (lab_panel_ready()) {
                    printf("  playing '%s' — any key stops\n", s_entries[i].sel);
                    s_entries[i].fn();
                    printf("  back at the menu\n");
                }
                break;
            }
        }
        if (!found && line[0] != '\0') {
            printf("  unknown selection '%s' (try 1, 7a, 11b, c, i)\n", line);
        }
        if (line[0] == '\0') {
            print_banner();
        }
    }
}

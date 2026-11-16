/*
 * players.h — one function per playback technique (see main.c's menu).
 * Every player loops the clip until a key is pressed, printing a live
 * stats line every couple of seconds, then returns to the menu.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Group 1 — interlacing & scanlines */
void play_quad4(void);            /* 1   pure 4:1 quad interlace          */
void play_scan2(void);            /* 2   static scanlines + 2:1           */
void play_scan4(void);            /* 3   static scanlines + 4:1           */
void play_oct8(void);             /* 4   pure 8:1 octuple interlace       */
void play_tri3(void);             /* 5   3:1 triple interlace             */
void play_col4(void);             /* 6   column 4:1 interlace             */
void play_quincunx_full(void);    /* 7a  quincunx, full height            */
void play_quincunx_half(void);    /* 7b  quincunx + 2:1 rows              */

/* Group 2 — temporal / delta */
void play_macro(void);            /* 8   macroblock 16x16 delta grid      */
void play_spans(void);            /* 9   span delta (runtime, vs prev f)  */
void play_lfsr(void);             /* 10  LFSR stochastic 4:1              */

/* Group 3 — storage / colour / pipeline */
void play_double2(void);          /* 11a pixel 2x doubling (240-wide)     */
void play_double3(void);          /* 11b pixel 3x doubling (160-wide)     */
void play_indexed(void);          /* 12  8-bit indexed full frame         */
void play_prei4(void);            /* 13  pre-interlaced 4:1 rows          */
void play_pingpong(void);         /* 14  dual-core ping-pong DMA          */
void play_pure2(void);            /* 15  pure 2:1 row interlace (bonus)   */

/* Lazily bring the panel up on first use (init + 40 MHz + black fill).
 * Returns false if the panel or SPI bus failed to initialize. */
bool lab_panel_ready(void);

/* Playback control: any printable key on the console stops the player. */
bool lab_should_stop(void);
void lab_stop_reset(void);

/* Live SPI clock (kHz) for the menu display; set via the 'c' command. */
uint32_t lab_clock_khz(void);
void lab_set_clock_khz(uint32_t khz);

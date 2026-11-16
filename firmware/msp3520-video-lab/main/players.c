/*
 * players.c — the lab's technique implementations.
 *
 * Shared machinery:
 *  - lab_lut expands one palette index to 3 ILI9488 RGB666 wire bytes.
 *  - labvid_stage(f) decodes frame f of the FULL compressed clip into a
 *    480x320 DRAM staging buffer (cached per frame) — every technique
 *    reads the staged frame. The old per-technique flash assets are gone:
 *    what used to be half.idx / third.idx / pre_i4.idx / delta.spans is
 *    now derived at runtime from the same staged frames (merge-LUT
 *    decimation for 11a/11b, row reordering for 13, a live zero-tolerance
 *    diff against the previous frame for 9).
 *  - Row/stripe/span techniques use the driver's queued-window session:
 *    CS stays LOW for the whole field while CASET/PASET/RAMWR commands and
 *    pixel data are queued as DMA transactions (the pre_cb callback flips
 *    D/C per transaction). One window costs 11 wire bytes; a field of 80
 *    one-row windows carries ~0.8% addressing overhead.
 *  - Full-frame techniques (11/12/14 and the 8-fallback) use the fixed
 *    full-screen window + 16-line block streaming of the v6/v7 players
 *    (1 RAMWR + 20 blocks per field), one shared 2-block buffer pool.
 *  - Triple line buffers + drain_to(8) keep two bursts in flight while
 *    guaranteeing a reused buffer's transaction has retired.
 *  - Fields are played back-to-back (bus-limited); the source frame
 *    advances every `ratio` fields. A slower bus = slow-motion content,
 *    same semantics as the previous projects.
 *
 * Technique notes (numbering matches the request):
 *   1/4/5/15 pure interlace: physical row == source line, full 320-line
 *     coverage over `ratio` consecutive fields.
 *   2/3 static scanlines: the screen is cleared to black once at start;
 *     odd physical rows stay dark forever, active rows are the even ones
 *     and show the even source lines (classic line-drop CRT squeeze).
 *   6 column interlace: MADCTL stays 0x28 (MV=1 already maps CASET to the
 *     480-px axis), so a 1-px CASET window selects one full-height column
 *     — columns 0,4,8,... per field, identical wire cost to row passes.
 *   7 quincunx: every row sends one 240-px half; the half alternates with
 *     row parity and field parity ((y + field) & 1), a checkerboard that
 *     covers every pixel over two fields without venetian-blind lines.
 *   8 macroblock delta: 30x20 grid of 16x16 blocks; a block is sent when
 *     the encoder's mbmap says its index bytes differ from the previous
 *     source frame (the bitmap ships as an asset — the player no longer
 *     needs two frames in RAM). Dirty blocks are grouped into horizontal
 *     runs, one window per run. Above 66% dirty the field falls back to
 *     one full-frame pass.
 *   9 span delta: RUNTIME diff — windows only where the staged frame's
 *     index differs from the previous frame (tolerance 0, so the panel
 *     reconverges to the source every frame; no drift to bound). The
 *     previous frame streams row-by-row through the same decoder (the
 *     staged frame is cached, so its ring is free). Calm frames are a
 *     few KB; burst frames collapse to full rows.
 *   10 LFSR: one permutation of the 320 rows (Fisher-Yates over
 *     xorshift32); each field sends 80 permutation entries, so every row
 *     is refreshed exactly once per 4 fields but in scattered order —
 *     combing becomes film-grain. Re-shuffled every cycle.
 *   11a/b pixel doubling: the 240/160-wide source is built from the
 *     staged 480-wide frame through the merge2 LUT (nearest palette entry
 *     to the average of each 2/3-index group) and stretched 2x/3x in the
 *     DMA buffer. Same wire bytes as a full frame; the test shows the
 *     storage trade-off, not a bus-rate change.
 *   12 indexed full frame: the 256-colour + dither codec on its own.
 *   13 pre-interlaced: field = rows p, p+4, ... of the staged frame — the
 *     wire shape is identical to technique 1 and to the old prepacked
 *     blob; the field rows are walked strictly in order.
 *   14 dual-core ping-pong: core 0 stages frames and expands blocks into
 *     double buffers, core 1 queues them to SPI — compare its field rate
 *     with 12 (which pays the decode serially).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "driver/uart.h"

#include "players.h"
#include "assets.h"
#include "labvid.h"
#include "msp3520.h"

#define CONSOLE_UART   UART_NUM_0
#define FIELD_LINES    320
#define ROW_BYTES      (LCD_H_RES * 3)          /* 1440 */
#define HALF_BYTES     (LCD_H_RES / 2 * 3)      /* 720  */
#define COL_BYTES      (FIELD_LINES * 3)        /* 960  */
#define MB             16                       /* macroblock edge       */
#define MB_COLS        (LCD_H_RES / MB)         /* 30 */
#define MB_ROWS        (FIELD_LINES / MB)       /* 20 */
#define MB_DIRTY_MAX   ((MB_COLS * MB_ROWS * 2) / 3)  /* 66% fallback */

/* ================= panel bring-up + console control ==================== */

static bool     s_panel_ok;
static uint32_t s_clock_khz = 40000;

uint32_t lab_clock_khz(void)
{
    return s_clock_khz;
}

void lab_set_clock_khz(uint32_t khz)
{
    s_clock_khz = khz;
}

bool lab_panel_ready(void)
{
    if (s_panel_ok) {
        return true;
    }
    esp_err_t err = msp3520_init();               /* 1 MHz + panel reset   */
    if (err == ESP_OK) {
        err = msp3520_set_clock(s_clock_khz * 1000);
    }
    if (err == ESP_OK) {
        err = msp3520_fill_screen(0x0000);        /* clean black slate     */
    }
    if (err != ESP_OK) {
        printf("panel init FAILED: %s\n", esp_err_to_name(err));
        return false;
    }
    s_panel_ok = true;
    return true;
}

static bool s_stop;

void lab_stop_reset(void)
{
    uint8_t c;
    while (uart_read_bytes(CONSOLE_UART, &c, 1, 0) == 1) {
    }
    s_stop = false;
}

bool lab_should_stop(void)
{
    uint8_t c;
    while (uart_read_bytes(CONSOLE_UART, &c, 1, 0) == 1) {
        if (c >= 0x20 && c < 0x7F) {
            s_stop = true;
        }
    }
    return s_stop;
}

/* ================= stats =============================================== */

typedef struct {
    const char *name;
    int64_t     t_last;
    uint32_t    fields;
    uint64_t    bytes;
} pstats_t;

static void ps_begin(pstats_t *s, const char *name)
{
    s->name = name;
    s->t_last = esp_timer_get_time();
    s->fields = 0;
    s->bytes = 0;
    msp3520_wire_bytes_reset();
}

static void ps_line(pstats_t *s, bool final)
{
    int64_t now = esp_timer_get_time();
    double sec = (double)(now - s->t_last) / 1e6;
    if (sec <= 0) {
        return;
    }
    uint64_t bytes = msp3520_wire_bytes();
    printf("%s[%s] %.1f fields/s  %.2f MB/s  %lu fields%s\n",
           final ? "  -> " : "  .. ",
           s->name,
           s->fields / sec,
           (bytes - s->bytes) / sec / 1e6,
           (unsigned long)s->fields,
           final ? " (stopped)" : "");
    fflush(stdout);
    s->t_last = now;
    s->fields = 0;
    s->bytes = bytes;
}

static void ps_tick(pstats_t *s)
{
    s->fields++;
    if (esp_timer_get_time() - s->t_last >= 2000000) {
        ps_line(s, false);
    }
}

/* ================= expansion helpers =================================== */

/* n indexed pixels -> n*factor wire pixels (factor 1..3), LUT lookup. */
static void expand_xN(const uint8_t *idx, uint8_t *dst, int n, int factor)
{
    const uint8_t (*lut)[3] = lab_lut;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = lut[idx[i]];
        for (int f = 0; f < factor; f++) {
            dst[0] = p[0];
            dst[1] = p[1];
            dst[2] = p[2];
            dst += 3;
        }
    }
}

/* One staged 480-index row, decimated factor:1 through the merge LUT and
 * stretched back 1:factor — the runtime 240/160-wide source of 11a/11b.
 * factor 2: merge pairs; factor 3: merge triples (two LUT steps). */
static void expand_merged(const uint8_t *row, uint8_t *dst, int factor)
{
    const uint8_t *m = lab_merge2();
    const uint8_t (*lut)[3] = lab_lut;
    if (factor == 2) {
        for (int x = 0; x < LCD_H_RES / 2; x++) {
            const uint8_t *p =
                lut[m[((uint32_t)row[2 * x] << 8) | row[2 * x + 1]]];
            dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2];
            dst[3] = p[0]; dst[4] = p[1]; dst[5] = p[2];
            dst += 6;
        }
    } else {
        for (int x = 0; x < LCD_H_RES / 3; x++) {
            uint8_t ab = m[((uint32_t)row[3 * x] << 8) | row[3 * x + 1]];
            const uint8_t *p = lut[m[((uint32_t)ab << 8) | row[3 * x + 2]]];
            for (int f = 0; f < 3; f++) {
                dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2];
                dst += 3;
            }
        }
    }
}

/* Expand one full-height column x of a 480-wide frame (top to bottom). */
static void expand_column(const uint8_t *frame, int x, uint8_t *dst)
{
    const uint8_t (*lut)[3] = lab_lut;
    for (int y = 0; y < FIELD_LINES; y++) {
        const uint8_t *p = lut[frame[(size_t)y * LCD_H_RES + x]];
        dst[0] = p[0];
        dst[1] = p[1];
        dst[2] = p[2];
        dst += 3;
    }
}

/* ================= shared field senders ================================ */

/* Shared 2-block DMA pool: field_fullframe() (single-task players) and
 * the ping-pong producer/consumer (technique 14) never run at the same
 * time, so one pool serves both. */
static uint8_t s_blk[2][LCD_BLOCK_BYTES];

/* Send `n` whole rows (physical row == source row) as one CS-low session:
 * one CASET + per-row PASET/RAMWR/data. Triple-buffered. */
static bool field_rows(const uint8_t *frame, const uint16_t *rows, int n)
{
    static uint8_t line[3][ROW_BYTES];
    int b = 0;
    if (msp3520_session_begin() != ESP_OK) {
        return false;
    }
    if (msp3520_qcaset(0, LCD_H_RES - 1) != ESP_OK) {
        msp3520_session_end();
        return false;
    }
    for (int i = 0; i < n; i++) {
        uint16_t y = rows[i];
        msp3520_drain_to(8);
        expand_xN(frame + (size_t)y * LCD_H_RES, line[b], LCD_H_RES, 1);
        if (msp3520_qpaset(y, y) != ESP_OK
         || msp3520_qramwr() != ESP_OK
         || msp3520_qdata(line[b], ROW_BYTES) != ESP_OK) {
            msp3520_session_end();
            return false;
        }
        b = (b + 1) % 3;
    }
    msp3520_session_end();
    return true;
}

/* Full 480x320 field through the fixed-window block path: 1 RAMWR + 20
 * 16-line DMA blocks. `src` is a frame of src_w indexed pixels, stretched
 * `factor` times horizontally (factor 1 = techniques 12/14 and 8's
 * full-frame fallback). */
static bool field_fullframe(const uint8_t *src, int src_w, int factor)
{
    if (msp3520_prepare_stream() != ESP_OK) {
        return false;
    }
    if (msp3520_field_begin() != ESP_OK) {
        return false;
    }
    for (int b = 0; b < FIELD_LINES / LCD_BLOCK_LINES; b++) {
        int i = b & 1;
        if (b >= TX_BLOCKS_IN_FLIGHT && msp3520_retire_block() != ESP_OK) {
            msp3520_field_end();
            return false;
        }
        uint8_t *dst = s_blk[i];
        for (int l = 0; l < LCD_BLOCK_LINES; l++) {
            expand_xN(src + (size_t)(b * LCD_BLOCK_LINES + l) * src_w,
                      dst, src_w, factor);
            dst += (size_t)LCD_H_RES * 3;
        }
        if (msp3520_queue_block(s_blk[i]) != ESP_OK) {
            msp3520_field_end();
            return false;
        }
    }
    for (int i = 0; i < TX_BLOCKS_IN_FLIGHT; i++) {
        (void)msp3520_retire_block();
    }
    msp3520_field_end();
    return true;
}

/* Full 480x320 field built from the STAGED frame decimated factor:1 via
 * the merge LUT (techniques 11a/11b) — wire-identical to field_fullframe
 * on a narrow source, but the source rows are derived at runtime. */
static bool field_fullframe_merged(const uint8_t *frame, int factor)
{
    if (msp3520_prepare_stream() != ESP_OK) {
        return false;
    }
    if (msp3520_field_begin() != ESP_OK) {
        return false;
    }
    for (int b = 0; b < FIELD_LINES / LCD_BLOCK_LINES; b++) {
        int i = b & 1;
        if (b >= TX_BLOCKS_IN_FLIGHT && msp3520_retire_block() != ESP_OK) {
            msp3520_field_end();
            return false;
        }
        uint8_t *dst = s_blk[i];
        for (int l = 0; l < LCD_BLOCK_LINES; l++) {
            expand_merged(frame + (size_t)(b * LCD_BLOCK_LINES + l) * LCD_H_RES,
                          dst, factor);
            dst += (size_t)LCD_H_RES * 3;
        }
        if (msp3520_queue_block(s_blk[i]) != ESP_OK) {
            msp3520_field_end();
            return false;
        }
    }
    for (int i = 0; i < TX_BLOCKS_IN_FLIGHT; i++) {
        (void)msp3520_retire_block();
    }
    msp3520_field_end();
    return true;
}

/* ================= group 1: interlacing & scanlines ==================== */

/* Pure row interlace, ratio 2/3/4/8: field k covers rows {k%ratio,
 * +ratio, ...}, source frame advances every ratio fields. */
static void play_row_interlace(const char *name, int ratio)
{
    pstats_t s;
    uint16_t rows[160];
    uint32_t frames = labvid_frames();
    lab_stop_reset();
    ps_begin(&s, name);
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        int p = (int)(k % (uint32_t)ratio);
        uint32_t f = (k / (uint32_t)ratio) % frames;
        const uint8_t *frame = labvid_stage(f);
        if (frame == NULL) {
            break;
        }
        int n = 0;
        for (int y = p; y < FIELD_LINES; y += ratio) {
            rows[n++] = (uint16_t)y;
        }
        if (!field_rows(frame, rows, n)) {
            break;
        }
        ps_tick(&s);
    }
    ps_line(&s, true);
}

void play_quad4(void)  { play_row_interlace("1 pure-4:1", 4); }
void play_oct8(void)   { play_row_interlace("4 pure-8:1", 8); }
void play_tri3(void)   { play_row_interlace("5 3:1", 3); }
void play_pure2(void)  { play_row_interlace("15 pure-2:1", 2); }

/* Static scanlines: clear to black, then only even physical rows carry
 * source lines (line-drop). 2:1: fields alternate active rows 0,4,8.. /
 * 2,6,10.. ; 4:1: four fields step through 0,8,16.. / 2,10.. / 4,12.. /
 * 6,14..  */
static void play_scan_interlace(const char *name, int ratio)
{
    pstats_t s;
    uint16_t rows[80];
    uint32_t frames = labvid_frames();
    lab_stop_reset();
    if (!lab_panel_ready()) {
        return;
    }
    msp3520_fill_screen(0x0000);               /* the once-per-boot clear */
    ps_begin(&s, name);
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        int p = (int)(k % (uint32_t)ratio);
        uint32_t f = (k / (uint32_t)ratio) % frames;
        const uint8_t *frame = labvid_stage(f);
        if (frame == NULL) {
            break;
        }
        int n = 0;
        /* active rows are phys 0,2,4,...; field p takes every ratio-th */
        for (int m = p; m < FIELD_LINES / 2; m += ratio) {
            rows[n++] = (uint16_t)(2 * m);
        }
        if (!field_rows(frame, rows, n)) {
            break;
        }
        ps_tick(&s);
    }
    ps_line(&s, true);
}

void play_scan2(void) { play_scan_interlace("2 scan+2:1", 2); }
void play_scan4(void) { play_scan_interlace("3 scan+4:1", 4); }

/* Column 4:1 interlace: one full-height 1-px column per window, columns
 * {k%4, +4, ...} — 120 windows, 115.2 KB/field, same as a row pass. */
void play_col4(void)
{
    pstats_t s;
    static uint8_t col[3][COL_BYTES];
    uint32_t frames = labvid_frames();
    lab_stop_reset();
    ps_begin(&s, "6 col-4:1");
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        int p = (int)(k & 3);
        uint32_t f = (k >> 2) % frames;
        const uint8_t *frame = labvid_stage(f);
        if (frame == NULL) {
            break;
        }
        int b = 0;
        if (msp3520_session_begin() != ESP_OK
         || msp3520_qpaset(0, FIELD_LINES - 1) != ESP_OK) {
            msp3520_session_end();
            break;
        }
        for (int x = p; x < LCD_H_RES; x += 4) {
            msp3520_drain_to(8);
            expand_column(frame, x, col[b]);
            if (msp3520_qcaset(x, x) != ESP_OK
             || msp3520_qramwr() != ESP_OK
             || msp3520_qdata(col[b], COL_BYTES) != ESP_OK) {
                msp3520_session_end();
                return;
            }
            b = (b + 1) % 3;
        }
        msp3520_session_end();
        ps_tick(&s);
    }
    ps_line(&s, true);
}

/* Quincunx: per row a 240-px half window; the half = (row + field) & 1. */
static void play_quincunx(const char *name, bool half_rows)
{
    pstats_t s;
    static uint8_t line[3][HALF_BYTES];
    uint32_t frames = labvid_frames();
    int ratio = half_rows ? 2 : 1;
    lab_stop_reset();
    ps_begin(&s, name);
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        uint32_t f = (k / (uint32_t)ratio) % frames;
        const uint8_t *frame = labvid_stage(f);
        if (frame == NULL) {
            break;
        }
        int b = 0;
        if (msp3520_session_begin() != ESP_OK) {
            break;
        }
        for (int y = (int)(k & 1) * (half_rows ? 1 : 0);
             y < FIELD_LINES; y += half_rows ? 2 : 1) {
            int half = (y + (int)k) & 1;
            msp3520_drain_to(8);
            expand_xN(frame + (size_t)y * LCD_H_RES + half * (LCD_H_RES / 2),
                      line[b], LCD_H_RES / 2, 1);
            if (msp3520_qcaset(half * (LCD_H_RES / 2),
                               half * (LCD_H_RES / 2) + LCD_H_RES / 2 - 1) != ESP_OK
             || msp3520_qpaset(y, y) != ESP_OK
             || msp3520_qramwr() != ESP_OK
             || msp3520_qdata(line[b], HALF_BYTES) != ESP_OK) {
                msp3520_session_end();
                return;
            }
            b = (b + 1) % 3;
        }
        msp3520_session_end();
        ps_tick(&s);
    }
    ps_line(&s, true);
}

void play_quincunx_full(void) { play_quincunx("7a quincunx", false); }
void play_quincunx_half(void) { play_quincunx("7b quincunx+2:1", true); }

/* ================= group 2: delta techniques =========================== */

/* Macroblock dirty bit from the encoder's bitmap (VLM1): bit (by*30+bx)
 * of frame f's 75-byte bitmap = "block differs from frame f-1". */
static inline bool mb_dirty(const uint8_t *bm, int bx, int by)
{
    int i = by * MB_COLS + bx;
    return (bm[i >> 3] >> (i & 7)) & 1;
}

/* Expand a horizontal run of macroblocks into wire bytes. */
static void expand_run(const uint8_t *fr, int bx0, int bx1, int by, uint8_t *dst)
{
    int w = (bx1 - bx0 + 1) * MB;
    for (int l = 0; l < MB; l++) {
        expand_xN(fr + (size_t)(by * MB + l) * LCD_H_RES + (size_t)bx0 * MB,
                  dst, w, 1);
        dst += (size_t)w * 3;
    }
}

/* Macroblock delta grid: send only 16x16 blocks whose bitmap bit is set,
 * grouped into runs; fall back to a full frame above 66% dirty. Frame 0
 * (and every loop wrap) refreshes everything. */
void play_macro(void)
{
    pstats_t s;
    struct { uint8_t by, bx0, bx1; } runs[MB_ROWS * MB_COLS];
    uint32_t frames = labvid_frames();
    lab_stop_reset();
    ps_begin(&s, "8 macroblock");
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        uint32_t f = k % frames;
        const uint8_t *fr = labvid_stage(f);
        if (fr == NULL) {
            break;
        }
        const uint8_t *bm = lab_mbmap() + (size_t)f * ((MB_COLS * MB_ROWS + 7) / 8);

        int nruns = 0, dirty = 0;
        if (f != 0) {
            for (int by = 0; by < MB_ROWS; by++) {
                int bx = 0;
                while (bx < MB_COLS) {
                    if (!mb_dirty(bm, bx, by)) {
                        bx++;
                        continue;
                    }
                    int bx1 = bx;
                    while (bx1 + 1 < MB_COLS && mb_dirty(bm, bx1 + 1, by)) {
                        bx1++;
                    }
                    runs[nruns].by = (uint8_t)by;
                    runs[nruns].bx0 = (uint8_t)bx;
                    runs[nruns].bx1 = (uint8_t)bx1;
                    nruns++;
                    dirty += bx1 - bx + 1;
                    bx = bx1 + 1;
                }
            }
        }
        if (f == 0 || dirty > MB_DIRTY_MAX) {
            if (!field_fullframe(fr, LCD_H_RES, 1)) {
                break;
            }
            ps_tick(&s);
            continue;
        }

        int b = 0;
        if (msp3520_session_begin() != ESP_OK) {
            break;
        }
        for (int i = 0; i < nruns; i++) {
            /* One window = 4 queued transactions (3 command + 1 data);
             * TX_RING_SLOTS is 16, so with two buffers live+queued stays
             * <= 8 <= 16 and drain_to(4) keeps a reused buffer's data
             * transaction retired before expand_run() touches it — the
             * same two-deep invariant the row path holds with three
             * buffers and drain_to(8). */
            msp3520_drain_to(4);
            expand_run(fr, runs[i].bx0, runs[i].bx1, runs[i].by, s_blk[b]);
            int x0 = runs[i].bx0 * MB;
            int x1 = (runs[i].bx1 + 1) * MB - 1;
            int y0 = runs[i].by * MB;
            size_t len = (size_t)(x1 - x0 + 1) * MB * 3;
            if (msp3520_qcaset(x0, x1) != ESP_OK
             || msp3520_qpaset(y0, y0 + MB - 1) != ESP_OK
             || msp3520_qramwr() != ESP_OK
             || msp3520_qdata(s_blk[b], len) != ESP_OK) {
                msp3520_session_end();
                return;
            }
            b = (b + 1) % 2;
        }
        msp3520_session_end();
        ps_tick(&s);
    }
    ps_line(&s, true);
}

/* Span delta, runtime: windows only where the staged frame's index differs
 * from the PREVIOUS frame (tolerance 0 — every changed pixel is sent, so
 * the panel reconverges to the source each frame; nothing can drift).
 * The previous frame streams row-by-row through the SAME decoder that
 * staged the current frame — safe because labvid_stage() caches the
 * staged frame (the decode ring is free once staging is done), and the
 * two never interleave: stage(f), then rows_begin(f-1) + 320 x
 * rows_next() reading the cached frame, repeat. Heavily fragmented rows
 * (> 32 spans or > 92% coverage) collapse to one full-row span, like
 * the encoder-side asset did. */
void play_spans(void)
{
    pstats_t s;
    static uint8_t span[3][ROW_BYTES];
    static uint8_t prevrow[LCD_H_RES];
    static struct { uint16_t x0, ln; } spans[LCD_H_RES / 2];
    uint32_t frames = labvid_frames();

    lab_stop_reset();
    ps_begin(&s, "9 span-delta");
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        uint32_t f = k % frames;
        const uint8_t *frame = labvid_stage(f);
        if (frame == NULL
         || !labvid_rows_begin((f + frames - 1) % frames)) {
            break;
        }
        int b = 0;
        if (msp3520_session_begin() != ESP_OK
         || msp3520_qcaset(0, LCD_H_RES - 1) != ESP_OK) {
            msp3520_session_end();
            break;
        }
        bool ok = true;
        for (int y = 0; y < FIELD_LINES && ok; y++) {
            const uint8_t *row = frame + (size_t)y * LCD_H_RES;
            if (!labvid_rows_next(prevrow)) {
                break;
            }
            int nspans = 0, cover = 0, x = 0;
            while (x < LCD_H_RES) {
                if (row[x] == prevrow[x]) {
                    x++;
                    continue;
                }
                int x0 = x;
                while (x < LCD_H_RES && row[x] != prevrow[x]) {
                    x++;
                }
                spans[nspans].x0 = (uint16_t)x0;
                spans[nspans].ln = (uint16_t)(x - x0);
                cover += x - x0;
                nspans++;
            }
            bool full = nspans > 32 || cover > (LCD_H_RES * 92) / 100;
            if (full) {
                msp3520_drain_to(8);
                expand_xN(row, span[b], LCD_H_RES, 1);
                if (msp3520_qpaset(y, y) != ESP_OK
                 || msp3520_qramwr() != ESP_OK
                 || msp3520_qdata(span[b], ROW_BYTES) != ESP_OK) {
                    ok = false;
                    break;
                }
                b = (b + 1) % 3;
            } else {
                for (int sp = 0; sp < nspans; sp++) {
                    msp3520_drain_to(8);
                    expand_xN(row + spans[sp].x0, span[b], spans[sp].ln, 1);
                    if (msp3520_qpaset(y, y) != ESP_OK
                     || msp3520_qcaset(spans[sp].x0,
                                       spans[sp].x0 + spans[sp].ln - 1) != ESP_OK
                     || msp3520_qramwr() != ESP_OK
                     || msp3520_qdata(span[b], (size_t)spans[sp].ln * 3) != ESP_OK) {
                        ok = false;
                        break;
                    }
                    b = (b + 1) % 3;
                }
            }
        }
        msp3520_session_end();
        if (!ok) {
            break;
        }
        ps_tick(&s);
    }
    ps_line(&s, true);
}

/* LFSR stochastic 4:1: one permutation of all 320 rows (Fisher-Yates over
 * xorshift32); field k sends 80 consecutive permutation entries, so all
 * rows refresh exactly once per 4 fields — but scattered, turning combing
 * into film-grain noise. Re-shuffled once per clip cycle. */
static uint32_t xorshift32(uint32_t *st)
{
    *st ^= *st << 13;
    *st ^= *st >> 17;
    *st ^= *st << 5;
    return *st;
}

void play_lfsr(void)
{
    pstats_t s;
    static uint16_t perm[FIELD_LINES];
    uint32_t frames = labvid_frames();
    uint32_t seed = 0xC0FFEE1u;
    lab_stop_reset();
    ps_begin(&s, "10 lfsr-4:1");
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        if (k % ((uint32_t)frames * 4) == 0) {
            for (int i = 0; i < FIELD_LINES; i++) {
                perm[i] = (uint16_t)i;
            }
            for (int i = FIELD_LINES - 1; i > 0; i--) {
                int j = (int)(xorshift32(&seed) % (uint32_t)(i + 1));
                uint16_t t = perm[i];
                perm[i] = perm[j];
                perm[j] = t;
            }
        }
        int g = (int)(k & 3);
        uint32_t f = (k >> 2) % frames;
        const uint8_t *frame = labvid_stage(f);
        if (frame == NULL) {
            break;
        }
        if (!field_rows(frame, perm + g * 80, 80)) {
            break;
        }
        ps_tick(&s);
    }
    ps_line(&s, true);
}

/* ================= group 3: storage / colour / pipeline ================ */

/* Pixel doubling: the narrow source is built from the staged frame
 * through the merge LUT and stretched to full width in the DMA buffer —
 * identical wire bytes to technique 12; the test shows the storage
 * trade-off, not a bus-rate change. */
static void play_doubled(const char *name, int factor)
{
    pstats_t s;
    uint32_t frames = labvid_frames();
    lab_stop_reset();
    ps_begin(&s, name);
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        const uint8_t *frame = labvid_stage(k % frames);
        if (frame == NULL || !field_fullframe_merged(frame, factor)) {
            break;
        }
        ps_tick(&s);
    }
    ps_line(&s, true);
}

void play_double2(void) { play_doubled("11a double-2x", 2); }
void play_double3(void) { play_doubled("11b double-3x", 3); }

/* Indexed full frame: the 256-colour + dither codec on its own. */
void play_indexed(void)
{
    pstats_t s;
    uint32_t frames = labvid_frames();
    lab_stop_reset();
    ps_begin(&s, "12 indexed");
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        const uint8_t *frame = labvid_stage(k % frames);
        if (frame == NULL || !field_fullframe(frame, LCD_H_RES, 1)) {
            break;
        }
        ps_tick(&s);
    }
    ps_line(&s, true);
}

/* Pre-interlaced 4:1: field = rows p, p+4, ... of the staged frame, read
 * strictly in order — the wire shape is identical to technique 1 (and to
 * the old prepacked blob: every byte read is sent). */
void play_prei4(void)
{
    pstats_t s;
    static uint8_t line[3][ROW_BYTES];
    uint32_t frames = labvid_frames();
    uint32_t fields_total = frames * 4;
    lab_stop_reset();
    ps_begin(&s, "13 pre-i4");
    for (uint32_t k = 0; !lab_should_stop(); k++) {
        uint32_t fld = k % fields_total;
        int phase = (int)(fld & 3);
        const uint8_t *frame = labvid_stage(fld >> 2);
        if (frame == NULL) {
            break;
        }
        int b = 0;
        if (msp3520_session_begin() != ESP_OK
         || msp3520_qcaset(0, LCD_H_RES - 1) != ESP_OK) {
            msp3520_session_end();
            break;
        }
        for (int i = 0; i < FIELD_LINES / 4; i++) {
            int y = phase + i * 4;
            msp3520_drain_to(8);
            expand_xN(frame + (size_t)y * LCD_H_RES, line[b], LCD_H_RES, 1);
            if (msp3520_qpaset(y, y) != ESP_OK
             || msp3520_qramwr() != ESP_OK
             || msp3520_qdata(line[b], ROW_BYTES) != ESP_OK) {
                msp3520_session_end();
                return;
            }
            b = (b + 1) % 3;
        }
        msp3520_session_end();
        ps_tick(&s);
    }
    ps_line(&s, true);
}

/* Dual-core ping-pong (technique 14): core 0 stages frames and expands
 * 16-line blocks into double buffers, core 1 feeds them to the SPI — the
 * same full-frame output as technique 12, so the field-rate difference is
 * pure pipeline (12 pays the frame decode serially, 14 overlaps it).
 *
 * Handshake per block i: producer takes free_[i], fills s_blk[i], gives
 * ready[i]; consumer takes ready[i], queues s_blk[i], retires the block
 * queued before it, then releases that buffer. Two blocks are in flight
 * mid-field; at a field boundary the last block is retired BEFORE CS goes
 * high. The semaphores and state are static and reused across runs (the
 * producer might outlive play_pingpong() by one 200 ms timeout). */

typedef struct {
    volatile bool    stop;
    volatile bool    done;
} pp_t;

static pp_t              pp_state;
static SemaphoreHandle_t pp_ready[2];
static SemaphoreHandle_t pp_free[2];
static SemaphoreHandle_t pp_finished;

static void pp_init_once(void)
{
    if (pp_ready[0] == NULL) {
        for (int i = 0; i < 2; i++) {
            pp_ready[i] = xSemaphoreCreateBinary();
            pp_free[i]  = xSemaphoreCreateBinary();
        }
        pp_finished = xSemaphoreCreateBinary();
    }
    /* clean slate: no pending buffers, both slots free */
    for (int i = 0; i < 2; i++) {
        while (xSemaphoreTake(pp_ready[i], 0)) {
        }
        xSemaphoreGive(pp_free[i]);
    }
    while (xSemaphoreTake(pp_finished, 0)) {
    }
}

static void pp_producer(void *arg)
{
    pp_t *pp = arg;
    uint32_t frames = labvid_frames();
    uint32_t k = 0;
    while (!pp->stop) {
        const uint8_t *frame = labvid_stage(k % frames);
        if (frame == NULL) {
            break;
        }
        for (int b = 0; b < FIELD_LINES / LCD_BLOCK_LINES && !pp->stop; b++) {
            int i = b & 1;
            if (!xSemaphoreTake(pp_free[i], pdMS_TO_TICKS(200))) {
                continue;
            }
            uint8_t *dst = s_blk[i];
            for (int l = 0; l < LCD_BLOCK_LINES; l++) {
                expand_xN(frame + (size_t)(b * LCD_BLOCK_LINES + l) * 480,
                          dst, 480, 1);
                dst += ROW_BYTES;
            }
            xSemaphoreGive(pp_ready[i]);
        }
        k++;
    }
    pp->done = true;
    vTaskDelete(NULL);
}

static void pp_consumer(void *arg)
{
    pp_t *pp = arg;
    pstats_t s;
    int blocks_per_field = FIELD_LINES / LCD_BLOCK_LINES;
    int outstanding = 0;
    int prev = -1;
    ps_begin(&s, "14 ping-pong");
    msp3520_prepare_stream();
    for (uint32_t blocks = 0; !pp->done; blocks++) {
        int i = (int)(blocks & 1);
        if (!xSemaphoreTake(pp_ready[i], pdMS_TO_TICKS(100))) {
            continue;
        }
        if ((blocks % blocks_per_field) == 0) {
            msp3520_field_begin();
        }
        msp3520_queue_block(s_blk[i]);
        outstanding++;
        if ((blocks % blocks_per_field) == blocks_per_field - 1) {
            /* field boundary: retire everything still in flight (FIFO:
             * the previous block, then this one) and only then CS high */
            if (prev >= 0) {
                msp3520_retire_block();
                outstanding--;
                xSemaphoreGive(pp_free[prev]);
            }
            msp3520_retire_block();
            outstanding--;
            xSemaphoreGive(pp_free[i]);
            prev = -1;
            msp3520_field_end();
            ps_tick(&s);
            if (lab_should_stop()) {
                pp->stop = true;
            }
        } else if (prev >= 0) {
            msp3520_retire_block();
            outstanding--;
            xSemaphoreGive(pp_free[prev]);
            prev = i;
        } else {
            prev = i;
        }
    }
    while (outstanding > 0) {
        msp3520_retire_block();
        outstanding--;
    }
    ps_line(&s, true);
    xSemaphoreGive(pp_finished);
    vTaskDelete(NULL);
}

void play_pingpong(void)
{
    pp_init_once();
    pp_state.stop = false;
    pp_state.done = false;
    lab_stop_reset();
    xTaskCreatePinnedToCore(pp_producer, "pp_prod", 4096, &pp_state, 5, NULL, 0);
    xTaskCreatePinnedToCore(pp_consumer, "pp_cons", 4096, &pp_state, 5, NULL, 1);
    xSemaphoreTake(pp_finished, portMAX_DELAY);
}

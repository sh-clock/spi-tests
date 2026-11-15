/*
 * main.c — MSP3520 field-interlaced video player + SPI clock sweep (v7).
 *
 * Plays the converted video (embedded video.bin, 480x320) as FIELD-NATIVE
 * half-frames: each field is one 240-px half of the display (left x 0..239,
 * then right x 240..479) carrying the source frame at its OWN timestamp —
 * consecutive fields are one field period apart in content. Fields stream
 * straight from the decoder into 16-line half-window DMA blocks: no PSRAM
 * stage, no full frame ever exists in RAM.
 *
 * v7 adds the DIRTY schedule (blob flags bit3): each field starts with a
 * block bitmap and carries ONLY the 16-line blocks whose wire bytes differ
 * from the previous field that wrote the same half (field k-2) — the
 * ILI9488 GRAM persists, so unchanged blocks are simply not re-sent (and
 * not even stored in the blob). Present blocks are grouped into contiguous
 * runs; each run is one address window + one RAMWR session. Static scenes
 * leave the bus idle; the status line shows the live bus utilization.
 * The pixel data itself is Bayer 8x8 dithered offline before the RGB666
 * quantizer (banding -> fine frame-stable grain).
 *
 * Why interlace at all: a full RGB666 frame is 460,800 B; the 40 MHz bus
 * tops out at ~10.85 full frames/s. A half-frame is 230,400 B, so the same
 * bus carries ~21.7 fields/s — and because every field carries a FRESH
 * source sample, motion advances at ~21.7 steps/s instead of ~10.85. The
 * trade (accepted): each field is half the picture, so there is a
 * stationary mid-screen seam and fast pans shear across it (~1 field
 * period of motion). This is the classic interlace trade realized in the
 * only wire-cheap shape the ILI9488 offers: contiguous half-windows (true
 * even/odd row interlace would need a CASET+PASET+RAMWR per line — 1280
 * transactions/frame, the mistake v1-v3 made).
 *
 * The sweep runs the SAME video at 10..80 MHz SPI clock, 5 MHz steps. At
 * each frequency the video loops for PLAY_SECONDS while you watch; press
 * y/n/r/q at any moment (single keypress):
 *    y = clean   n = artifacts   r = restart this frequency   q = stop
 * After the time runs out you get the same question once more.
 *
 * Field rates to expect: with RGB666 a field is 1,843,200 bits, so the
 * ideal field time is bits/clock; the 40 MHz bus manages ~21.7 fields/s
 * (~20.6 measured with transaction overhead). NOTE the ESP32-S3 cannot
 * generate every clock: requests of 35-60 MHz actually run at 40 MHz,
 * 65-80 MHz at 80 MHz (the ILI9488 is 4x out of spec at 80 — expect snow
 * there). The console shows the measured field rate at every frequency.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/uart.h"

#include "msp3520.h"
#include "video.h"

/* the embedded video (see main/CMakeLists.txt: EMBED_FILES video.bin) */
extern const uint8_t video_start[] asm("_binary_video_bin_start");
extern const uint8_t video_end[]   asm("_binary_video_bin_end");

/* Line buffers are sized for the largest supported wire format; the bytes
 * actually sent per line are 480 * video_hdr()->bpp (see msp3520_queue_block). */

/* ================= configuration ================= */

#define CONSOLE_UART     UART_NUM_0

#define FREQ_START_MHZ   10
#define FREQ_STOP_MHZ    80
#define FREQ_STEP_MHZ    5
#define STEP_COUNT       ((FREQ_STOP_MHZ - FREQ_START_MHZ) / FREQ_STEP_MHZ + 1)

#define PLAY_SECONDS     10          /* video loop time per frequency */

#define FRAME_BLOCKS     (LCD_V_RES / LCD_BLOCK_LINES)   /* 20 */

/* double block buffers: one in flight on SPI, one being decoded */
static uint8_t s_block[TX_BLOCKS_IN_FLIGHT][LCD_BLOCK_BYTES];

/* --- field pacing ----------------------------------------------------------
 * The blob is baked at FIELD_HZ_X100/100 fields/s (convert_video.py
 * --field-rate; 21.70 = the 40 MHz RGB666 wire ceiling), so real-time
 * playback = one field every 1e8/2170 us. A slower bus simply falls behind
 * (the clip plays in slow motion — exactly what v5 progressive did below
 * its 15 fps target). Keep this in sync with the converter default. */
#define FIELD_HZ_X100   2170
#define FIELD_US(f)     ((int64_t)(f) * 100000000LL / FIELD_HZ_X100)

/* ================= console helpers (same style as project 1) =========== */

static void console_init(void)
{
    uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0);
}

static void print_x10(uint64_t value_x10)
{
    printf("%llu.%llu", (unsigned long long)(value_x10 / 10),
           (unsigned long long)(value_x10 % 10));
}

static char ask_key(const char *prompt, const char *valid)
{
    uint8_t c;
    printf("%s", prompt);
    for (;;) {
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n' || c == ' ') {
            continue;
        }
        char ch = (char)tolower((int)c);
        if (ch != 0 && strchr(valid, ch) != NULL) {
            printf("%c\n", ch);
            while (uart_read_bytes(CONSOLE_UART, &c, 1, pdMS_TO_TICKS(30)) == 1) {
                /* swallow trailing Enter */
            }
            return ch;
        }
        printf("  [press one of: %s]\n%s", valid, prompt);
    }
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

/* Non-blocking single key read (0 = nothing pressed). */
static char poll_key(void)
{
    uint8_t c;
    if (uart_read_bytes(CONSOLE_UART, &c, 1, 0) == 1) {
        if (c == '\r' || c == '\n' || c == ' ') {
            return 0;
        }
        char ch = (char)tolower((int)c);
        if (ch != 0 && strchr("ynrqd", ch) != NULL) {
            /* echo + drain the rest of the line */
            printf("%c\n", ch);
            while (uart_read_bytes(CONSOLE_UART, &c, 1, pdMS_TO_TICKS(30)) == 1) {
            }
            return ch;
        }
    }
    return 0;
}

/* ================= the player ================= */

typedef struct {
    uint32_t mhz;
    bool     tested;
    bool     pass;
    uint32_t fps_x10;     /* fields/s x10; 0 = not measured */
} step_t;

static step_t s_steps[STEP_COUNT];

/* Push one field: a 240-px half-window (half=0 left x 0..239, half=1 right
 * x 240..479) carrying only the blocks named in the bitmap (bit b = 16-line
 * block b of this half). The ILI9488 GRAM keeps whatever the previous
 * same-half field wrote, so clean blocks are simply not re-sent. Present
 * blocks are grouped into contiguous runs; each run is one address window
 * (msp3520_prepare_half_run) + one RAMWR session of 16-line DMA blocks.
 * bitmap == 0 (nothing changed) touches no SPI at all. MIV2 blobs report
 * an all-present bitmap, so they stream exactly like v6.
 * Returns false only if the video decoder hard-fails (corrupt blob). */
static bool push_field(int half, uint32_t bitmap)
{
    const video_hdr_t *hdr = video_hdr();
    const size_t half_stride = (size_t)(LCD_H_RES / 2) * hdr->bpp;  /* 720 */
    const uint32_t present = bitmap & ((1u << FRAME_BLOCKS) - 1u);

    if (present == 0) {
        return true;               /* GRAM already holds this field */
    }
    int b = 0;
    while (b < FRAME_BLOCKS) {
        if (!(present & (1u << b))) {
            b++;
            continue;
        }
        int e = b;                 /* extend to the end of the run */
        while (e + 1 < FRAME_BLOCKS && (present & (1u << (e + 1)))) {
            e++;
        }
        ESP_ERROR_CHECK(msp3520_prepare_half_run(
            half, b * LCD_BLOCK_LINES, (e + 1) * LCD_BLOCK_LINES - 1));
        ESP_ERROR_CHECK(msp3520_field_begin());      /* CS low + RAMWR */
        int live = 0;
        int i_tx = 0;
        for (int blk = b; blk <= e; blk++) {
            /* buffers alternate by QUEUE order (FIFO retire discipline),
             * not by block index — runs start at arbitrary blocks */
            uint8_t *dst = s_block[i_tx & (TX_BLOCKS_IN_FLIGHT - 1)];
            i_tx++;
            for (int l = 0; l < LCD_BLOCK_LINES; l++) {
                if (!video_read_line(dst + (size_t)l * half_stride)) {
                    /* end of the video inside a field cannot happen (the
                     * blob ends on field boundaries), but be safe */
                    video_rewind();
                    if (!video_read_line(dst + (size_t)l * half_stride)) {
                        /* corrupt blob: drain + CS high so the panel and
                         * the SPI queue are in a clean state */
                        while (live > 0) {
                            msp3520_retire_block();
                            live--;
                        }
                        msp3520_field_end();
                        return false;
                    }
                }
            }
            /* a half block is 11,520 B; the s_block rows hold 23,040 B */
            ESP_ERROR_CHECK(msp3520_queue_half_block(dst));
            if (++live >= TX_BLOCKS_IN_FLIGHT) {
                ESP_ERROR_CHECK(msp3520_retire_block());
                live--;
            }
        }
        while (live > 0) {
            ESP_ERROR_CHECK(msp3520_retire_block());
            live--;
        }
        msp3520_field_end();
        b = e + 1;
    }
    return true;
}

/* Sleep until target_us, in a watchdog-friendly way (real delays for the
 * bulk, a short spin only for the last ~11 ms; the SPI retire waits in the
 * playback loop also block properly, which keeps the idle task running). */
static void pace_until(int64_t target_us)
{
    for (;;) {
        int64_t remain = target_us - esp_timer_get_time();
        if (remain <= 0) {
            return;
        }
        if (remain > 15000) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((remain - 10000) / 1000)));
        } else if (remain > 11000) {
            vTaskDelay(1);
        } else {
            while (esp_timer_get_time() < target_us) {
                /* brief spin */
            }
            return;
        }
    }
}

/* Play the looping video for `seconds` at the current SPI clock.
 * Fields are paced to FIELD_HZ_X100/100 per second whenever the bus
 * finishes early (real time for a blob baked at the same rate); when the
 * bus is slower, playback simply runs at whatever the bus allows.
 * Returns a verdict key ('y','n','r','q') pressed during playback, or 0 if
 * the time simply ran out. Fills *fields_out and *rate_x10 (fields/s x10). */
static char play_segment(int seconds, uint32_t *fields_out, uint32_t *rate_x10)
{
    uint32_t fields = 0;
    uint32_t blocks = 0;               /* dirty blocks pushed to the panel */
    uint64_t wire_bytes = 0;           /* pixel bytes pushed to the panel */
    int64_t t0 = esp_timer_get_time();
    int64_t deadline = t0 + (int64_t)seconds * 1000000;
    int64_t next_status = t0 + 500000;

    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            break;
        }

        /* start the next field; at the loop point start over. Field 0 is
         * fully present (nothing precedes it), so a rewind repaints the
         * whole panel cleanly. */
        uint32_t bitmap;
        uint32_t fidx = video_field_index();
        if (!video_next_field(&bitmap)) {
            video_rewind();
            fidx = 0;
            if (!video_next_field(&bitmap)) {
                break;                 /* pathological: empty blob */
            }
        }

        /* stream the field: even index = left half, odd = right half;
         * only the blocks named in the bitmap go on the wire */
        if (!push_field((int)(fidx & 1u), bitmap)) {
            break;
        }
        {
            uint32_t n = (uint32_t)__builtin_popcount(
                bitmap & ((1u << FRAME_BLOCKS) - 1u));
            blocks += n;
            wire_bytes += (uint64_t)n * LCD_HALF_BLOCK_BYTES;
        }
        fields++;

        /* real time: 21.70 fields/s when the bus keeps up (matches the
         * converter's --field-rate bake) */
        pace_until(t0 + FIELD_US(fields));

        /* live keys: judge while watching, no need to wait for the end */
        char k = poll_key();
        if (k == 'd') {
            /* live experiment: cycle SCK/MOSI GPIO drive capability
             * (weaker edges ring less on jumper wires — see README) */
            static int s_drive = -1;      /* -1 = chip default, then 0..3 */
            s_drive = (s_drive >= 3) ? 0 : s_drive + 1;
            msp3520_set_edge_drive(s_drive);
            printf("\n  [SCK/MOSI GPIO drive capability -> %d]\n", s_drive);
            continue;
        }
        if (k != 0) {
            *fields_out = fields;
            int64_t el = esp_timer_get_time() - t0;
            *rate_x10 = el > 0 ? (uint32_t)((uint64_t)fields * 10 * 1000000 / el) : 0;
            printf("\n");
            return k;
        }

        now = esp_timer_get_time();
        if (now >= next_status) {
            int64_t el = now - t0;
            uint32_t f10 = el > 0
                ? (uint32_t)((uint64_t)fields * 10 * 1000000 / el) : 0;
            /* share of the SPI bus actually carrying pixels (x10 %):
             * wire bits / (elapsed us * clock Hz) */
            uint64_t capacity = (uint64_t)((uint64_t)el) * msp3520_get_clock();
            uint32_t util10 = capacity > 0
                ? (uint32_t)(wire_bytes * 80 / capacity) : 0;
            printf("\r  playing: %u fields, ", (unsigned)fields);
            print_x10(f10);
            printf(" fields/s, ");
            print_x10(util10);
            printf("%% bus   [y/n/r/q/d]");
            fflush(stdout);
            next_status += 500000;
        }
    }
    printf("\n");

    *fields_out = fields;
    int64_t el = esp_timer_get_time() - t0;
    *rate_x10 = el > 0 ? (uint32_t)((uint64_t)fields * 10 * 1000000 / el) : 0;
    return 0;
}

/* Run one sweep frequency. Returns false when the user pressed q. */
static bool run_step(int idx)
{
    step_t *st = &s_steps[idx];
    char key;
    uint32_t fields = 0;

    /* clean panel + fresh clock for every frequency */
    ESP_ERROR_CHECK(msp3520_reinit_panel());
    ESP_ERROR_CHECK(msp3520_set_clock(st->mhz * 1000000u));
    ESP_ERROR_CHECK(msp3520_prepare_stream());
    video_rewind();

    printf("\n  [%2d/%d] %2u MHz\n", idx + 1, STEP_COUNT, (unsigned)st->mhz);

    for (;;) {
        key = play_segment(PLAY_SECONDS, &fields, &st->fps_x10);
        if (key == 'r') {
            video_rewind();
            continue;
        }
        if (key == 0) {
            key = ask_key("   -> clean? [y]es [n]o [r]eplay [q]uit: ", "ynrq");
            if (key == 'r') {
                video_rewind();
                continue;
            }
        }
        break;
    }

    st->tested = true;
    st->pass = (key == 'y');
    printf("   %2u MHz: ", (unsigned)st->mhz);
    print_x10(st->fps_x10);
    printf(" fields/s, %u fields -> %s\n", (unsigned)fields,
           st->pass ? "PASS" : "FAIL");

    if (key == 'q') {
        printf("\n  (test stopped by user)\n");
        return false;
    }
    return true;
}

static void print_summary(void)
{
    uint32_t best = 0;
    int tested = 0, passed = 0;

    printf("\n");
    printf("  ======================== SUMMARY ========================\n");
    printf("   MHz  fields/s  verdict\n");
    printf("  --------------------------------------------------------\n");
    for (int i = 0; i < STEP_COUNT; i++) {
        const step_t *st = &s_steps[i];
        if (!st->tested) {
            continue;
        }
        tested++;
        if (st->pass) {
            passed++;
            if (st->mhz > best) {
                best = st->mhz;
            }
        }
        printf("   %2u    ", (unsigned)st->mhz);
        print_x10(st->fps_x10);
        printf("    %s\n", st->pass ? "PASS" : "FAIL");
    }
    if (tested == 0) {
        printf("   (no steps were tested)\n");
        return;
    }

    printf("  ========================================================\n");
    if (best > 0) {
        uint32_t recommend = (best > FREQ_START_MHZ) ? best - FREQ_STEP_MHZ : best;
        printf("  Highest CLEAN frequency : %u MHz  (%d of %d steps passed)\n",
               (unsigned)best, passed, tested);
        printf("  Suggested for projects  : %u MHz or lower\n", (unsigned)recommend);
        printf("  Video is the harshest test there is: hours of continuous\n");
        printf("  streaming, every byte on the wire. One 5 MHz step of margin\n");
        printf("  is cheap insurance.\n");
    } else {
        printf("  No frequency played back cleanly!\n");
        printf("  -> Check wiring (short SCK/MOSI jumpers!), power, and the\n");
        printf("     README troubleshooting table. 10 MHz must work.\n");
    }
}

/* Optional finale: continuous looping playback at one chosen frequency. */
static void cinema_loop(void)
{
    char line[16];
    uint32_t fields = 0, rate_x10 = 0;

    read_line("\nContinuous playback? Enter SPI MHz (or just Enter to finish) > ",
              line, sizeof(line));
    if (line[0] == '\0') {
        return;
    }
    long f = strtol(line, NULL, 10);
    if (f < 1 || f > 80) {
        printf("  Enter a whole number between 1 and 80; skipping.\n");
        return;
    }
    ESP_ERROR_CHECK(msp3520_reinit_panel());
    ESP_ERROR_CHECK(msp3520_set_clock((uint32_t)f * 1000000u));
    ESP_ERROR_CHECK(msp3520_prepare_stream());
    video_rewind();

    printf("  Playing the video on loop at %ld MHz - press y/n/r/q to stop.\n", f);
    play_segment(3600, &fields, &rate_x10);
    printf("  Stopped after %u fields.\n", (unsigned)fields);
}

/* After the sweep: re-test individual frequencies. */
static void retest_loop(void)
{
    char line[16];

    for (;;) {
        read_line("\nFrequency (MHz) to re-test, or just Enter to finish > ",
                  line, sizeof(line));
        if (line[0] == '\0') {
            return;
        }
        long f = strtol(line, NULL, 10);
        if (f < 1 || f > 80) {
            printf("  Please enter a whole number between 1 and 80.\n");
            continue;
        }
        printf("  Re-testing %ld MHz ...\n", f);
        ESP_ERROR_CHECK(msp3520_reinit_panel());
        ESP_ERROR_CHECK(msp3520_set_clock((uint32_t)f * 1000000u));
        ESP_ERROR_CHECK(msp3520_prepare_stream());
        video_rewind();

        char key;
        uint32_t fields = 0, rate_x10 = 0;
        for (;;) {
            key = play_segment(PLAY_SECONDS, &fields, &rate_x10);
            if (key == 'r') {
                video_rewind();
                continue;
            }
            if (key == 0) {
                key = ask_key("  -> clean? [y]es [n]o [r]eplay [q]uit: ", "ynrq");
                if (key == 'r') {
                    video_rewind();
                    continue;
                }
            }
            break;
        }
        for (int i = 0; i < STEP_COUNT; i++) {
            if (s_steps[i].mhz == (uint32_t)f) {
                s_steps[i].tested = true;
                s_steps[i].fps_x10 = rate_x10;
                s_steps[i].pass = (key == 'y');
                break;
            }
        }
        if (key == 'q') {
            return;
        }
        print_summary();
    }
}

/* ================= boot ================= */

static void print_banner(const video_hdr_t *hdr)
{
    uint32_t blob_kb  = (uint32_t)(video_end - video_start) / 1024;
    /* raw pixel data: every field is a 240x320 half of a source frame */
    uint32_t raw_kb   = (uint32_t)hdr->fields * (480u / 2) * 320u * hdr->bpp / 1024u;
    uint32_t ratio10  = (raw_kb * 10 + blob_kb / 2) / blob_kb;   /* rounded */

    printf("\n");
    printf("  ============================================================\n");
    printf("   MSP3520 VIDEO player + SPI clock sweep (v7, DIRTY-RECT)\n");
    printf("   ESP32-S3 + ESP-IDF  |  sweep %d..%d MHz in %d MHz steps\n",
           FREQ_START_MHZ, FREQ_STOP_MHZ, FREQ_STEP_MHZ);
    printf("  ============================================================\n");
    printf("\n");
    printf("  Embedded video: %u source frames @ 15 fps -> %u fields\n",
           (unsigned)hdr->frames, (unsigned)hdr->fields);
    printf("  (one field = one 240-px half of a FRESH source frame),\n");
    printf("  stored in %u KiB (", (unsigned)blob_kb);
    print_x10(ratio10);
    printf("x smaller than the raw %u KiB of pixel data)\n", (unsigned)raw_kb);
    printf("\n");
    printf("  Wire format: %u bytes/pixel (COLMOD 0x%02X). S3 clock\n",
           (unsigned)hdr->bpp, (unsigned)(hdr->bpp == 3 ? 0x66 : 0x55));
    printf("  reality: 35-60 MHz requests run at 40 MHz, 65-80 MHz at\n");
    printf("  80 MHz (no other steps exist).\n");
    printf("  FIELD-INTERLACED streaming: 16-line DMA blocks into\n");
    printf("  alternating half-windows, one address window + RAMWR per\n");
    printf("  contiguous dirty run. No PSRAM needed. Field cadence is\n");
    printf("  ~21.7 fields/s at 40 MHz (vs ~10.85 full frames/s):\n");
    printf("  every field carries a newer source sample, so motion\n");
    printf("  advances at double cadence.\n");
    printf("  V7 DIRTY SCHEDULE: a field only carries the 16-line blocks\n");
    printf("  that changed vs the previous field of the same half — the\n");
    printf("  ILI9488 GRAM keeps the rest. Wire bytes follow motion;\n");
    printf("  static scenes idle the bus (status line shows bus %%).\n");
    printf("  V7 DITHER: pixels are Bayer 8x8 dithered offline before\n");
    printf("  the RGB666 quantizer — gradient banding becomes fine,\n");
    printf("  frame-stable grain.\n");
    printf("  Trade: a stationary mid-screen seam; fast pans shear across\n");
    printf("  it by ~1 field period. Static content: identical halves.\n");
    printf("  Noise, torn stripes, wrong colors or freezes\n");
    printf("  are NOT normal - press n.\n");
    printf("\n");
    printf("  Keys work DURING playback (single keypress, no Enter):\n");
    printf("    y  PASS   n  FAIL   r  restart   q  quit sweep\n");
    printf("    d  SCK/MOSI drive cap (80 MHz experiment)\n");
    printf("\n");
    printf("  Pin map (change in main/msp3520.h):\n");
    printf("    SCK->GPIO%d  MOSI->GPIO%d  MISO->GPIO%d  CS->GPIO%d\n",
           LCD_PIN_SCLK, LCD_PIN_MOSI, LCD_PIN_MISO, LCD_PIN_CS);
    printf("    DC->GPIO%d  RESET->GPIO%d  backlight(LED)->GPIO%d\n",
           LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_BL);
    printf("\n");
    printf("  Starting in 3 seconds ...\n");
    vTaskDelay(pdMS_TO_TICKS(3000));
}

void app_main(void)
{
    console_init();

    if (!video_open(video_start, (uint32_t)(video_end - video_start))) {
        printf("  FATAL: embedded video.bin failed to open (bad header?)\n");
        return;
    }
    if ((video_hdr()->flags & 0x4) == 0) {
        /* video_open also accepts progressive blobs (flags bit1) so the
         * decoder stays testable — but THIS player streams fields */
        printf("  FATAL: v7 streams FIELD-INTERLACED blobs (flags bit2).\n"
               "  Regenerate video.bin with:\n"
               "    tools/convert_video.py IN.mp4 main/video.bin \\\n"
               "        --scan field-halves --dither --dirty\n");
        return;
    }
    print_banner(video_hdr());

    esp_err_t err = msp3520_init();
    if (err != ESP_OK) {
        printf("  FATAL: display bring-up failed: %s\n", esp_err_to_name(err));
        printf("  Check the wiring, then press RESET and try again.\n");
        return;
    }
    printf("  Display initialised. ");
    {
        uint8_t raw_d3[6], raw_04[6];
        if (msp3520_read_id(raw_d3, raw_04)) {
            printf("Controller ID 0x9488 found (SPI read path OK).\n");
        } else {
            printf("Controller ID not found (MISO check only - carry on).\n");
        }
    }

    /* the wire pixel format comes from the blob: RGB666 (bpp 3) is the
     * only format the ILI9488 supports on its native SPI interface */
    ESP_ERROR_CHECK(msp3520_set_wire_bpp(video_hdr()->bpp));

    /* a solid screen proves the whole push path before video starts */
    ESP_ERROR_CHECK(msp3520_fill_screen(0x001F));   /* dark blue */
    printf("  Boot screen shown. Starting the sweep in 2 s ...\n");
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (int i = 0; i < STEP_COUNT; i++) {
        s_steps[i] = (step_t){ .mhz = FREQ_START_MHZ + i * FREQ_STEP_MHZ };
    }

    for (int i = 0; i < STEP_COUNT; i++) {
        if (!run_step(i)) {
            break;
        }
    }

    print_summary();
    retest_loop();
    cinema_loop();

    printf("\n  Test finished. The last played field stays on screen.\n");
    printf("  Nothing was permanently changed - press RESET to re-run.\n");
}

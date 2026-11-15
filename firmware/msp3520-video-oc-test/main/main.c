/*
 * main.c — MSP3520 progressive video player + SPI clock sweep.
 *
 * Plays the converted video (embedded video.bin, 480x320, 15 fps source)
 * as full progressive frames: the whole-display address window is set once
 * (msp3520_prepare_stream), then each frame is ONE RAMWR followed by pure
 * pixel data in 16-line DMA blocks — 41 SPI transactions per frame instead
 * of the 1280 the v3 per-line protocol needed (that transaction overhead
 * was worth ~12 ms/field — the reason v3 measured 8.5 fps at 40 MHz).
 * Default mode is progressive (no combing). 'i' toggles COLUMN-interlaced
 * mode: each frame goes out as two 240-px half-frames ~46 ms apart — the
 * SAME bytes per frame, so the fps ceiling does NOT change (interlacing
 * halves bytes per field, never bytes per picture); it doubles the update
 * rate to ~21.7 fields/s at 40 MHz. Cadence A/B experiment — see README.
 * Corruption (noise, stripes, wrong colors) IS a fault: press n.
 *
 * The sweep runs the SAME video at 10..80 MHz SPI clock, 5 MHz steps. At
 * each frequency the video loops for PLAY_SECONDS while you watch; press
 * y/n/r/q at any moment (single keypress):
 *    y = clean   n = artifacts   r = restart this frequency   q = stop
 * After the time runs out you get the same question once more.
 *
 * Frame rates to expect: with RGB666 a frame is 3,686,400 bits, so the
 * ideal frame time is bits/clock; 15 fps needs ~62 MHz — the 40 MHz bus
 * manages ~10.7 fps. NOTE the ESP32-S3 cannot generate every clock:
 * requests of 35-60 MHz actually run at 40 MHz, 65-80 MHz at 80 MHz (the
 * ILI9488 is 4x out of spec at 80 — expect snow there). The console shows
 * the measured fps at every frequency — a second opinion on the bus limit.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"

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

/* --- column-interlace mode ('i' key) ---------------------------------------
 * The decoder is forward-only, so an interlaced frame is decoded ONCE into
 * a full-frame stage and both half-fields are sliced out of it. The stage
 * lives in PSRAM and is read by the CPU only — DMA always reads the internal
 * s_block buffers, so no PSRAM-DMA/cache concerns. */
static bool     s_ilace;            /* toggled live with 'i'               */
static uint8_t *s_ilace_stage;      /* LCD_V_RES * 1440 B, alloc on demand */

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
        if (strchr(valid, ch) != NULL) {
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
        if (strchr("ynrqdi", ch) != NULL) {
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
    uint32_t fps_x10;     /* 0 = not measured */
} step_t;

static step_t s_steps[STEP_COUNT];

/* Push one full progressive frame (320 lines = 20 blocks of 16 lines).
 * Returns false only if the video decoder hard-fails (corrupt blob). */
static bool push_frame(void)
{
    const video_hdr_t *hdr = video_hdr();
    const size_t stride = (size_t)LCD_H_RES * hdr->bpp;  /* wire bytes/line */

    /* the window may currently be a HALF window (interlace mode was on) */
    ESP_ERROR_CHECK(msp3520_prepare_stream());
    ESP_ERROR_CHECK(msp3520_field_begin());  /* CS low + RAMWR: from row 0 */
    int live = 0;
    for (int b = 0; b < FRAME_BLOCKS; b++) {
        uint8_t *dst = s_block[b & (TX_BLOCKS_IN_FLIGHT - 1)];
        for (int l = 0; l < LCD_BLOCK_LINES; l++) {
            if (!video_read_line(dst + l * stride)) {
                /* end of the video inside a frame cannot happen (the blob
                 * ends on frame boundaries), but be safe and restart */
                video_rewind();
                if (!video_read_line(dst + l * stride)) {
                    return false;
                }
            }
        }
        ESP_ERROR_CHECK(msp3520_queue_block(dst));
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
    return true;
}

/* Push one frame as TWO column-interlaced half-fields (left x 0..239, then
 * right x 240..479), sent back-to-back ~46 ms apart at 40 MHz. Same wire
 * bytes as push_frame() — this is a cadence experiment, not a speedup.
 * Returns false only on a decoder hard-fail (corrupt blob). */
static bool push_ilace_frame(void)
{
    const video_hdr_t *hdr = video_hdr();
    const size_t stride   = (size_t)LCD_H_RES * hdr->bpp;   /* 1440 (bpp 3) */
    const size_t half_len = stride / 2;                     /* 720          */

    /* one-time full-frame stage (PSRAM first; CPU-only reads) */
    if (s_ilace_stage == NULL) {
        s_ilace_stage = heap_caps_malloc((size_t)LCD_V_RES * stride,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_ilace_stage == NULL) {
            s_ilace_stage = heap_caps_malloc((size_t)LCD_V_RES * stride,
                                             MALLOC_CAP_8BIT);
        }
        if (s_ilace_stage == NULL) {
            printf("\n  [interlace unavailable: no memory for the frame stage]\n");
            s_ilace = false;            /* fall back for the next frames */
            return true;
        }
    }

    /* decode the whole frame into the stage once (the decoder is
     * forward-only, so both halves are sliced out of this one copy) */
    uint8_t *p = s_ilace_stage;
    for (int l = 0; l < LCD_V_RES; l++) {
        if (!video_read_line(p)) {
            video_rewind();
            if (!video_read_line(p)) {
                return false;
            }
        }
        p += stride;
    }

    for (int half = 0; half < 2; half++) {
        ESP_ERROR_CHECK(msp3520_prepare_half_stream(half));
        ESP_ERROR_CHECK(msp3520_field_begin());
        int live = 0;
        for (int b = 0; b < FRAME_BLOCKS; b++) {
            const uint8_t *src = s_ilace_stage
                               + (size_t)b * LCD_BLOCK_LINES * stride
                               + (size_t)half * half_len;
            /* reuse a s_block row as the half-block staging buffer
             * (11,520 B needed, 23,040 B available) */
            uint8_t *dst = s_block[b & (TX_BLOCKS_IN_FLIGHT - 1)];
            for (int l = 0; l < LCD_BLOCK_LINES; l++) {
                memcpy(dst + (size_t)l * half_len,
                       src + (size_t)l * stride,
                       half_len);
            }
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
 * Frames are paced to 15 per second whenever the bus finishes early; when
 * the bus is slower than that, playback simply runs at whatever the bus
 * allows (measured fps drops below 15).
 * Returns a verdict key ('y','n','r','q') pressed during playback, or 0 if
 * the time simply ran out. Fills *fps_x10 and *frames. */
static char play_segment(int seconds, uint32_t *frames_out, uint32_t *fps_x10)
{
    const video_hdr_t *hdr = video_hdr();
    uint32_t frames = 0;
    uint32_t frame = 0;                 /* frame index 0..hdr->fields */
    int64_t t0 = esp_timer_get_time();
    int64_t deadline = t0 + (int64_t)seconds * 1000000;
    int64_t next_status = t0 + 500000;

    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            break;
        }

        if (s_ilace ? !push_ilace_frame() : !push_frame()) {
            break;
        }
        frames++;
        frame++;
        if (frame >= hdr->fields) {
            frame = 0;                  /* loop the video */
            video_rewind();
        }

        /* natural speed: 15 fps when the bus keeps up */
        pace_until(t0 + (int64_t)(frames + 1) * (1000000 / 15));

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
        if (k == 'i') {
            /* A/B experiment: column-interlaced half-frames. Same bytes per
             * frame (the fps ceiling cannot move), but the update rate
             * doubles to ~21.7 fields/s at 40 MHz. Takes effect next frame. */
            s_ilace = !s_ilace;
            printf("\n  [%s mode - takes effect next frame]\n",
                   s_ilace ? "COLUMN-INTERLACED" : "PROGRESSIVE");
            continue;
        }
        if (k != 0) {
            *frames_out = frames;
            int64_t el = esp_timer_get_time() - t0;
            *fps_x10 = el > 0 ? (uint32_t)((uint64_t)frames * 10 * 1000000 / el) : 0;
            printf("\n");
            return k;
        }

        now = esp_timer_get_time();
        if (now >= next_status) {
            int64_t el = now - t0;
            uint32_t f10 = el > 0
                ? (uint32_t)((uint64_t)frames * 10 * 1000000 / el) : 0;
            printf("\r  playing: %u frames, ", (unsigned)frames);
            print_x10(f10);
            printf(" fps");
            if (s_ilace) {
                printf(" (");
                print_x10(f10 * 2);    /* 2 half-frame updates per frame */
                printf(" upd/s)");
            }
            printf("   [y/n/r/q/d/i]");
            fflush(stdout);
            next_status += 500000;
        }
    }
    printf("\n");

    *frames_out = frames;
    int64_t el = esp_timer_get_time() - t0;
    *fps_x10 = el > 0 ? (uint32_t)((uint64_t)frames * 10 * 1000000 / el) : 0;
    return 0;
}

/* Run one sweep frequency. Returns false when the user pressed q. */
static bool run_step(int idx)
{
    step_t *st = &s_steps[idx];
    char key;
    uint32_t frames = 0;

    /* clean panel + fresh clock for every frequency */
    ESP_ERROR_CHECK(msp3520_reinit_panel());
    ESP_ERROR_CHECK(msp3520_set_clock(st->mhz * 1000000u));
    ESP_ERROR_CHECK(msp3520_prepare_stream());
    video_rewind();

    printf("\n  [%2d/%d] %2u MHz\n", idx + 1, STEP_COUNT, (unsigned)st->mhz);

    for (;;) {
        key = play_segment(PLAY_SECONDS, &frames, &st->fps_x10);
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
    printf(" fps, %u frames -> %s\n", (unsigned)frames,
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
    printf("   MHz      fps     verdict\n");
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
    uint32_t frames = 0, fps_x10 = 0;

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
    play_segment(3600, &frames, &fps_x10);
    printf("  Stopped after %u frames.\n", (unsigned)frames);
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
        uint32_t frames = 0, fps_x10 = 0;
        for (;;) {
            key = play_segment(PLAY_SECONDS, &frames, &fps_x10);
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
                s_steps[i].fps_x10 = fps_x10;
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
    uint32_t raw_kb   = (uint32_t)hdr->frames * (480u * 320u * hdr->bpp) / 1024u;
    uint32_t ratio10  = (raw_kb * 10 + blob_kb / 2) / blob_kb;   /* rounded */

    printf("\n");
    printf("  ============================================================\n");
    printf("   MSP3520 VIDEO player + SPI clock sweep (v5)\n");
    printf("   ESP32-S3 + ESP-IDF  |  sweep %d..%d MHz in %d MHz steps\n",
           FREQ_START_MHZ, FREQ_STOP_MHZ, FREQ_STEP_MHZ);
    printf("  ============================================================\n");
    printf("\n");
    printf("  Embedded video: %u frames @ 15 fps (%u.%u s), %u fields,\n",
           (unsigned)hdr->frames, (unsigned)(hdr->frames * 10 / 15 / 10),
           (unsigned)(hdr->frames * 10 / 15 % 10), (unsigned)hdr->fields);
    printf("  stored in %u KiB (", (unsigned)blob_kb);
    print_x10(ratio10);
    printf("x smaller than the raw %u KiB of pixel data)\n", (unsigned)raw_kb);
    printf("\n");
    printf("  Wire format: %u bytes/pixel (COLMOD 0x%02X). S3 clock\n",
           (unsigned)hdr->bpp, (unsigned)(hdr->bpp == 3 ? 0x66 : 0x55));
    printf("  reality: 35-60 MHz requests run at 40 MHz, 65-80 MHz at\n");
    printf("  80 MHz (no other steps exist).\n");
    printf("  PROGRESSIVE scan, 16-line DMA blocks: ~41 SPI transactions\n");
    printf("  per frame (v3 needed 1280 - that overhead cost it ~2 fps).\n");
    printf("  No combing in progressive mode. Press i to A/B the\n");
    printf("  column-interlace cadence (21.7 upd/s at 40 MHz; same fps\n");
    printf("  ceiling - the wire still carries every byte of every\n");
    printf("  frame, only the update cadence changes).\n");
    printf("  Noise, torn stripes, wrong colors or freezes\n");
    printf("  are NOT normal - press n.\n");
    printf("\n");
    printf("  Keys work DURING playback (single keypress, no Enter):\n");
    printf("    y  PASS   n  FAIL   r  restart   q  quit sweep\n");
    printf("    d  SCK/MOSI drive cap (80 MHz experiment)\n");
    printf("    i  column-interlace on/off (cadence experiment)\n");
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

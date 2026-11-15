/*
 * msp3520.c — ESP-IDF driver for the MSP3520 (ILI9488) display, streaming
 * edition. Pins, panel init sequence and reset behaviour are identical to
 * the msp3520-oc-test project; what is new here is how data is pushed:
 *
 *  - D/C is set by lcd_pre_cb (the SPI driver invokes it right before each
 *    transaction, polled or queued). t->user == 1 means "data phase" (D/C
 *    high), NULL means "command byte" (D/C low).
 *  - msp3520_queue_block() queues ONE 16-line DMA transaction (23,040 B
 *    with RGB666) and returns immediately; DMA pushes it while the CPU
 *    decodes the next block.
 *
 * A frame is pushed with CS held LOW the whole time: one RAMWR (polled,
 * inside msp3520_field_begin) followed by 20 pure-data block transactions.
 * The panel sees one continuous burst of pixels — no per-line addressing
 * (the v1-v3 per-line protocol needed 4 transactions per line = 1280 per
 * frame; that overhead cost ~12 ms/field at 40 MHz).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "msp3520.h"

#define LCD_SAFE_CLOCK_HZ   1000000
#define LCD_SPI_HOST        SPI2_HOST

/* Transaction ring: one queued block uses 1 slot; with
 * TX_BLOCKS_IN_FLIGHT == 2 at most 2 slots are live at any time. */
#define TX_RING_SLOTS 16

static spi_device_handle_t s_spi;
static uint32_t            s_clock_hz;
static int                 s_wire_bpp = LCD_BPP;  /* 2 (565) or 3 (666)   */

static spi_transaction_t s_tx[TX_RING_SLOTS];
static int               s_tx_head;
static int               s_tx_live;   /* queued but not yet retired */

/* ================= low-level pin helpers ================= */

static inline void lcd_cs_low(void)    { gpio_set_level(LCD_PIN_CS, 0); }
static inline void lcd_cs_high(void)   { gpio_set_level(LCD_PIN_CS, 1); }

/* Called by the SPI driver immediately before every transaction (polled or
 * queued). For queued transactions it runs in interrupt context, which is
 * exactly why D/C must be handled here and not at queue time. */
static void IRAM_ATTR lcd_pre_cb(spi_transaction_t *t)
{
    gpio_set_level(LCD_PIN_DC, (int)(intptr_t)t->user);
}

/* ================= transaction helpers ================= */

static uint64_t s_wire_bytes;   /* total bytes pushed to the wire */

/* Submit one transaction (blocking if the queue were full — with our
 * accounting that never happens). `data_phase` selects the D/C level. */
static esp_err_t lcd_queue(const uint8_t *data, size_t len, bool data_phase)
{
    if (s_tx_live >= TX_RING_SLOTS) {
        return ESP_ERR_INVALID_STATE;          /* caller accounting bug */
    }
    spi_transaction_t *t = &s_tx[s_tx_head];
    memset(t, 0, sizeof(*t));
    t->user   = data_phase ? (void *)1 : (void *)0;
    t->length = len * 8;
    if (len <= 4 && data != NULL) {
        t->flags = SPI_TRANS_USE_TXDATA;       /* small sends: no DMA needed */
        memcpy(t->tx_data, data, len);
    } else {
        t->tx_buffer = data;                   /* must stay valid until retire */
    }
    esp_err_t err = spi_device_queue_trans(s_spi, t, portMAX_DELAY);
    if (err == ESP_OK) {
        s_tx_head = (s_tx_head + 1) % TX_RING_SLOTS;
        s_tx_live++;
        s_wire_bytes += len;
    }
    return err;
}

/* Wait for the oldest queued transaction to complete. */
static esp_err_t lcd_retire(void)
{
    spi_transaction_t *done;
    esp_err_t err = spi_device_get_trans_result(s_spi, &done, portMAX_DELAY);
    if (err == ESP_OK) {
        s_tx_live--;
    }
    return err;
}

/* One blocking (polled) SPI write. */
static esp_err_t lcd_poll(const uint8_t *data, size_t len, bool data_phase)
{
    spi_transaction_t t = {
        .user   = data_phase ? (void *)1 : (void *)0,
        .length = len * 8,
    };
    if (len <= 4 && data != NULL) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, data, len);
    } else {
        t.tx_buffer = data;
    }
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err == ESP_OK) {
        s_wire_bytes += len;
    }
    return err;
}

/* Command byte + optional parameters inside one CS-low window. */
static esp_err_t lcd_cmd(uint8_t cmd, const uint8_t *params, size_t n)
{
    esp_err_t err;

    lcd_cs_low();
    err = lcd_poll(&cmd, 1, false);
    if (err == ESP_OK && n > 0) {
        err = lcd_poll(params, n, true);
    }
    lcd_cs_high();
    return err;
}

/* ================= bring-up / panel init ================= */

esp_err_t msp3520_init(void)
{
    /* Idle levels first, then switch pins to outputs (glitch-free). */
    gpio_set_level(LCD_PIN_CS, 1);
    gpio_set_level(LCD_PIN_DC, 0);
    gpio_set_level(LCD_PIN_RST, 1);
    gpio_set_level(LCD_PIN_BL, 1);

    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << LCD_PIN_CS) | (1ULL << LCD_PIN_DC) |
                        (1ULL << LCD_PIN_RST) | (1ULL << LCD_PIN_BL),
        .mode         = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    const spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_PIN_SCLK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = LCD_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_MAX_TRANSFER,
    };
    err = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    err = msp3520_set_clock(LCD_SAFE_CLOCK_HZ);
    if (err != ESP_OK) {
        return err;
    }
    return msp3520_reinit_panel();
}

esp_err_t msp3520_set_clock(uint32_t hz)
{
    esp_err_t err;

    if (s_spi != NULL) {
        err = spi_bus_remove_device(s_spi);   /* queue must be empty here */
        if (err != ESP_OK) {
            return err;
        }
        s_spi = NULL;
    }

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = (int)hz,
        .mode           = 0,
        .spics_io_num   = -1,       /* CS is a plain GPIO under our control */
        .queue_size     = TX_RING_SLOTS,
        .pre_cb         = lcd_pre_cb,
    };
    err = spi_bus_add_device(LCD_SPI_HOST, &devcfg, &s_spi);
    if (err == ESP_OK) {
        s_clock_hz = hz;
        s_tx_head  = 0;
        s_tx_live  = 0;
    }
    return err;
}

uint32_t msp3520_get_clock(void)
{
    return s_clock_hz;
}

void msp3520_backlight(bool on)
{
    gpio_set_level(LCD_PIN_BL, on ? 1 : 0);
}

void msp3520_set_edge_drive(int cap)
{
    /* Live experiment knob for the 80 MHz stability question: set the GPIO
     * drive capability of SCK/MOSI. 0..3 = weakest..strongest edges
     * (gpio_drive_cap_t). The SPI peripheral keeps running; only the pad
     * drive strength changes, so it can be flipped mid-playback with the
     * 'd' key. Weaker settings (0/1) slow the edges and can reduce
     * ringing/reflections on jumper wires — try each while watching for
     * snow at the 65-80 MHz steps. */
    if (cap < 0 || cap > 3) {
        return;
    }
    gpio_set_drive_capability(LCD_PIN_SCLK, (gpio_drive_cap_t)cap);
    gpio_set_drive_capability(LCD_PIN_MOSI, (gpio_drive_cap_t)cap);
}

esp_err_t msp3520_reinit_panel(void)
{
    uint32_t restore_hz = s_clock_hz;
    esp_err_t err = msp3520_set_clock(LCD_SAFE_CLOCK_HZ);
    if (err != ESP_OK) {
        return err;
    }

    /* Hardware reset. */
    lcd_cs_low();
    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cs_high();

    /* ILI9488 power-on sequence (same register table as TFT_eSPI). */
    static const struct {
        uint8_t cmd;
        uint8_t params[15];
        uint8_t n;
        uint16_t delay_ms;
    } init_seq[] = {
        { 0xE0, {0x00,0x03,0x09,0x08,0x16,0x0A,0x3F,0x78,0x4C,0x09,0x0A,0x08,0x16,0x1A,0x0F}, 15, 0 },
        { 0xE1, {0x00,0x16,0x19,0x03,0x0F,0x05,0x32,0x45,0x46,0x04,0x0E,0x0D,0x35,0x37,0x0F}, 15, 0 },
        { 0xC0, {0x17,0x15}, 2, 0 },
        { 0xC1, {0x41},       1, 0 },
        { 0xC5, {0x00,0x12,0x80}, 3, 0 },
        { 0x36, {LCD_MADCTL}, 1, 0 },                 /* MADCTL: landscape  */
        { 0x3A, {0x66},       1, 0 },                 /* COLMOD: 18-bit RGB666 (SPI-only mode) */
        { 0xB0, {0x00},       1, 0 },
        { 0xB1, {0xA0},       1, 0 },
        { 0xB4, {0x02},       1, 0 },
        { 0xB6, {0x02,0x02,0x3B}, 3, 0 },
        { 0xB7, {0xC6},       1, 0 },
        { 0xF7, {0xA9,0x51,0x2C,0x82}, 4, 0 },
        { 0x11, {0},          0, 120 },               /* sleep out          */
        { 0x29, {0},          0, 25  },               /* display on         */
    };

    for (size_t i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
        err = lcd_cmd(init_seq[i].cmd, init_seq[i].params, init_seq[i].n);
        if (err != ESP_OK) {
            return err;
        }
        if (init_seq[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(init_seq[i].delay_ms));
        }
    }

#if LCD_INVERT_COLORS
    err = lcd_cmd(0x21, NULL, 0);                     /* INVON */
    if (err != ESP_OK) {
        return err;
    }
#endif

    /* Re-assert the selected wire pixel format after the reset (the init
     * table writes 0x66 — the only format for a native-SPI ILI9488). */
    {
        uint8_t colmod = (s_wire_bpp == 3) ? 0x66 : 0x55;
        err = lcd_cmd(0x3A, &colmod, 1);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (restore_hz != LCD_SAFE_CLOCK_HZ) {
        return msp3520_set_clock(restore_hz);
    }
    return ESP_OK;
}

/* ================= ID read (wiring sanity check) ================= */

static esp_err_t lcd_read_reg(uint8_t cmd, uint8_t *out, size_t n)
{
    static const uint8_t dummy[8] = {0};
    esp_err_t err;

    lcd_cs_low();
    err = lcd_poll(&cmd, 1, false);
    if (err == ESP_OK) {
        spi_transaction_t t = {
            .length    = n * 8,
            .rxlength  = n * 8,             /* full-duplex read */
            .user      = (void *)1,
            .tx_buffer = dummy,
            .rx_buffer = out,
        };
        err = spi_device_polling_transmit(s_spi, &t);
    }
    lcd_cs_high();
    return err;
}

static bool bits_contain(const uint8_t *buf, size_t n, uint16_t pattern)
{
    uint32_t window = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        for (int b = 7; b >= 0; b--) {
            window = (window << 1) | ((buf[i] >> b) & 1);
            if (++bits >= 16 && (window & 0xFFFF) == pattern) {
                return true;
            }
        }
    }
    return false;
}

bool msp3520_read_id(uint8_t raw_d3[6], uint8_t raw_04[6])
{
    bool ok = false;
    uint32_t restore_hz = s_clock_hz;
    if (restore_hz != LCD_SAFE_CLOCK_HZ) {
        msp3520_set_clock(LCD_SAFE_CLOCK_HZ);
    }

    memset(raw_d3, 0, 6);
    memset(raw_04, 0, 6);
    if (lcd_read_reg(0xD3, raw_d3, 6) == ESP_OK) {
        ok = ok || bits_contain(raw_d3, 6, 0x9488);
    }
    if (lcd_read_reg(0x04, raw_04, 6) == ESP_OK) {
        ok = ok || bits_contain(raw_04, 6, 0x9488);
    }

    if (restore_hz != LCD_SAFE_CLOCK_HZ) {
        msp3520_set_clock(restore_hz);
    }
    return ok;
}

/* ================= wire pixel format ================= */

esp_err_t msp3520_set_wire_bpp(int bpp)
{
    uint8_t colmod;
    if (bpp != 2 && bpp != 3) {
        return ESP_ERR_INVALID_ARG;
    }
    colmod = (bpp == 3) ? 0x66 : 0x55;
    esp_err_t err = lcd_cmd(0x3A, &colmod, 1);
    if (err == ESP_OK) {
        s_wire_bpp = bpp;
    }
    return err;
}

int msp3520_get_wire_bpp(void)
{
    return s_wire_bpp;
}

/* ================= streaming + fill ================= */

esp_err_t msp3520_prepare_stream(void)
{
    /* Full-display window (CASET + PASET). The controller wraps rows inside
     * the window, so the whole frame can then be streamed as pure pixel
     * data — no per-line addressing on the wire. */
    const uint8_t caset[4] = { 0, 0, (LCD_H_RES - 1) >> 8, (LCD_H_RES - 1) & 0xFF };
    const uint8_t paset[4] = { 0, 0, (LCD_V_RES - 1) >> 8, (LCD_V_RES - 1) & 0xFF };
    esp_err_t err = lcd_cmd(0x2A, caset, 4);
    if (err != ESP_OK) {
        return err;
    }
    return lcd_cmd(0x2B, paset, 4);
}

esp_err_t msp3520_prepare_half_stream(int half)
{
    /* Half-width window for column-interlaced fields (v5/v6): the
     * full-height special case (lines 0..319) of msp3520_prepare_half_run. */
    return msp3520_prepare_half_run(half, 0, LCD_V_RES - 1);
}

esp_err_t msp3520_prepare_half_run(int half, int y0, int y1)
{
    /* v7 dirty runs: a 240-px half window covering ONLY lines y0..y1
     * (inclusive). MV=1 (LCD_MADCTL) makes CASET span the 480-px display
     * axis, so a "half" is a 240-px column band; PASET narrows it to the
     * run's line range. Every RAMWR after this starts at the window
     * origin (datasheet §5.2.24: the column and page registers reset to
     * SC/SP), and rows wrap inside the window exactly like the full-window
     * case (§6.3) — so a run of 16-line blocks streams as one RAMWR
     * session of back-to-back DMA transactions. */
    static const uint8_t caset[2][4] = {
        { 0, 0,               0, (LCD_H_RES / 2) - 1 },               /* x 0..239   */
        { 0, LCD_H_RES / 2,   (LCD_H_RES - 1) >> 8, (LCD_H_RES - 1) & 0xFF }, /* 240..479 */
    };
    const uint8_t paset[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                               (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    if ((half != 0 && half != 1) || y0 < 0 || y1 < y0 || y1 >= LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lcd_cmd(0x2A, caset[half], 4);
    if (err != ESP_OK) {
        return err;
    }
    return lcd_cmd(0x2B, paset, 4);
}

esp_err_t msp3520_field_begin(void)
{
    /* CS low + RAMWR: opens a frame write. The address pointer resets to
     * the window start (row 0) and keeps advancing while CS stays low. */
    const uint8_t ramwr = 0x2C;
    lcd_cs_low();
    return lcd_poll(&ramwr, 1, false);
}

esp_err_t msp3520_queue_block(const uint8_t *block_bytes)
{
    /* pure pixel data: the window was set by msp3520_prepare_stream() and
     * the write session opened by msp3520_field_begin() (RAMWR). The
     * controller wraps rows inside the window and keeps consuming bytes
     * while CS stays low, so blocks are just back-to-back DMA transactions
     * — 1 RAMWR + 20 blocks per frame instead of 4 transactions per line. */
    return lcd_queue(block_bytes,
                     (size_t)LCD_BLOCK_LINES * LCD_H_RES * (size_t)s_wire_bpp,
                     true);
}

esp_err_t msp3520_queue_half_block(const uint8_t *block_bytes)
{
    /* like msp3520_queue_block, but for a 240-px half window: 16 lines of
     * LCD_H_RES/2 pixels. The half window wraps rows exactly like the full
     * window, so blocks are back-to-back data transactions again. */
    return lcd_queue(block_bytes,
                     (size_t)LCD_BLOCK_LINES * (LCD_H_RES / 2) * (size_t)s_wire_bpp,
                     true);
}

esp_err_t msp3520_retire_block(void)
{
    return lcd_retire();
}

void msp3520_field_end(void)
{
    lcd_cs_high();
}

esp_err_t msp3520_fill_screen(uint16_t color565)
{
    static uint8_t block[LCD_BLOCK_BYTES];
    const size_t stride = (size_t)LCD_H_RES * (size_t)s_wire_bpp;

    /* build one line, then replicate it into a 16-line block */
    if (s_wire_bpp == 3) {
        uint8_t rgb[3];
        lcd_rgb565_to_666(color565, rgb);
        for (int x = 0; x < LCD_H_RES; x++) {
            block[x * 3 + 0] = rgb[0];
            block[x * 3 + 1] = rgb[1];
            block[x * 3 + 2] = rgb[2];
        }
    } else {
        uint8_t px[2];
        lcd_rgb565_to_be16(color565, px);
        for (int x = 0; x < LCD_H_RES; x++) {
            block[x * 2 + 0] = px[0];
            block[x * 2 + 1] = px[1];
        }
    }
    for (int l = 1; l < LCD_BLOCK_LINES; l++) {
        memcpy(block + l * stride, block, stride);
    }

    esp_err_t err = msp3520_prepare_stream();
    if (err != ESP_OK) {
        return err;
    }
    err = msp3520_field_begin();
    if (err != ESP_OK) {
        return err;
    }
    for (int b = 0; b < LCD_V_RES / LCD_BLOCK_LINES; b++) {
        err = msp3520_queue_block(block);
        if (err != ESP_OK) return err;
        err = msp3520_retire_block();    /* boot-time fill: no pipelining */
        if (err != ESP_OK) return err;
    }
    msp3520_field_end();
    return ESP_OK;
}

/* ================= lab: queued window sessions =========================== */

/* The row/column-window techniques (interlacing, quincunx, spans, ...) need
 * a different address window for almost every burst, so the fixed-window
 * streaming API above does not fit. These calls queue CASET/PASET/RAMWR
 * COMMANDS and pixel data as ordinary DMA transactions while CS stays LOW
 * for the whole field — the D/C line is switched per transaction by
 * lcd_pre_cb, so commands and data interleave cleanly and the wire never
 * idles between bursts. Wire cost of one window is 11 bytes (CASET+PASET+
 * RAMWR headers), ~0.8% on a 1440-byte row. */

esp_err_t msp3520_session_begin(void)
{
    lcd_cs_low();
    return ESP_OK;
}

esp_err_t msp3520_qcaset(uint16_t x0, uint16_t x1)
{
    const uint8_t cmd = 0x2A;
    const uint8_t params[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0,
                                (uint8_t)(x1 >> 8), (uint8_t)x1 };
    esp_err_t err = lcd_queue(&cmd, 1, false);
    if (err == ESP_OK) {
        err = lcd_queue(params, 4, true);
    }
    return err;
}

esp_err_t msp3520_qpaset(uint16_t y0, uint16_t y1)
{
    const uint8_t cmd = 0x2B;
    const uint8_t params[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0,
                                (uint8_t)(y1 >> 8), (uint8_t)y1 };
    esp_err_t err = lcd_queue(&cmd, 1, false);
    if (err == ESP_OK) {
        err = lcd_queue(params, 4, true);
    }
    return err;
}

esp_err_t msp3520_qramwr(void)
{
    const uint8_t cmd = 0x2C;
    return lcd_queue(&cmd, 1, false);
}

esp_err_t msp3520_qdata(const uint8_t *data, size_t len)
{
    return lcd_queue(data, len, true);
}

/* Retire queued transactions until at most `keep` are live, so the buffers
 * of everything older are reusable. */
esp_err_t msp3520_drain_to(int keep)
{
    while (s_tx_live > keep) {
        esp_err_t err = lcd_retire();
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* Retire everything, then raise CS (closes the session). */
void msp3520_session_end(void)
{
    while (s_tx_live > 0) {
        (void)lcd_retire();
    }
    lcd_cs_high();
}

/* Total bytes handed to the SPI driver (commands + pixel data) since the
 * last reset — the lab's live bus-load statistic. */
uint64_t msp3520_wire_bytes(void)
{
    return s_wire_bytes;
}

void msp3520_wire_bytes_reset(void)
{
    s_wire_bytes = 0;
}

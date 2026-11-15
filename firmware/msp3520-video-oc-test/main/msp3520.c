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
    return spi_device_polling_transmit(s_spi, &t);
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

/*
 * msp3520-clock-probe — which SPI clocks does the ESP32-S3 GPSPI actually
 * deliver, and does a "1-bit I80" LCD_CAM mode exist?
 *
 * BACKGROUND (the claim under test): a web-sourced suggestion said the
 * LCD_CAM peripheral in "1-bit I80 mode" could drive this SPI-wired
 * MSP3520 panel at 53.33 MHz (PLL_F160M/3) or 60 MHz (PLL_D2/4) without
 * moving a wire, because LCD_CAM routes through the GPIO matrix. The
 * IDF v6.1 source says the 1-bit mode does not exist (esp_lcd i80 is
 * 8/16-bit parallel only) and GPSPI cannot reach the 160/240 MHz PLLs.
 * This probe demonstrates both on real silicon and answers the practical
 * question: which clocks exist between 40 and 80 MHz?
 *
 * What it does (safe with the display wired and powered — CS is held
 * high, so the ILI9488 ignores every SCK/MOSI wiggle this makes):
 *   1. Holds the panel control lines inactive (CS/DC/RST high, BL on).
 *   2. Sweeps requested SPI clocks. For each: spi_bus_add_device(),
 *      dump the GPSPI2 SPI_CLOCK register the driver programmed
 *      (clk_equ_sysclk / clkdiv_pre / clkcnt_n / clkcnt_h / clkcnt_l),
 *      compute the clock those fields imply, and MEASURE the real
 *      clock by timing dummy 16 KiB polling transfers (bits / second).
 *   3. Calls esp_lcd_new_i80_bus() with bus_width=1 and 2, printing
 *      what the "1-bit I80 mode" actually returns.
 *
 * Register semantics (IDF v6.1, all verified in source):
 *   - soc/spi_struct.h: "spi_clk frequency is system /
 *     (spi_clkdiv_pre+1)/(spi_clkcnt_N+1)"  — integer divisors only
 *   - esp_hal_gpspi/esp32s3/include/hal/spi_ll.h: spi_ll_freq_for_pre_n()
 *     = fapb/(pre*n); clkcnt_h/clkcnt_l set the DUTY CYCLE, never the
 *     frequency; requests above 3/4 of the source take clk_equ_sysclk=1
 *   - GPSPI clock sources (soc/clk_tree_defs.h): APB (80 MHz) and XTAL
 *     only — the 160/240 MHz LCD_CAM PLLs are not SPI clock sources
 *   - components/esp_lcd/i80/esp_lcd_panel_io_i80.c:129 rejects any
 *     bus_width other than 8 or 16
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_private/esp_clk.h"     /* esp_clk_apb_freq() */
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"          /* esp_lcd_i80_bus_config_t */
#include "soc/spi_struct.h"          /* GPSPI2 */

/* same wiring as the msp3520 projects (MSP3520 -> ESP32-S3-DevKitC-1) */
#define PIN_SCLK  12
#define PIN_MOSI  11
#define PIN_CS    10
#define PIN_DC     9
#define PIN_RST   14
#define PIN_BL    47

#define BUF_BYTES 16384
#define PASSES    10

static uint8_t *s_buf;

static void hold_panel_inactive(void)
{
    /* CS/DC/RST high = the ILI9488 ignores the bus; backlight on */
    const int pins[] = { PIN_CS, PIN_DC, PIN_RST, PIN_BL };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_set_direction((gpio_num_t)pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)pins[i], 1);
    }
}

/* Push PASSES x 16 KiB through the SPI and time it — see probe_measured(). */

/* Phase 1 — the driver's own answer, no SPI transactions: add a device
 * at the requested clock and ask the driver what it actually configured
 * (spi_device_get_actual_freq -> spi_ll_master_cal_clock). Runs anywhere,
 * including QEMU. */
static void probe_registers(uint32_t req_hz)
{
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = (int)req_hz,
        .mode = 0,
        .spics_io_num = -1,          /* CS is a GPIO held high above */
        .queue_size = 4,
    };
    spi_device_handle_t spi = NULL;
    esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev, &spi);
    if (err != ESP_OK) {
        printf("  %6.2f        add_device FAILED: %s\n",
               req_hz / 1e6, esp_err_to_name(err));
        return;
    }

    int khz = 0;
    err = spi_device_get_actual_freq(spi, &khz);
    if (err != ESP_OK) {
        printf("  %6.2f        get_actual_freq FAILED: %s\n",
               req_hz / 1e6, esp_err_to_name(err));
    } else {
        printf("  %6.2f                %8.3f\n",
               req_hz / 1e6, khz / 1000.0);
    }

    spi_bus_remove_device(spi);
}

/* Phase 2 — hardware timing: one tiny transfer first (the clock register
 * is applied at transaction time, not at add_device time), then dump the
 * live register and push PASSES x 16 KiB to time the real bus clock.
 * The panel ignores everything (CS high). */
static void probe_measured(uint32_t req_hz, uint32_t src_hz)
{
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = (int)req_hz,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 4,
    };
    spi_device_handle_t spi = NULL;
    esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev, &spi);
    if (err != ESP_OK) {
        return;
    }

    /* apply the device clock (register written at transaction start) */
    uint8_t one = 0xA5;
    spi_transaction_t t1 = { .length = 8, .tx_buffer = &one };
    err = spi_device_polling_transmit(spi, &t1);
    if (err != ESP_OK) {
        printf("  %6.2f MHz   polling transmit failed: %s\n",
               req_hz / 1e6, esp_err_to_name(err));
        spi_bus_remove_device(spi);
        return;
    }

    uint32_t equ = GPSPI2.clock.clk_equ_sysclk;
    uint32_t pre = GPSPI2.clock.clkdiv_pre + 1;
    uint32_t n   = GPSPI2.clock.clkcnt_n + 1;
    uint32_t h   = GPSPI2.clock.clkcnt_h + 1;
    uint32_t l   = GPSPI2.clock.clkcnt_l + 1;
    double computed = equ ? (double)src_hz
                          : (double)src_hz / ((double)pre * n);

    spi_transaction_t t = {
        .length = BUF_BYTES * 8,
        .tx_buffer = s_buf,
    };
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < PASSES; i++) {
        err = spi_device_polling_transmit(spi, &t);
        if (err != ESP_OK) {
            printf("  %6.2f MHz   polling transmit failed: %s\n",
                   req_hz / 1e6, esp_err_to_name(err));
            spi_bus_remove_device(spi);
            return;
        }
    }
    int64_t el = esp_timer_get_time() - t0;
    double measured = el > 0
        ? (double)PASSES * BUF_BYTES * 8 / ((double)el / 1e6) : 0;

    printf("  %6.2f      %u %2u %2u %2u %2u   %7.2f   %7.2f\n",
           req_hz / 1e6, (unsigned)equ, (unsigned)pre, (unsigned)n,
           (unsigned)h, (unsigned)l,
           computed / 1e6, measured / 1e6);

    spi_bus_remove_device(spi);
}

static void probe_i80_bus_width(int width)
{
    const esp_lcd_i80_bus_config_t bus = {
        .dc_gpio_num = PIN_DC,
        .wr_gpio_num = PIN_SCLK,
        .clk_src = LCD_CLK_SRC_PLL160M,
        .data_gpio_nums = { PIN_MOSI },   /* only D0, as the claim suggests */
        .bus_width = width,
        .max_transfer_bytes = 4,
    };
    esp_lcd_i80_bus_handle_t handle = NULL;
    esp_err_t err = esp_lcd_new_i80_bus(&bus, &handle);
    printf("  esp_lcd_new_i80_bus(bus_width=%d) -> %s\n",
           width, esp_err_to_name(err));
    if (err == ESP_OK) {
        esp_lcd_del_i80_bus(handle);  /* unreachable on S3: 8/16 only */
    }
}

void app_main(void)
{
    printf("\n=== MSP3520 SPI clock probe (ESP32-S3, IDF v6.1) ===\n");
    printf("claim under test: '1-bit I80 LCD_CAM mode' at 53.3/60 MHz\n\n");

    hold_panel_inactive();

    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,     /* native FSPI IOMUX pins = full speed */
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BUF_BYTES,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        printf("FATAL: spi_bus_initialize: %s\n", esp_err_to_name(err));
        return;
    }
    s_buf = heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA);
    if (s_buf == NULL) {
        printf("FATAL: no DMA buffer\n");
        return;
    }
    memset(s_buf, 0xA5, BUF_BYTES);

    uint32_t src_hz = esp_clk_apb_freq();
    printf("GPSPI source (APB): %u Hz\n", (unsigned)src_hz);
    printf("Register model: SCK = src/(pre*n), h/l = duty only.\n\n");

    printf("--- phase 1: the driver's own answer (no transactions) ---\n");
    printf("  request             actual (driver)\n");
    printf("   (MHz)                  (MHz)\n");
    printf("  ----------------------------------\n");

    const uint32_t sweep[] = {
        10000000, 15000000, 20000000, 25000000, 30000000, 35000000,
        40000000, 45000000, 50000000, 53333333, 55000000, 60000000,
        60001000, 64000000, 65000000, 70000000, 75000000, 80000000,
    };
    for (size_t i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
        probe_registers(sweep[i]);
    }

    printf("\n--- the '1-bit I80 mode' claim ---\n");
    probe_i80_bus_width(1);
    probe_i80_bus_width(2);

    printf("\n--- phase 2: live register + measured throughput ---\n");
    printf("            (after a transaction applies the clock)\n");
    printf("  request   equ pre  n  h  l    computed   measured\n");
    printf("   (MHz)                                     (MHz)      (MHz)\n");
    printf("  ----------------------------------------------------\n");
    for (size_t i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
        probe_measured(sweep[i], src_hz);
    }

    printf("\n=== VERDICT (matches the IDF v6.1 source) ===\n");
    printf("1. GPSPI lattice = 80/(pre*n): 80, 40, 26.67, 20, 16...\n");
    printf("   NOTHING exists strictly between 40 and 80 MHz; requests\n");
    printf("   above 3/4 of the source (i.e. > 60 MHz) go straight to 80.\n");
    printf("2. clkcnt_h / clkcnt_l shape duty cycle only, never the\n");
    printf("   frequency - so even direct register writes cannot create\n");
    printf("   53.3 or 60 MHz on the GPSPI.\n");
    printf("3. esp_lcd_new_i80_bus() accepts only 8 or 16 data lines\n");
    printf("   (esp_lcd/i80/esp_lcd_panel_io_i80.c:129). No 1-bit serial\n");
    printf("   mode exists, and the 160/240 MHz LCD_CAM PLLs are not\n");
    printf("   GPSPI clock sources. On this wiring, 40 and 80 are it.\n");
    printf("\nprobe complete - safe to power off. Flash a msp3520 video\n");
    printf("firmware again to get the display back.\n");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

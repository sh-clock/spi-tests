# MSP3520 SPI Clock Probe — ESP32-S3 + ESP-IDF v6.1

A one-shot diagnostic that answers two questions on real silicon (safe
with the display wired — it holds CS/DC/RST high so the ILI9488 ignores
everything):

1. **Which SPI clocks can the ESP32-S3 GPSPI actually produce?**
   Phase 1 asks the driver itself (`spi_device_get_actual_freq`) for a
   sweep of requested clocks; phase 2 applies each clock with a 1-byte
   transfer, dumps the live `SPI2` clock register
   (`clk_equ_sysclk / clkdiv_pre / clkcnt_n / clkcnt_h / clkcnt_l`) and
   **measures** the real bus clock by timing dummy 16 KiB transfers.

2. **Does a "1-bit I80 LCD_CAM mode" exist?**
   The probe calls `esp_lcd_new_i80_bus(bus_width=1)` (and 2) and prints
   what comes back: `ESP_ERR_INVALID_ARG` ("invalid bus width") — the
   LCD_CAM I80 interface is 8/16-bit parallel only, so it cannot emulate
   SPI on 2 wires.

## Expected output (phase 1, verified in QEMU)

```
  request             actual (driver)
   (MHz)                  (MHz)
   10.00                  10.000
   15.00                  16.000
   20.00                  20.000
   25.00                  26.666
   30.00                  26.666
   35.00                  40.000
   40.00                  40.000
   45.00                  40.000
   50.00                  40.000
   53.33                  40.000     <- 160/3 is NOT reachable
   55.00                  40.000
   60.00                  40.000     <- requests <= 60 use the divider
   60.00                  80.000     <- 60.001 MHz: the 3/4-source cliff
   64.00                  80.000
   80.00                  80.000
```

The register model (IDF v6.1 source): `SCK = source / ((clkdiv_pre+1) *
(clkcnt_n+1))` — integer divisors of the 80 MHz APB clock only
(`components/soc/esp32s3/register/soc/spi_struct.h:58`,
`components/esp_hal_gpspi/esp32s3/include/hal/spi_ll.h`). `clkcnt_h` /
`clkcnt_l` shape the **duty cycle**, never the frequency. Requests above
3/4 of the source (> 60 MHz) bypass the divider (`clk_equ_sysclk=1`) and
run at exactly 80 MHz. So the GPSPI lattice is {80, 40, 26.67, 20, 16,
…} — **nothing exists strictly between 40 and 80**, not through the
driver and not even with direct register writes.

The LCD_CAM peripheral does have 160/240 MHz PLL sources and a ÷1..64
pclk divider (53.33 = 160/3 and 60 = 240/4 would be real), but it is a
parallel interface and cannot drive this 2-wire SPI panel.

## Build / flash

```
idf.py set-target esp32s3
idf.py build flash monitor      # 115200 on either USB port
```

Default partition table, 221 KB app, 8 MB flash setting. Same wiring as
the msp3520 projects (SCK GPIO12, MOSI GPIO11, CS GPIO10, DC GPIO9,
RESET GPIO14, backlight GPIO47 — the probe only holds them inactive).

## Files

- `main/main.c` — the whole probe (~250 lines, heavily commented)
- `sdkconfig.defaults` — 8 MB flash only; everything else is stock

See `../msp3520-clock-probe-firmware/` for the flashable binary bundle.

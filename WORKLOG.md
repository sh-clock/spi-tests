# WORKLOG — MSP3520 (ILI9488 480x320 SPI) video performance project

Repo: sh-clock/spi-tests (GPL-3.0). Target: fullscreen 480x320 video, **24 fps** target,
**15 fps** hard minimum (source video rate), on ESP32-S3 + MSP3520 module.
Safe SPI limit per owner's sweep: "60 MHz" (see §2 — this is actually 40 MHz).

## COMMANDMENTS (from the owner, embedded so they are never forgotten)

- PRIORITISE ACCURACY ABOVE ALL ELSE (x3). Efficiency is second.
- TEST ALL WORK PRIOR TO SHIPPING. Full audit, not smoke tests.
- USE VLM / PIXEL PEEPING to verify output correctness. Visual audits matter.
- USE MULTIPLE PARALLEL AGENTS for code checks; combine findings in this WORKLOG.
- Check offending lines + context before fixing — avoid false positives.
- Emulate the target platform as best as possible (ESP32-S3; homemade ILI9488
  emulator built from reputable docs if no established emulator exists).
- PREFER COPYING VERBATIM from permissively-licensed open source; port logic, keep
  functionality verbatim. No hacks/patches to "fix" output — understand everything.
- Push consistently to this repo so progress is never lost.
- If context runs out: STOP and report state; the order to continue will be given.
- Read code fully, no skimming. No hallucinated references — hard evidence only.
- Stress test with common AND niche cases; fix-test-fix until zero errors remain.
- Codespaces/Actions allowed but do not overbill (turn off, keep clean).

## 1. Current state

- `firmware/msp3520-video-oc-test` — v1 player (RGB666/3B-px, LZSS+RLE "MIV1" blob,
  interlaced fields, per-line DMA queue, MADCTL landscape fix verified). Known good
  on hardware (owner confirmed video displays, orientation fixed).
- v2 (this branch of work): **RGB565/2B-px wire format** + real-clock logging +
  deeper DMA pipeline. Status: IN PROGRESS (see §5).

## 2. Research findings (verified, sources cited)

1. **"60 MHz" does not exist on ESP32-S3 GPSPI.** The SPI master clock derives from
   an 80 MHz source with integer dividers; requests map as: 35–60 MHz → **40 MHz**,
   65–80 MHz → **80 MHz** (ESP-IDF `spi_ll_master_cal_clock`; TRM §30.7).
   Consequence: the owner's sweep result "clean up to 60, corrupt above" means
   **40 MHz clean, 80 MHz corrupt** on the current wiring. All fps math must use
   5 MB/s (40 MHz) not 7.5 MB/s. Fix under test: print the *actual* clock via
   `spi_device_get_actual_freq()`; try 5V supply + short wires to unlock 80 MHz.
   Sources: docs.espressif.com .../peripherals/spi_master.html; S3 TRM v1.8 §30.7.
2. **LovyanGFX forces 18bpp (RGB666) for ILI9488 over SPI**
   (`Panel_ILI948x.hpp::setColorDepth_impl` — `rgb888_3Byte` whenever bus is SPI;
   comment cites ILI9486 spec p.125). 18bpp caps fullscreen at 10.8 fps @40 MHz —
   BELOW the 15 fps minimum. LovyanGFX (FreeBSD/BSD-2, GPL-3-compatible) is
   therefore NOT used for the video path; our audited driver is. Its S3 SCT
   (segmented DMA) benefit ≈ ESP-IDF's automatic descriptor chaining (99.8% bus
   utilisation documented). Integration recipe kept in §6 for optional later use.
3. **ILI9488 16bpp (COLMOD 0x55) over 4-wire SPI is datasheet-listed** (§4.7.2:
   "65K-Colors, RGB 5,6,5 bits input data (set 3Ah as 101)") and is the shipping
   default of Arduino_GFX for ILI9488 (`Arduino_ILI9488.cpp: writeC8D8(0x3A,0x55)`).
   TFT_eSPI/LovyanGFX choose 0x66 for SPI (ILI9486-family caution). Evidence
   supports 0x55 on ILI9488; hardware test will confirm (owner flashes).
4. **ESP-IDF DMA facts**: per-transaction hardware cap 32 KB; driver auto-chains
   4092-byte descriptors, CS held; polling-DMA overhead ~11 µs/transaction;
   formula `n/(20+8n/F)` µs → large chained transfers reach ~99% of bus speed.
   No S3 GPSPI errata. Source: spi_master docs + `dma_types.h`, `spi_ll.h`.
5. **Fps arithmetic at 480x320** (100% bus):
   | format | bytes/frame | @40 MHz | @80 MHz |
   |---|---|---|---|
   | RGB666 3B/px | 460,800 | 10.8 fps | 21.7 fps |
   | **RGB565 2B/px** | **307,200** | **16.3 fps** | **32.6 fps** |
   => 15 fps minimum needs 565 at 40 MHz (92% duty) — v1 (666) could NEVER reach
   15 fps. 24 fps needs 80 MHz (or dirty-line skipping, see §6).
6. Community scan: no public project demonstrates 24 fps software-decoded video at
   480x320 on ESP32; raw-stream players (moononournation, MIT-incompatible repos —
   techniques only, no code copied) hit 24–30 fps at 320x240 @80 MHz. Our
   pre-compressed flash blob avoids both decode and storage bottlenecks.

## 3. v2 design (implemented in this repo)

- Wire format: **RGB565, big-endian pixel, COLMOD 0x55**, MADCTL 0x28 (unchanged
  geometry/color-order behavior; BGR bit semantics identical to verified v1).
- Blob "MIV2": same LZSS(64K win)+per-line-RLE layers; RLE run = `[count-1][hi][lo]`;
  header field `reserved` → `bpp` (=2). Decoder change is 6 lines; LZSS untouched.
- `TX_LINES_IN_FLIGHT` 2 → 4 (overlap decode/DMA; SPI queue sized accordingly).
- Console prints the ACTUAL SPI clock next to the requested one.
- Fallback documented: if a panel rejects 0x55 (unlikely per §2.3), revert to v1
  commit (this repo) or set COLMOD 0x66 + rebuild blob with `--bpp 3`.

## 4. Verification protocol (per commandments)

- Host roundtrip: converter re-decodes LZSS+RLE and byte-compares ALL frames (python)
  AND the real device `video.c` compiled with gcc must produce byte-identical
  output (`tools/host_test_video.py`). This is the pixel-level guarantee.
- Binary audit: byte-pattern check that built firmware contains MADCTL 0x28 init
  entry and COLMOD 0x55.
- Hardware (owner): flash, confirm colors/orientation, read measured fps at
  40 MHz (expect ~14–16) and optionally 80 MHz after the 5V/short-wire test.

## 5. Status / next steps

- [x] Research (§2) — three parallel agents, sources cited above.
- [x] Baseline v1 imported + this worklog.
- [ ] v2 implementation (§3) — IN PROGRESS at time of this commit.
- [ ] Host verification (§4) + firmware build + binary audit.
- [ ] Parallel audit agents on the v2 diff (coordinate via this file).
- [ ] Owner hardware test: fps @40 MHz actual, color check, 80 MHz attempt.
- [ ] Optional next iterations (see §6).

## 6. Future ideas (documented, not blocking)

- Dirty-line skipping (send only changed line spans; huge win on static scenes).
- 80 MHz unlock experiments: 5V VCC (module LDO then regulates properly — the
  "3.3V + J1 open" configuration starves the rail, plausibly the 80 MHz
  corruption cause), shorter/grounded jumper wires, IOMUX pins (already used).
- LovyanGFX optional integration: clone into `components/`, project CMakeLists
  `list(APPEND EXTRA_COMPONENT_DIRS ...)`, `REQUIRES LovyanGFX`; CI-tested on
  IDF 6.1 (their `examples/Test/build_test`). Only for widget/sprite UIs —
  NOT the video path (18bpp lock, §2.2).
- ILI9488 GRAM emulator (command-stream → PNG) for full-stack pixel audit; QEMU
  esp32s3 smoke run (GPSPI DMA fidelity unverified — time-boxed).

## 7. Build & flash (ESP-IDF v6.1)

```
cd firmware/msp3520-video-oc-test
idf.py set-target esp32s3 && idf.py build
idf.py -p COMx flash monitor      # or esptool write-flash 0x0 <merged>.bin
# regenerating video.bin:
python3 tools/convert_video.py assets/Hill.mp4 firmware/msp3520-video-oc-test/main/video.bin
python3 tools/host_test_video.py  # must print HOST VERIFY: PASS
```

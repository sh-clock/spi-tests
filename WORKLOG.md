# WORKLOG — MSP3520 (ILI9488 480x320 SPI) video performance project

Repo: sh-clock/spi-tests (GPL-3.0). Target: fullscreen 480x320 video, **24 fps** target,
**15 fps** hard minimum (source video rate), on ESP32-S3 + MSP3520 module.
Reliable SPI clock per owner's sweep: "60 MHz" requested — see §2.1 (this is 40 MHz actual).

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

- **v3 (current)**: runtime-selectable wire pixel format (`msp3520_set_wire_bpp`),
  **RGB666 / COLMOD 0x66 default** (the only format this panel accepts on SPI —
  see §8), bpp-driven blob decoder ("MIV2" header, bpp 2 or 3), converter `--bpp`
  flag (default 3). The player sets the wire format from the blob header.
- v2 (commit 794cdb4): RGB565/0x55 — **BROKEN ON THIS PANEL: blank screen**.
  Root cause in §8. Kept in history as the cautionary tale.
- v1 (commit 0a5b4ba): RGB666, fixed geometry — hardware-verified working.

## 2. Verified facts (sources cited)

1. **"60 MHz" does not exist on ESP32-S3 GPSPI.** Integer dividers from 80 MHz:
   requests 35–60 MHz → **40 MHz**, 65–80 MHz → **80 MHz**. The owner's sweep
   result "clean to 60, corrupt above" = 40 MHz clean / 80 MHz corrupt.
   Sources: docs.espressif.com .../peripherals/spi_master.html; S3 TRM §30.7.
2. **LovyanGFX forces 18bpp (RGB666) for ILI9488 over SPI**
   (`Panel_ILI948x.hpp::setColorDepth_impl`, comment cites ILI9486 spec p.125).
   FreeBSD/BSD-2 license, GPL-3-compatible. NOT used for the video path — no
   benefit (bus-bound streaming) and it forces the same 18bpp we now use anyway.
3. **ILI9488 16bpp (COLMOD 0x55) DOES NOT WORK on native 4-wire SPI** —
   see §8 for the full evidence chain. (This CORRECTS this WORKLOG's v2-era
   assessment, which relied on the datasheet bullet list and Arduino_GFX's
   default class — both misleading: the datasheet lists 0x55 for DBI but
   documents no 16-bit SPI data packing, and Arduino_GFX's plain ILI9488 class
   is for PARALLEL buses; its SPI class is the 18-bit one.)
4. **ESP-IDF DMA**: per-transaction hardware cap 32 KB; driver auto-chains
   4092-byte descriptors, CS held; polling overhead ~11 µs/transaction;
   large chained transfers reach ~99% of bus speed.
5. **Fps physics at 480x320, RGB666 (0.46 MB/frame)**:
   | bus | ceiling | verdict |
   |---|---|---|
   | 40 MHz (5 MB/s) | **10.8 fps** | v3 expectation ~10 |
   | 80 MHz (10 MB/s) | **20.7 fps** | needs the 80 MHz experiment (§6) |
   | 88.5+ Mbps | 24 fps | **physically impossible** — above the S3's max SPI bus |
6. **Dirty-line skipping measured NOT worth it for this content**
   (`tools/measure_dirty.py`, Hill video, 118 frame transitions): 5.1% of field
   lines fully identical, 50.8% partial-span (wide spans), net wire saving
   **5.7%** (436,511 vs 462,720 B/frame; best frame 0 B = loop point, worst
   frame = full frame). Implied fps @40 MHz: 10.8 → 11.4. A "MIV3" skip format
   would buy ~0.6 fps — rejected on evidence.

## 3. v3 design

- Wire format: **RGB666, 3 bytes/pixel, COLMOD 0x66**, selected at runtime via
  `msp3520_set_wire_bpp(2|3)`; the player calls it with `video_hdr()->bpp` so
  the blob drives the format. `msp3520_reinit_panel()` re-asserts COLMOD after
  each hardware reset (the sweep reinits per frequency).
- Line buffers sized for the max format (1440 B); `msp3520_queue_line` sends
  `480 * wire_bpp` bytes. `LCD_MAX_TRANSFER` covers the 666 line.
- Blob "MIV2": header carries bpp (3 = RGB666 default, 2 = RGB565 for
  parallel-interface modules); RLE runs = `[count-1] + bpp payload bytes`;
  LZSS layer unchanged. Decoder accepts bpp 2 or 3.
- Interlacing (even/odd fields) kept: halves per-update latency at the same
  average throughput.
- `TX_LINES_IN_FLIGHT` = 2 (bus-bound; decode outpaces the wire).

## 4. Verification protocol (all must pass before shipping)

1. Converter roundtrip: all frames byte-identical (in-script asserts).
2. **Blob gate**: regenerated blob header must be `MIV2` with bpp=3 (a stale
   565 blob once got past the old chain — see §9 incident 2).
3. Host verify: gcc-compiled device `video.c` decodes the whole blob
   byte-identical to the Python reference (`tools/host_test_video.py`).
4. Binary byte-audit: init table contains MADCTL 0x28 + COLMOD 0x66 exactly
   once; the v2 combo (0x28+0x55) and the portrait bug (0x48) must be absent.
5. Hardware (owner): flash, confirm picture + fps at 40 MHz (~10 expected).

## 5. Status

- [x] v2 root-caused (§8) — 0x55 unsupported on native-SPI ILI9488.
- [x] v3 implemented: runtime wire-bpp, RGB666 default everywhere.
- [x] Dirty-line compression evaluated on real content: rejected (5.7%).
- [x] Harness bugs fixed (§9 incidents 1–2): bpp-generic RLE emission,
  bpp-aware host-test line dump.
- [x] RGB666 blob regenerated (7,636,014 B, MIV2 bpp=3) + converter
      roundtrip PASS + blob-header gate PASS.
- [x] Host verify PASS: device decoder byte-identical to the reference,
      sha256 c9e275970e3fc83e (54,835,200 bytes = 238x160x1440).
- [x] Pixel audit PASS: decoder frames 0/20/40/60/80/100 vs the source
      (same pipeline) — max channel error 7 = exactly the RGB666
      quantization bound; mean 2.88.
- [x] VLM visual audit: initial "CORRUPT" verdict proven a FALSE POSITIVE —
      the source video's opening frame is genuinely overexposed/washed-out
      (a raw ffmpeg frame, bypassing our pipeline, gets the identical
      description under a neutral prompt). Decode correctness stands on
      the pixel audit.
- [x] v3 firmware built (app 7,865,488 B, 6% free) + binary byte-audit
      PASS (MADCTL 0x28 + COLMOD 0x66 present exactly once; the v2 0x55
      combo and the portrait 0x48 bug both absent) + packaged
      (firmware/msp3520-video-firmware-v3/ + download/ zips).
- [ ] Owner hardware test: picture OK + ~10 fps @ 40 MHz.
- [ ] 80 MHz stability experiment (§6) — the ONLY path to 15+ fps.

## 6. Roadmap (revised after §2.5/§2.6 measurements)

1. **80 MHz experiment (critical path for ≥15 fps)**: the earlier corruption at
   "65–80 MHz" (= real 80 MHz) may be supply-induced — the module run at 3.3 V
   with J1 open starves the rail (~3.1 V). Try: 5 V on VCC (module's LDO then
   regulates properly), SCK/MOSI jumpers as short as possible (<10 cm), ground
   return alongside, then run the firmware's retest loop at 70/75/80.
   Payoff: 20.7 fps ceiling (RGB666, fullscreen).
2. If 80 MHz stays corrupt on this module: the practical ceiling on this
   hardware is ~10 fps. Options then: accept it, or a parallel-interface
   module (those accept RGB565 → 16.3 fps @40 MHz / 32.6 @80 MHz).
3. 24 fps is **not achievable on ILI9488 over SPI** (needs 88.5 Mbps > 80 MHz
   max bus). If 24 is a hard requirement, the panel must change (parallel RGB
   panel, or a module with onboard SPI→parallel converter running RGB565, or
   a different controller).
4. Rejected on evidence: dirty-line skipping (5.7%), LovyanGFX (no gain,
   forces same 18bpp), dual-core/PSRAM (bus-bound, not decode-bound).

## 7. Build & flash (ESP-IDF v6.1)

```
cd firmware/msp3520-video-oc-test
idf.py set-target esp32s3 && idf.py build
idf.py -p COMx flash monitor
# regenerating video.bin (RGB666 default):
python3 tools/convert_video.py assets/Hill.mp4 firmware/msp3520-video-oc-test/main/video.bin
python3 tools/host_test_video.py   # must print HOST VERIFY: PASS
# dirty-line savings measurement (why we rejected it):
python3 tools/measure_dirty.py     # decodes the source video itself
```

## 8. Incident log — v2 blank screen (2026-11-14)

**Symptom**: v2 firmware (RGB565, COLMOD 0x55) — backlight on, panel completely
blank, zero errors on the console; all SPI transactions completed. v1 (RGB666)
on the same wiring showed perfect video.

**Root cause**: the ILI9488 does **not** support 16-bit RGB565 on its native
4-wire SPI (DBI Type-C) interface, despite §4.7.2 of the datasheet listing it
in a bullet list. That same datasheet documents SPI data packing ONLY for
3-bit and 18-bit (Fig 106/107) — there is no 16-bit SPI packing figure. Every
major library independently forces 18-bit for ILI9488 SPI:

- TFT_eSPI: `ILI9488_Init.h` writes 0x55 only for parallel; SPI gets 0x66.
  Bodmer, TFT_eSPI issue #40 (2021): *"The screen can only be used in 18bit
  colour mode with a SPI interface. In parallel mode it will work with 16 bit
  colours."* — the same issue reports the blank screen with 0x55 on SPI.
- LovyanGFX: `Panel_ILI948x.hpp` comment: "ILI9481/ILI9486 は SPIバス接続時は
  16bppが使用できない" (cannot use 16bpp on SPI), citing ILI9486 spec p.125.
- Arduino_GFX: plain `Arduino_ILI9488` (0x55) is the PARALLEL class; wiki:
  *"This display SPI interface not support 16-bit SPI"*; issue #777 = our exact
  symptom (ESP32-S3 + ILI9488 SPI, off-white blank with the 16-bit class).
  Maintainer: *"According to ILI9488 data sheet, SPI not support 16-bit color
  mode. That's why it dedicated a 18bit class."*
- micropython-nano-gui DRIVERS.md §3.4.1: *"The chip only supports RGB565 color
  when driven with a parallel interface. Native SPI supports only 18-bit
  RGB666."* Modules that DO show RGB565-over-SPI carry an onboard
  SPI→parallel converter (Waveshare approach). The MSP3520 is native SPI.
- LCDWiki's own MSP3520 demo package: every Arduino sketch uses
  `MODEL ILI9488_18` → init writes COLMOD **0x66**.

**Fix (v3)**: revert the wire format to RGB666/0x66, but keep it runtime-
selectable (`msp3520_set_wire_bpp`) with the blob header carrying bpp, so the
converter can still emit 565 blobs for parallel-interface modules.

**Lesson recorded**: a datasheet bullet list is not a guarantee; the absence of
a documented data-packing figure plus every field library's contrary choice was
the real signal. v2 shipped because that signal was under-weighted.

## 9. Incident log — build-chain catches (2026-11-14, same day)

1. **Converter crash (v3 edit gap)**: `build_rle_stream` still unpacked runs
   as `hi, lo = line[s]` (565-only) after the bpp-generic refactor →
   `ValueError: too many values to unpack` on a 666 stream, and the blob was
   silently NOT regenerated. Fixed with bpp-generic emission
   (`px = line[s].tobytes()`).
2. **Stale-blob near-miss**: because of (1), the old 565 blob stayed in place;
   the host test then failed with a size mismatch — exposing that
   `test_video_host.c` dumped `VIDEO_LINE_BYTES` (1440) per line regardless
   of blob bpp, which would have made a 565 blob "pass" the byte-compare.
   Fixed: the harness now dumps `480 * h->bpp` per line, and the chain gained
   an explicit blob-header gate (MIV2 + bpp=3) before the host test.
   Also fixed: the chain's `echo rc=$?` masked the converter's exit code —
   gates now run inside the `&&` chain.

## 10. Sandbox/recycle notes (operational)

The build sandbox recycles between sessions and has repeatedly restored STALE
snapshots of the working tree (losing edits made late in a session; twice it
reverted the MADCTL landscape fix, once the whole v2 tree, once the repo clone
itself). Mitigation in force: push to this repo early and often; treat this
GitHub repo, not the sandbox tree, as the source of truth; re-clone after any
recycle and re-verify key bytes (MADCTL 0x28, COLMOD 0x66, MIV2 bpp=3) before
building.

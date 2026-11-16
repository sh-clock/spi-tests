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

- **v4 (current)**: full-frame **progressive** block streaming — address window
  (CASET+PASET) set once per panel init; each frame = ONE RAMWR + 20 pure-data
  DMA transactions of 16 lines (23,040 B) = **41 transactions/frame**.
  v1-v3 sent 4 transactions per line (1280/frame); that overhead was the gap
  between the owner's measured 8.5 fps and the 10.8 fps wire ceiling.
  Expect ~10.5 fps at 40 MHz. Interlacing removed (no combing anymore).
- v3: RGB666 + runtime wire-bpp. Hardware-verified working (~8.5 fps @ 40 MHz
  actual, owner-measured 2026-11-14; 80 MHz snowy even with 5 V + fresh wires).
- v2: RGB565/0x55 — blank screen (see §8). v1: RGB666 interlaced, worked.

## 2. Verified facts (sources cited)

1. **"60 MHz" does not exist on ESP32-S3 GPSPI.** Integer dividers from 80 MHz:
   requests 35–60 MHz → **40 MHz**, 65–80 MHz → **80 MHz**. Sources:
   docs.espressif.com .../peripherals/spi_master.html; S3 TRM §30.7.
2. **ILI9488 max SPI write clock = 20 MHz per datasheet** (§17.4.3, 4-line
   SPI, twc min 50 ns; read = 150 ns). 40 MHz (our stable clock) is already a
   **2x overclock** — and is the highest *reliably reported* ILI9488 SPI clock
   anywhere (TFT_eSPI setups offer 40; loboris "up to 40 tested"; wdim0
   "happy up to 40 MHz"; LovyanGFX maintainer "usually safe at 40"; no
   verified stable report above 40 exists; the one ESPHome 80 MHz report
   crashed). The owner's 80 MHz snow (persisting with 5 V + new short wires)
   is consistent with the **die** being the limiter, not wiring.
   Datasheet: LCDWiki-hosted ILI9488_IDT_V100 §17.4.3.
3. **16-bit RGB565 over native 4-wire SPI: definitively impossible on this
   controller.** (Owner asked to re-check everything; closure 2026-11-15.)
   - COLMOD 0x55 (DBI=101) is a valid register code and §4.7.2 even lists
     "65K colors" for the 4-line interface — but the 343-page datasheet
     documents **no byte packing for it** (only 3-bit §4.7.2.1 and 18-bit
     §4.7.2.2 have SPI data-format figures). 16-bit packing exists ONLY for
     parallel (§4.7.3.1/§4.7.5.1) and MIPI DSI (§4.6.3.4).
   - Interface Mode Control (0xB0) contains no format switching (SDA_EN +
     RGB polarities only) — checked exhaustively.
   - The module vendor's own manual: "The ILI9488 controller uses 18bit
     (RGB666) to control a pixel display."
   - Every maintained library forces 18-bit for ILI9488 SPI: TFT_eSPI
     (0x66 for SPI, 0x55 only parallel), LovyanGFX (structurally), Arduino_GFX
     (issue #777 = our exact blank symptom, closed "SPI not support 16-bit
     color mode"), atanisoft esp_lcd (converts 16→18 in the callback).
   - Modules that DO accept 16-bit SPI carry a board-level SPI→parallel
     converter (KeDei v4.0, IM[2:0]=011) or a different controller
     (ST7796: COLMOD 0x55 native + 80 MHz-class clocks; HX8357D). The
     MSP3520 is native SPI (IM=111).
   - The owner's v2 firmware was itself the field experiment: 0x55 = blank.
   - The only reduced-byte SPI mode that exists is 3-bit/8-color (§4.7.2.1)
     — unusable for video.
4. **ESP-IDF DMA**: per-transaction hardware cap 32 KB; driver auto-chains
   4092-byte descriptors, CS held; polling overhead ~11 µs/transaction.
5. **Transaction overhead was worth ~2.3 fps at 40 MHz (measured)**:
   v3 = 4 transactions/line × 160 lines = 640 per field = 1280 per frame.
   Measured 8.5 fps vs 10.8 wire ceiling ⇒ ~12 ms/field dead time ⇒
   ~18-20 µs per transaction of driver/handoff overhead. v4 cuts to 41
   transactions/frame. Fps physics at 480x320 RGB666 (460,800 B/frame):
   | bus | ceiling | |
   |---|---|---|
   | 40 MHz | **10.8 fps** | v4 expected ~10.5 (v3 measured 8.5) |
   | 80 MHz | 20.7 fps | dead on this panel (§2.2 — 4x overclock) |
   | 24 fps | 88.5 Mbps | impossible on ILI9488 over SPI, any clock |
6. **Dirty-line skipping: rejected on measurement** (5.7% wire saving on this
   content — it is a full-viewport pan; 5.1% of lines static). tools/measure_dirty.py.
7. **LovyanGFX forces the same 18bpp we use** — no benefit (bus-bound).

## 3. v4 design (progressive block streaming)

- `msp3520_prepare_stream()`: CASET 0..479 + PASET 0..319 once per panel
  init. The controller wraps rows inside the window on RAMWR.
- `msp3520_field_begin()`: CS low + RAMWR (polled, 1 byte). Write pointer
  resets to row 0; **the panel keeps consuming bytes while CS stays low**,
  so the whole frame is one continuous write as far as it is concerned.
- `msp3520_queue_block()`: one 16-line (23,040 B) DMA data transaction.
  20 blocks/frame, TX_BLOCKS_IN_FLIGHT=2, retire per block. Player decodes
  the next block while DMA pushes the previous one.
- Blob "MIV2" gains flags bit1 = progressive; the v4 player requires it
  (video_open rejects legacy interlaced blobs). Converter: `--scan
  progressive|interlaced` (default progressive). bpp-driven decode kept
  (3 = RGB666 default, 2 = RGB565 for parallel-interface modules).
- Fill: one replicated 16-line block × 20, unpipelined (boot only).
- Interlacing removed: no combing, simpler protocol, fewer transactions.
  At ~10 fps its 2x-update-rate smoothness benefit was not worth 2x the
  transaction count.
- `LCD_EDGE_DRIVE_CAP` knob in msp3520.h (-1 = off): optional weaker SCK/MOSI
  drive for the 80 MHz experiment (see §6).

## 4. Verification protocol (all must pass before shipping)

1. Converter roundtrip (in-script asserts, all frames byte-identical).
2. Blob gate: MIV2 + bpp=3 + flags bit1 (progressive).
3. Host verify: gcc-compiled device video.c decodes the whole blob
   byte-identical to the Python reference (tools/host_test_video.py).
4. Binary byte-audit: init table = MADCTL 0x28 + COLMOD 0x66 exactly once;
   the v2 combo (0x28+0x55) and portrait bug (0x48) absent.
5. Hardware (owner): picture OK, ~10.5 fps at the 35-60 MHz steps.

## 5. Status

- [x] 16-bit recheck closed (§2.3) — no path, high confidence.
- [x] 80 MHz verdict (§2.2) — die-limited; 40 MHz is the practical ceiling.
- [x] 8.5 fps gap root-caused (transaction overhead, §2.5).
- [x] v4 implemented: progressive block streaming (driver + player +
      converter + decoder flag).
- [x] v4 chain: progressive blob (7,254,058 B) + gate + converter roundtrip
      PASS + host verify PASS (sha256 11e5be3aef5fb81a, 54,835,200 bytes)
      + build complete.
- [x] Pixel audit + binary byte-audit + package + push.
- [ ] Owner hardware test: expect ~10.5 fps @ 40 MHz actual, no combing.
- [ ] Optional last $0 experiments for 80 MHz (§6) — low expectation.
- [x] v5 (2026-11-15): interlacing challenge answered (§14), column-
      interlace A/B mode built ('i' key), README de-staled, pushed.

## 6. Roadmap

1. **This panel's realistic ceiling is ~10.5-10.8 fps** (40 MHz, RGB666,
   fullscreen). 15 fps would need ~62 MHz of clean bus; the ILI9488's
   silicon does not do it (§2.2), and 16-bit is impossible (§2.3).
2. Last $0 experiments for 80 MHz (in order, low expectation — 4x overclock):
   a. Series resistor 47-100 Ω on SCK at the ESP32 end (community range
      33-150 Ω; dampens reflections on ~10 cm jumpers), ground wire routed
      adjacent to SCK (SCK-GND-MOSI-GND ordering).
   b. `LCD_EDGE_DRIVE_CAP 1` (weaker SCK/MOSI edges) + rebuild — knob is in
      the driver, default off.
   c. Re-run the firmware's retest loop at 70/75/80.
3. If more fps is a hard requirement, the panel must change: same-form-factor
   **ST7796** module (LCDWiki MSP3525/26) natively supports RGB565 over SPI
   and tolerates 80 MHz-class clocks → ~32 fps at 80 MHz. Not pursued (owner
   does not want a new screen).
4. Rejected on evidence: dirty-line skipping (5.7%), LovyanGFX (no gain),
   dual-core/PSRAM (bus-bound), 3-bit 8-color mode (unusable quality).

## 7. Build & flash (ESP-IDF v6.1)

```
cd firmware/msp3520-video-oc-test
idf.py set-target esp32s3 && idf.py build && idf.py -p COMx flash monitor
# regenerate the blob (RGB666 progressive by default):
python3 tools/convert_video.py assets/Hill.mp4 firmware/msp3520-video-oc-test/main/video.bin
python3 tools/host_test_video.py    # must print HOST VERIFY: PASS
```

## 8. Incident log — v2 blank screen (2026-11-14)

**Symptom**: v2 (RGB565, COLMOD 0x55) — backlight on, panel blank, no SPI
errors; v1 (RGB666) perfect on the same wiring.

**Root cause**: ILI9488 native 4-wire SPI does not support 16bpp. Evidence
chain in §2.3 (datasheet gap + 0xB0 + vendor manual + four libraries +
Arduino_GFX #777 + our own field test). Fixed in v3 (RGB666, runtime-
selectable wire format). Lesson: a datasheet bullet list is not a guarantee;
the absence of a data-packing figure plus every field library's contrary
choice was the real signal.

## 9. Incident log — build-chain catches (2026-11-14)

1. Converter crash: `build_rle_stream` still unpacked `hi, lo` (565-only)
   after the bpp-generic refactor → blob silently NOT regenerated. Fixed
   (bpp-generic emission) + added the blob-header gate.
2. Stale-blob near-miss: `test_video_host.c` dumped `VIDEO_LINE_BYTES` per
   line regardless of blob bpp, which would have made a 565 blob "pass".
   Fixed (`480 * h->bpp`); the gate (MIV2 + bpp + flags) runs before the
   host test from then on.
3. Progressive scan (2026-11-15): two more stale-format assumptions in the
   verification layer — the converter's full-expansion check always
   de-interleaved (AssertionError on a *correct* progressive blob), and
   `test_video_host.c` computed total lines as fields × H/2 (C decode
   stopped at exactly half the stream). Both fixed to be flags-driven; the
   blob itself was correct both times — the fail-fast gates caught the
   harness, not the data.

## 10. Incident log — v3 8.5 fps vs 10.8 ceiling (2026-11-15)

Measured 8.5 fps at 40 MHz (owner). Accounting: 4 transactions/line ×
160 lines/field ≈ 12 ms/field of inter-transaction dead time (~19 µs each:
queue handoff + DMA descriptor setup + polling retire). Fix = v4 (§3):
41 transactions/frame. Also removed interlacing (its 2x-update-rate benefit
at 15 fps was not worth 2x transactions at 10 fps, and it caused combing).

## 11. Sandbox/recycle notes (operational)

The build sandbox recycles between sessions and has repeatedly restored
STALE snapshots (four times so far: twice the MADCTL fix, once the whole v2
tree, once the repo clone + ESP-IDF + setup scripts). Mitigation in force:
push early and often; this GitHub repo is the source of truth; re-clone
after any recycle and re-verify key bytes (MADCTL 0x28, COLMOD 0x66, MIV2
bpp=3 + flags bit1) before building.

## 12. Research verdicts — the 15 fps trawl (2026-11-15, 4 parallel agents)

All claims below carry source URLs in the session record; headline verdicts:

1. **Parallel/8080/DPI on the MSP3520: physically impossible.** Vendor
   schematic: DB0–DB17 (FPC pins 15–32), DEN/PCLK/HSYNC/VSYNC (34–37) and
   TE (pin 7) are UNROUTED on the module PCB; IM2 strapped to 3V3 (=111,
   4-wire SPI); RD tied high; the header is 14 pins (the "18" = 14 + the
   unpopulated SD-card header J4). Only FPC micro-surgery (0.5 mm pitch)
   would reach the parallel bus. Sources: LCDWiki schematic
   (3.5寸SPI模块原理图.pdf), QD3503 panel spec, MSP3520 user manual.
2. **80 MHz SPI on ILI9488: die-limited verdict reinforced, but not
   absolutely closed.** Convergent ceiling evidence: ESPHome default 40,
   LovyanGFX maintainer "usually safe 40", LVGL forum 80 MHz = black
   screen, RPi fbcp ~50 MHz borderline-with-corruption. Verified counter-
   examples on other hosts/boards: 65 MHz (ESP32 CYD, PROF0UND issue #1),
   60 MHz (Teensy 4.1, vindar/ILI9341_T4 #5, ~16 fps ILI9488), 80–93.75
   MHz no-glitch (PicoCalc ILI9488, Rockchip host, PCB-short traces,
   picocalc-drivers PR #32), TFT_eSPI ships a commented 80 MHz option.
   ESP32-S3-specific factor (IDF docs): GPIO-matrix routing == IOMUX only
   ≤40 MHz; above that, IOMUX pins + shortest wires matter — our SCK=12/
   MOSI=11 ARE the FSPI IOMUX pins already. Remaining $0 levers: series-R
   (22–100 Ω on SCK at the ESP32 end), GND wire alongside SCK, <100 mm
   wires, and GPIO drive-capacity tuning (now a live hotkey, §13).
   Expectation: LOW–MEDIUM. 55–65 MHz is the well-evidenced gray zone —
   but the S3 GPSPI cannot generate it (40 or 80 only), so it is
   80-or-nothing on this module.
3. **15 fps arithmetic on this module over SPI: only an 80 MHz rescue can
   do it.** Every other lever is verified insufficient for this content:
   dirty-rect ≈ 0 gain on full-screen pan (rjonkman: 60 fps sparse vs
   20 fps heavy animation); interlacing has no precedent + combs; TE pin
   unrouted; DMA-gap tweaks ≤ 0.5%; 3-bit mode rejected by owner; 16-bit
   impossible (5th independent confirmation — vindar maintainer quote).
4. **If 80 MHz fails: hardware swap is the only path.** Same panel
   parallel = MakerFabs ESP32-S3 Parallel 3.5" ILI9488 (16-bit 8080 via
   esp_lcd i80; verified 37→58 fps LVGL vs SPI; ~86 fps ceiling at
   20 MHz PCLK). Or ST7796 SPI module (RGB565 native, 80 MHz-class →
   ~32 fps). NOT pursued — owner does not want a new screen.
5. **esp_lcd I80 implementation fact sheet captured** (for a future swap):
   PCLK = 80/n from PLL160M (n=1..64) or 120/n from PLL240M (30/24 MHz
   reachable), ILI9488 8080 twc = 40 ns (25 MHz max), IM straps 011 =
   8-bit / 010 = 16-bit, official i80_controller example is CC0-1.0,
   atanisoft/esp_lcd_ili9488 (MIT) has the full init table + draw_bitmap
   pattern; DC/CS driven by hardware per phase; GPIO-matrix = any pins.

## 13. v4.1 (2026-11-15) — the 80 MHz experiment build

- New live hotkey 'd' during playback cycles SCK/MOSI GPIO drive
  capability 0→1→2→3 (msp3520_set_edge_drive(), gpio_drive_cap_t). The
  compile-time LCD_EDGE_DRIVE_CAP block was removed (superseded).
  Protocol: at a snowy 65–80 MHz step, press d repeatedly and watch which
  drive setting (if any) clears the snow — weaker (0/1) = slower edges =
  less ringing on jumper wires.
- Combined with the §12 wiring levers (series-R on SCK, GND alongside
  SCK, <100 mm wires) this is the complete remaining $0 experiment space
  for 80 MHz. If none of it works, the module is definitively at its
  ceiling (~10.5 fps) and 15 fps requires different hardware (§12.4).

## 14. v5 (2026-11-15) — the interlacing challenge, answered + column-interlace A/B

The owner challenged §10's "interlacing adds no fps": "isn't it literally
the whole point of the technology to push less bandwidth over?" Four
parallel research agents (broadcast/display theory, GRAM-panel prior art,
datasheet mechanics, motion perception). Verdict — both sides are right
about different things:

1. **The owner is right about what interlacing is FOR.** It halves the
   bytes per FIELD (230,400 vs 460,800) → doubles the field/update rate
   (21.7/s at 40 MHz). That is exactly the 1934 Mertz & Gray broadcast
   trade (spatial-for-temporal at fixed bandwidth).
2. **It has never halved the bytes per PICTURE.** Every interlaced system
   carried 25/30 unique pictures/s at 50/60 fields/s. The temporal gain
   existed only for FIELD-NATIVE sources (50i/60i cameras); for
   progressive sources in an interlaced carrier (PsF/telecine) the
   literature is explicit: zero new motion information. Our source is
   15 fps progressive → the PsF case.
3. **Per-pixel update rate under interlacing = field_rate/2 = 10.85 Hz**,
   identical to v4 progressive. On a hold-type GRAM panel there is no
   luminance modulation at 21.7 Hz for the eye to fuse — the CRT trick
   (phosphor decay + eye integration) does not transfer.
4. **Row-interlace on GRAM SPI panels IS established prior art** —
   juj/fbcp-ili9341 ships it adaptively ("This effectively doubles the
   maximum display update rate"; on ILI9486-class "the rule rather than
   the exception... works much smoother when applying throttling");
   rickyzhang82 ESP32+ILI9341 NES interlace; MstfOztrk scanline GIF
   player. ZERO instances of column interlace found anywhere. Note juj's
   source is a ~60 fps Pi framebuffer (field-native case) — not ours.
5. **Perception (moderate confidence, ~65–70%, no direct study of
   two-block staggering):** a REGULAR ~46 ms update cadence may beat
   irregular full-frame steps — 10.85 Hz sits at the 10–12 Hz discrete-
   event visibility floor, and the telecine literature says irregular
   hold times are the primary judder driver, worst on slow pans.
   Content-dependent → hence an A/B toggle, not a default change.
6. **Datasheet mechanics all CONFIRMED** (agent-quoted): §5.2.24 every
   RAMWR resets the write pointer to the window origin; §4.3/4.3.1 CS
   release between whole bytes = legal pause, new command after = fine;
   §6.3 rows wrap inside any window; under MADCTL 0x28 (MV=1) CASET spans
   the 480-px display axis; NO stride/skip mechanism exists (so v1–v3 row
   interlace inherently needed 160 single-row windows per field — the
   reason it cost ~2 fps).

**v5 implementation** ('i' hotkey; progressive stays the default):
- Column halves are CONTIGUOUS windows (CASET 0..239 / 240..479 × PASET
  0..319) → v4-style block streaming per half: ~21 upd/s at 40 MHz (v3
  row interlace managed 17 fields/s), and combing becomes one stationary
  mid-screen seam (shear only during motion).
- The decoder is forward-only → push_ilace_frame() decodes each frame
  ONCE into a 460,800 B PSRAM stage (CPU-only reads; DMA still reads the
  internal s_block rows, which double as the 11,520 B half-block staging
  buffers) and slices both halves from it. Decode is NOT overlapped
  (~3–8 ms/frame serialized → a few % below theory; ping-pong staging is
  the natural v5.1 follow-up if the A/B favors the concept).
- Mode-toggle safe: the window is re-asserted on every progressive frame
  (push_frame now calls prepare_stream itself); fps counter stays
  frame-based (comparable with v4 numbers), upd/s shown alongside.
- Byte audit extended: the half-window CASET table (00 00 00 EF 00 F0
  01 DF) and the "ynrqdi" key string must each appear exactly once.

**Incidents this round:**
- FIFTH sandbox recycle: stale v2-era tree restored locally (row-interlace
  code, MADCTL 0x48!) + repo clone, ESP-IDF and setup scripts wiped. The
  §11 re-verify procedure caught it immediately (grep for {0x28} found 0
  → traced to LCD_MADCTL macro, binary audit confirmed 0x28 — genuine;
  the staleness was in the RESTORED LOCAL tree, discarded and re-cloned).
- The project README was still the v2-era interlace document (never
  updated for v4 — "combing is normal", 5.4 MB blob, queue_line API) and
  the firmware banner still said "interlaced VIDEO player". Both likely
  fed the interlacing confusion. README rewritten for v5; banner fixed.

**v5 audit round (3 parallel agents — all findings applied):**
- Interlace engine itself: NO bugs (logic/timing clean; datasheet 6/6
  CONFIRMED incl. Figure-129 axis mapping, RAMWR reset, CS pause,
  exact 76,800-px count; edge cases clean).
- FIXED: (a) NUL byte passed the strchr key check — one noise byte
  could silently FAIL a sweep step (ask_key/poll_key now guard ch!=0);
  (b) push_frame hard-fail (corrupt blob only) left CS low + one
  queued tx — now drains and raises CS first; (c) "~41 SPI
  transactions/frame" was wrong — 25 is correct (4 window polls +
  RAMWR + 20 blocks); (d) video.h contract was stale (progressive
  MIV2 writes fields == frames; shipped blob verified frames=119,
  fields=119, flags=2, bpp=3).
- DECIDED (not changed): SPIRAM stays OFF in sdkconfig.defaults —
  enabling it blind (quad vs octal, module variant unknown) could
  boot-loop the owner's board. 'i' needs a one-time menuconfig enable;
  README/FLASH-INSTRUCTIONS now say so. Safety-first call.
- KNOWN GAP: repo tools/convert_video.py is still the legacy MIV1
  converter (blobs the player rejects); shipped video.bin is MIV2.
  Porting the progressive converter is pending; README warns.
- NOTE: commit 894a3ad contains the pre-fix v5 build (the first chain
  finished during the audit); the follow-up commit carries these fixes.
## 15. v6 — field-interlaced playback (owner directive: frame rate over artifacts)

**Session context.** 6th+ sandbox recycle this session; the disk-full error
that preceded the crash was reset by the recycle itself (fresh filesystem,
15 % used). Everything under my-project survived via the restore snapshot;
the local git clone (spi-tests/.git) and — critically — the GitHub remote
URL + credentials lived only in the crashed conversation context and were
not recoverable from the machine (checked: no gh CLI, no SSH keys, no
.git-credentials, no credential helper, transcript/db hold only this
session). Remote state per WORKLOG: v5.1 pushed as 230aa4d. Recovery of
the local tree was verified before any new work: v5.1 byte audit PASS,
both zips intact, host decoder gate PASS (54,835,200 bytes byte-identical).

**Owner directive (this session):** "no issue with losing vertical
resolution and combing — I just want interlacing that allows for better
frame rate." That rejects v5's honest-cadence-only position and asks for
the classic interlace trade: fewer bytes per picture in exchange for more
pictures per second.

**Design.** The wire ceiling at 40 MHz is ~10.85 full RGB666 frames/s
(460,800 B each) = ~21.7 half-frames/s (230,400 B each). The only
wire-cheap half-picture shape on the ILI9488 is a contiguous half address
window — true even/odd row interlace needs CASET+PASET+RAMWR per line
(1280 tx/frame; the v1–v3 mistake, ~2 fps of pure overhead). So v6:

- **Converter** (`--scan field-halves`, new): the blob is baked as 173
  fields at 21.70 fields/s (the 40 MHz ceiling; `--field-rate`, kept in
  sync with `FIELD_HZ_X100` in main.c). Field k = one 240-px half (even k
  left, odd k right) of the source frame at time k/21.70 — every field
  carries a fresh temporal sample, one field period after the previous.
  Blob: 5,304,991 B (v5 progressive: 7,254,058 B — 27 % smaller; raw
  39,859,200 B → 7.5×). All 119 source frames used; 173 fields sample
  them at 21.70 Hz.
- **Format** (MIV2, flags bit2 = field-halves): fields of 320 RLE line
  blocks whose run counts sum to 240 px. bit1 (progressive) blobs still
  decode — video_open accepts either layout so the decoder stays
  testable; main.c requires bit2 and fails loudly otherwise.
- **Player**: push_field() streams each field straight from the decoder
  into the half-window (20 DMA blocks of 16 lines, ~23 SPI transactions).
  The v5 PSRAM frame stage is GONE — the blob holds fields, so the
  forward-only decoder walks exactly what the wire needs; nothing is
  staged and no PSRAM is required (the v5 menuconfig owner-action is
  obsolete). Pacing: 21.70 fields/s (real time at 40 MHz; slower buses
  run slow-motion, as v5 did below its 15 fps target).
- **Keys**: 'i' removed (v6 has one playback mode); set is now y n r q d.
- **Counters**: fields/s everywhere (status line, per-step, summary).

**What it buys, honestly.** At 40 MHz: ~21.7 fields/s vs ~10.85 full
frames/s — motion advances at double cadence (something moves every ~46 ms
instead of ~92 ms). Per-pixel update rate is unchanged (~10.85 Hz); the
15 fps source yields ~15 unique temporal samples/s displayed (vs 10.85
progressive, +38 %) at 2× cadence — a ≥21.7 fps source would show the
full 2×. Cost (accepted): stationary mid-screen seam; ~46 ms shear across
it on fast pans; static scenes pixel-identical to progressive.

**Verification.**
- Converter roundtrip: all 173 fields byte-identical to the source halves
  (in-script assert on every field).
- Host decoder gate: the exact device video.c decodes 55,360 lines
  (39,859,200 bytes) byte-identical to the Python reference; sha256
  prefix 9e1bf00086f894ea. PASS.
- Byte audit v6 (app bin): MADCTL 0x28 + COLMOD 0x66 ×1; no 0x55/0x48
  regressions; half-window CASET table ×1; key set "ynrqd" ×1 and stale
  "ynrqdi" ×0; embedded blob header (MIV2, flags=4, fields=173) ×1. PASS.
- Build: ESP-IDF v6.1, app size ~5.6 MB (v5: 7.14 MB) — extra flash
  headroom.

**Also this session:** the progressive/field-halves converter and the
host test harness were ported into the repo's tools/ (the copy there was
still the stale v1–v3 MIV1 converter — the pending port noted in the v5.1
README section 2 is done; tools/host_test_video.py now uses portable
paths).

**Owner validation (post-flash, hardware).** v6 was flashed and judged on
the real panel: "certainly looks better... honestly quite fluid until
heavy motion where it flickers in the middle — but I don't care." The
doubled cadence is perceived as fluid; the mid-seam flicker under heavy
motion is the accepted trade. v6 is the keeper.

**Board direction.** The MSP3520 + ESP32-S3 combo is now the PROTOTYPE
board (the panel: "torture screen — scammed me out of 14 bucks"). Next
hardware: a MIPI DSI panel on a Wireless Tag ESP32-P4/C61 — DSI removes
the 40 MHz 1-bit SPI ceiling entirely, so the v6 compression pipeline
(LZSS+RLE, field layout) becomes the reusable part, not the transport.

**Pending owner actions.** (1) ~~Flash v6 and A/B~~ DONE (validated
above). (2) 80 MHz d-key + series-R experiment: still open, now on the
torture screen. (3) DSI panel: model chosen (Wireless Tag P4/C61 host);
bring-up when it arrives. The PSRAM menuconfig step from v5 is obsolete
(v6 needs no PSRAM).
## 16. Research: visually-better signal over reduced bandwidth (2026-11-15, 6 parallel agents)

Owner brief: trawl for techniques that trade imperceptible information for
bandwidth (like interlacing), with feasibility verdicts for the S3/SPI
system. Six agents: analog broadcast, perceptual codecs, foveated/partial
update, display temporal tricks, chroma/dither, retro/demoscene-embedded.

Frame — on this system there are exactly three currencies:
1. WIRE (~5 MB/s at 40 MHz): the ONLY fps lever; fewer pixels per update
   is the only move (clock capped, 3 B/px forced by COLMOD 0x66 on serial).
2. FLASH (8 MB slot, 5.3 MB used): blob size buys clip length, not fps.
3. QUALITY PER BYTE: dithering, quantization policy, cadence.

Verdicts:

- **Dirty-rectangle / motion-weighted window scheduling — the top fps
  lever.** The ILI9488 GRAM persists; static regions need no re-sends.
  Generalizes v6's fixed half-windows into per-field baked window lists
  (CASET/PASET per window, 16-line DMA blocks unchanged). Small moving
  regions can refresh far faster than 21.7 Hz (a 240×160 window = 23 ms of
  wire ≈ 43 Hz). Content-dependent: a full-screen pan (worst case)
  degenerates to today's field-interlace. Sources: LVGL dirty-rect refresh
  (https://lvgl.io/docs/open/main-modules/display/refreshing), Guenter et
  al. foveated 3D graphics (microsoft.com research), eDP panel self-refresh,
  CDTech SPI-TFT partial-update engineering notes.
- **Bayer/ordered dither before the R5G6B5 quantization — top quality-per-
  byte win.** ≈+2 perceived bits (kills the gradient banding users actually
  notice on small TFTs), zero runtime cost, and the repetitive patterns
  compress BETTER under RLE/LZSS. Error diffusion rejected for video
  (temporal shimmer, worse compression). (Wikipedia: ordered dithering,
  FRC; crystalfontz RGB565-vs-666 field reports.)
- **Offline cadence correction** — minterpolate to 21.7 fields/s or
  deliberate motion-blur blending; targets the dominant low-field-rate
  artifact (judder + the heavy-motion seam flicker the owner observed).
  Free (conversion only). (TFT Central / Blur Busters: BFI cannot fix
  stutter; interpolation/blur does.)
- **Motion-adaptive quantization + artifact mobility** — coarse quantize
  fast fields (temporal masking), finest on stills; alternate dither/chroma
  phase per field PAL-style so residual error averages instead of parking.
  Encoder-only. (x265 aq-mode docs; arXiv 2112.12284; PAL phase alternation.)
- **Blob-size levers (NOT fps):** G-full/R+B-half chroma packing (~30-50%
  smaller blob), HAM-style 2 B/px channel-delta packing (~33%), film-grain
  stripping offline. CORRECTION to one agent's conclusion: 2 B/px packing
  does NOT raise fps — the wire is fixed 3 B/px (RGB565/COLMOD 0x55 is
  parallel-only on the ILI9488, blank screen over SPI; established §12);
  these buy clip length in the 8 MB slot only.
- **Rejected, with reasons:** temporal dithering/FRC (pixel regions refresh
  ~10 Hz → visible ~5 Hz flicker); backlight strobing/BFI (21 Hz flicker,
  no TE line to phase against the panel scan; software BFI costs wire);
  runtime FRUC and MPEG-class decode (240 MHz budget — ESPFLIX needed
  MPEG1 at reduced res with everything else dropped); panel-side
  scaling/CLUT/PenTile sub-channels (ILI9488 has none; RGB666 serial writes
  are pixel-atomic).

Task B confirmations (same session):
- v6 runs at 94.9% of the 40 MHz wire ceiling (20.6 of 21.70 fields/s);
  the ~5% residual is per-transaction overhead + field ramp/drain. The
  decoder is 2-3× faster than the wire and streaming is zero-copy
  (decoder writes straight into DMA block buffers). No worthwhile headroom
  remains on this transport.
- **Real 60 MHz SPI is impossible, twice over.** (1) S3 GPSPI synthesizer
  (spi_ll_master_cal_clock in esp-idf): 80 MHz source, either undivided
  (requests > 60 MHz produce exactly 80) or ÷(pre×n) integers (pre 1-16,
  n 2-64) → representable clocks {80, 40, 26.7, 20, 16, ...}; nothing
  exists between 40 and 80 — 60 would need a 4/3 ratio. Matches the
  measured sweep: 35-60 MHz requests behave as 40 MHz, 65-80 as 80.
  (2) ILI9488 datasheet §17.4.3: twc = 50 ns min serial write cycle =
  20 MHz spec max — 40 MHz is already 2× over spec (works on this
  specimen), 80 is 4× (snow, measured).

Next: the prototype board stays on v6. The dirty-rectangle generalization
(v7) is the only remaining SPI-side fps project if wanted; the P4/DSI
board removes the wire constraint entirely and inherits the blob codec
plus everything above.
## 17. v7 (2026-11-15) — dirty-rect fields + line dither

Owner directive: implement the two research wins (dither + dirty-rect
scheduling), answer the live-animation question, and CONSIDER (no
implementation) "interlacing on steroids" — threading 3-4 frames together
with dirty-rect on all of them.

**Design (MIV3 blob, flags = bit2|bit3 = 12).** The ILI9488 GRAM persists,
so a 16-line block is only stored/sent if its wire bytes differ from the
previous field of the SAME half (field k-2). Each field starts with a
little-endian u32 block bitmap (bit b = block b present) followed by only
the present blocks' line data — clean blocks are not in the stream at all
(no decode-and-discard). The player merges present blocks into contiguous
RUNS; each run streams as one address window (CASET+PASET over the run's
line range — new `msp3520_prepare_half_run()`) + one RAMWR session of
16-line DMA blocks. A zero-bitmap field touches no SPI. Fields 0/1 are
fully present (nothing precedes them), so loop restarts and the `r` key
repaint the whole panel. The half for each field comes from
`video_field_index()` parity — robust across restarts, unlike a
player-side counter. MIV2 blobs still open: `video_next_field()` returns
an all-present bitmap and consumes nothing, so v6 blobs play byte-for-byte
like v6 (free A/B against the v6 package).

**The dither saga (three attempts, all measurable).**
1. Full 8x8 2D Bayer grain: best banding mask, but it breaks RLE runs down
   to ~2 px (RLE stream 41.56 MB vs v6's 12.99) — blob 8.57 MB, past the
   0x7F0000 app partition. Deeper LZSS chains cannot rescue periodic data
   (the one-period-back match is always found first), so this is a hard
   format limit, not a tuning issue.
2. Mid-band gating (dither only residuals in [step/4, 3step/4)): barely
   helped (40.33 MB) — this content's runs are naturally ~9 px, i.e. the
   breakage population is textured gradient, not near-level flats.
3. LINE dither (row-constant thresholds — first column of the Bayer
   matrix, 8-row period, full ±step/2 amplitude): run boundaries stay
   where the undithered level crossings are (just shifted per row), so
   the RLE structure of v6 survives — PAL-style per-line error averaging
   for vertical gradients, frame-stable, on-level rows never flip.
   RLE 12.76 MB (SMALLER than v6: the dirty omission beats the dither
   cost), LZ 5.81 MB, **blob 5,805,916 B (9.4x)**, app 6,037,264 B with
   27% of the partition free. This is what ships.

**Dirty stats on the Hill clip:** 3,300/3,460 blocks present (95.4%),
8 fields completely empty (the duplicate halves created by sampling a
15 fps source at 21.7 fields/s). Expected wire savings on THIS clip are
modest (~5% + the empty fields); the mechanism pays off on static-heavy
content, where fields collapse to bitmap-only. The live status line now
shows fields/s AND actual bus utilization ("xx.x% bus" = wire bits /
(elapsed x clock)) so the effect is visible while watching.

**Gates (all PASS).**
- Converter roundtrip: LZSS OK + a GRAM simulation that must equal the
  intended half content after EVERY field (proves dirty detection AND
  stream reassembly).
- Host gate: the exact device decoder (video.c, compiled with gcc) walks
  all 173 fields — 38,016,692 bytes byte-identical to the Python
  reference walk (sha256 6eca12f2d2303e05) — and the C dump replayed
  into a simulated panel equals the dithered source half after every
  field.
- v7 byte audit: driver patterns (MADCTL 0x28/COLMOD 0x66, no 0x55/0x48
  regressions), half-window CASET table, key set "ynrqd", embedded MIV3
  header (flags=12, fields=173), app under the partition limit, zips OK.
- Lesson recorded: the first host-gate draft compared the FINAL gram
  state against every field (only the last field per half could pass).
  Per-field comparison is the only correct shape for dirty schedules.
- QEMU: the post-recycle sandbox's qemu-xtensa (esp_develop_9.2.2_20260417)
  stalls inside msp3520_init() after the boot banner — for the v7 image
  AND for the hardware-validated v6 image booted from its package
  (verified side by side), so it is an emulator regression in this
  environment, not a firmware change. Boot-level checks pass under QEMU
  (banner, MIV3 header parsed from flash mmap, FATAL paths clear); the
  SPI-stage run is untestable here until a working qemu build returns.
  As with v6 (which shipped without a QEMU gate), the host gates above
  are the shipping authority.

**Live animations (owner question).** Correct for the baked clip: the
schedule is precomputed into the blob. But (a) on-device animation code
knows what it changed — the v7 run-streaming primitive (window + RAMWR +
blocks, `msp3520_prepare_half_run`) is exactly the API such a loop would
call, LVGL-style; (b) live VIDEO with on-the-fly dirty detection is
feasible if the board has PSRAM: keep the previous frame (460 KB), diff
per 16-line block, send only changed runs — the blob format already has
the vocabulary. That is a v8 candidate, not v7.

**"Interlacing on steroids" (N-way threading) — considered, not built.**
Threading N frames means splitting the field among N regions, each
carrying a different temporal sample. The arithmetic is unforgiving:
per-region refresh = 21.7/N Hz (N=3 -> 7.2 Hz, N=4 -> 5.4 Hz), below the
~10 Hz floor where each region starts strobing on motion; N-1 seams each
with a one-source-frame mismatch; and full-frame rate drops to 21.7/N.
With dirty-rect on top, the frees are real but only for static regions —
moving regions still need their full per-region rate. Verdict: N=2
(v6/v7's halves) is the sweet spot for full-screen coverage on this bus;
N>2 threading only becomes sane on the P4/DSI board where every region
can hold >= 15-20 Hz. The RIGHT steroids on S3/SPI is what v7 now does:
spend the fixed 21.7 Hz grid on what actually changed (content-adaptive
temporal weaving), rather than statically slicing the screen N ways.
## 18. (2026-11-15) The "1-bit I80" clock claim vs the S3 silicon

Owner question: a web-sourced (Google AI) suggestion claimed the
LCD_CAM peripheral in "1-bit I80 mode" could drive the SPI-wired
MSP3520 at 53.33 MHz (PLL_F160M/3) or 60 MHz (PLL_D2/4) on the existing
pins 9-14. Is it accurate, and are clocks between 40 and 80 MHz
possible at all? "If it is accurate, test it."

**Verdict per claim (all verified against the IDF v6.1 source that this
firmware is built with):**

1. "1-bit I80 mode" — DOES NOT EXIST. esp_lcd_new_i80_bus() accepts
   only bus_width 8 or 16 and rejects everything else
   (components/esp_lcd/i80/esp_lcd_panel_io_i80.c:129, comment:
   "LCD_CAM can support up to 24 data lines, we restrict users to only
   use 8 or 16 bit width"). Demonstrated at runtime: bus_width=1 and 2
   both return ESP_ERR_INVALID_ARG ("invalid bus width:1").
2. "WR=SCLK, D0=MOSI, CS/DC identical" — only meaningful if (1) were
   true; it is not. There is no serial LCD mode in the S3 LCD_CAM.
3. The clock arithmetic — WOULD be correct if (1) existed: LCD_CAM
   sources are PLL_F160M (160 MHz) and PLL_D2 (240 MHz)
   (soc/clk_tree_defs.h: LCD_CLK_SRC_PLL160M/PLL240M), and pclk =
   source / integer prescale 1..64 (lcd_ll.h, LCD_LL_PCLK_DIV_MAX=64),
   so 160/3 = 53.33 and 240/4 = 60 are real LCD_CAM clocks. The
   claimed enum names are slightly off (LCD_CLK_SRC_PLL160M exists;
   "LCD_CLK_SRC_PLL_D2" is actually LCD_CLK_SRC_PLL240M), and the
   snippet's bus config fields (psram_trans_align/sram_trans_align)
   do not exist in v6.1.
4. reverse_color_bits — REAL (esp_lcd_io_i80.h:85), as Google hedged.
5. "GPIO drive cap 0/1 + GND twist" — matches our own WORKLOG §13
   findings; that part was never in dispute.

**The actual question — clocks between 40 and 80 MHz on the GPSPI:
none exist, and none can be made.** The S3 SPI clock register is
`SCK = source / ((clkdiv_pre+1) * (clkcnt_n+1))` with integer divisors
only (spi_struct.h:58 documents exactly this), clkcnt_h/clkcnt_l set
the DUTY CYCLE only (spi_ll.h: spi_ll_freq_for_pre_n = fapb/(pre*n);
h = (duty*n+127)/256), requests above 3/4 of the source (> 60 MHz)
take the clk_equ_sysclk bypass (exactly 80), and the GPSPI's only
clock sources are APB (80 MHz) and XTAL (soc/clk_tree_defs.h) — the
160/240 MHz LCD PLLs are not SPI sources. The lattice is
{80, 40, 26.67, 20, 16, ...}. This also fully explains the project's
measured "35-60 requests run at 40, 65-80 at 80" from the v1-v6 sweeps
(now with the exact mechanism: requests <= 60 use the divider (best
integer divisor match), > 60 bypass to 80).

**Tested** (as requested): new firmware `msp3520-clock-probe` —
- Phase 1: asks the driver itself (spi_device_get_actual_freq) for a
  request sweep. Verified in QEMU (full table): 10->10.0, 15->16.0,
  20->20.0, 25/30->26.67, 35..60->40.0 (including 53.33->40 and
  60.000->40), 60.001..80->80.0. Plus the live i80 bus_width=1/2
  rejections.
- Phase 2 (needs real DMA, so hardware-only): applies each clock with
  a 1-byte transfer, dumps the live SPI2 clock register, and measures
  the actual bus clock by timing 10 x 16 KiB dummy transfers.
  Panel-safe: CS/DC/RST held high, the ILI9488 ignores everything.
- Bonus finding: this also located the QEMU stall from §17 — the
  probe boots, initializes the bus, adds devices and runs the i80
  checks in QEMU, then hangs on the FIRST SPI polling transaction.
  So the v6/v7 QEMU stall is the emulator's GPSPI DMA model, not our
  firmware's boot path.

Deliverables: download/msp3520-clock-probe/ (source, README) +
msp3520-clock-probe-firmware/ (flashable bundle + instructions), both
pushed. Bottom line for the project: 40 MHz remains the top sane clock
on this wiring; 80 MHz remains the only step above it (4x the panel's
spec, needs the §13 wiring levers). The 53.3/60 MHz dream dies here —
not on wiring or drive strength, but in the S3's clock tree.
## 19. (2026-11-15) video-lab: INTRO.mkv as a 15-technique testbed

Owner request: new test video (INTRO.mkv, DVD rip, higher res/frame rate
than the 15 fps game rip), cut to fit flash if needed; rewrite the
firmware as a technique testbed with monitor-selectable playback
(techniques 1-14 from the request, looping until another option is
chosen), in a new repo folder; 40 MHz is the established safe clock.

Sandbox note: this session's sandbox had rolled back to a near-fresh
state (spi-tests, tools, scripts, download all gone locally). Recovered
by re-cloning sh-clock/spi-tests (all §1-18 work was pushed), re-running
idf_setup.sh (recreated), and reinstalling cmake/ninja via pip.

Source: 720x480 anamorphic (SAR 8:9) MPEG-2, 29.97 fps, 12.0 s, AC3
(ignored). Content is letterboxed 16:9 inside the 4:3 frame (bars
47/51 px measured). Encoder chain: desqueeze to 640x480, crop bars,
center-crop to 540 px (3:2, pan-and-scan — no geometry distortion so
motion artifacts stay honest), scale to 480x320. Motion scan places the
cut at t=8.47 s: 6 calm frames then the scene burst at t=8.67 s — the
delta techniques get both regimes. 20 base frames.

Codec (all techniques share it): 8-bit indexed frames + ONE global
256-colour palette in RGB666 wire space (palette bytes ARE the wire
bytes; firmware expands index->3 bytes via a 768 B DRAM LUT), 2x2 Bayer
ordered dither, 3x3 median pre-filter (kills MPEG-2 noise; without it
the dither+palette boundaries fragment every row). Mean decode error
2.79/255. Six assets, 6.29 MB total (base 20f, pre_i4 6f x4 fields,
half 12f 240-wide, third 12f 160-wide, delta.spans 12f, palette).

Two encoder bugs found by the new host verifier (tools/verify_assets.py,
which decodes every blob exactly like the firmware):
1. A 15-bit colour-key cache added ±3.5/255 quantization slop (decode
   error 21.96/255!) — replaced with exact 24-bit keys (2.79/255).
2. Span deltas compared against the PREVIOUS frame let sub-tolerance
   drift compound (frame 3 off by >2x the tolerance). Now the encoder
   tracks the decoder's CANVAS and emits a pixel when its colour differs
   from the canvas by >10/255 — drift is bounded forever (verified:
   frame 0 exact, all later frames worst-case 8).

Firmware (new folder firmware/msp3520-video-lab/, ~235 KB code):
- msp3520.c extended with a queued-window session API (session_begin /
  qcaset / qpaset / qramwr / qdata / drain_to / session_end) + a
  wire-byte counter: CS stays LOW for a whole field while CASET/PASET/
  RAMWR and pixel data are queued as DMA transactions (pre_cb flips D/C
  per transaction). One window = 11 wire bytes (~0.8% of a row). This
  is the queued answer to the v1-v3 per-line polling mistake.
- 17 menu entries: 1/4/5/15 pure row interlace (ratio 4/8/3/2), 2/3
  static scanlines (black clear once; even active rows, line-drop), 6
  column 4:1 (1-px CASET windows; MADCTL already MV=1 so no register
  change needed), 7a/7b quincunx (half-row windows, half = (y+field)&1),
  8 macroblock 16x16 grid (run-grouped windows, full-frame fallback
  above 66% dirty), 9 span delta (sequential cursor walk of
  delta.spans), 10 LFSR (xorshift32 Fisher-Yates permutation, 80 rows
  per field, reshuffled per cycle), 11a/11b pixel doubling (240/160-wide
  sources stretched 2x/3x in the DMA buffer), 12 indexed full frame,
  13 pre-interlaced pack (sequential fields, wire-identical to 1), 14
  dual-core ping-pong (core 0 expands double buffers, core 1 queues to
  SPI, FIFO-correct retire ordering — compare against 12), plus 'c'
  (real lattice clocks 10/15/20/26.667/40) and 'i' (asset info).
- Every player loops until any key, printing fields/s + MB/s every 2 s
  and a final line on stop. Panel init is lazy (menu first — also
  QEMU-friendly up to the first SPI transaction).
- Triple line buffers + drain_to(8) keep two bursts in flight with
  guaranteed-retired buffer reuse; the ping-pong consumer retires BOTH
  in-flight blocks before CS goes high at field boundaries (a bug I
  caught in review: FIFO retire order vs buffer release).

Expected field rates at 40 MHz (RGB666, wire-limited): 57.6 KB -> ~60-70/s
measured (86 max), 115.2 KB -> ~35-40 (43 max), 153.6 KB -> ~27-30,
230.4 KB -> ~20, 460.8 KB -> ~10.5. Adaptive 8/9 swing live between
calm (8-16 KB/field) and burst (96-151 KB).

Verified in this session: clean build (6.53 MB app, 20% of the factory
partition free), all asset checks passed (headers, palette, pre_i4 ==
base rows, delta.spans round-trip within tolerance). Not verified:
on-panel behaviour (no hardware here — the project flashes and reports
its own stats on the real board; QEMU skipped since it cannot run SPI
transactions, see §18).

Deliverables: firmware/msp3520-video-lab/ (source + encoder + verifier
+ README, pushed), download/msp3520-video-lab/ (same, cleaned),
download/msp3520-video-lab-firmware/ (bootloader + partition table +
app + merged full image + FLASH-INSTRUCTIONS), both zipped.

## 20. (2026-11-16) The FULL clip on internal flash — VLB2 compressed base + runtime derivation

Owner verdict after living with the lab build: "static scanlines look
pretty good" — techniques 2/3 keep their look byte-for-byte. New
directive: fit the FULL video (or more of it) in; processing the source
is allowed; if it cannot be done, an SD card reader gets wired up.

### Measured before deciding (scripts/measure_full.py)

The 20-frame cut cost 6.29 MB because base.idx stored RAW indices
(153,600 B/frame) — 362 native frames would be 55.6 MB before the
derived assets even start. What the real content measures like under
the v6/v7 RLE+LZSS stack (calm window t=2-6 s, burst window t=8.5-12 s,
per-frame independent, 64 KiB window, median3 + full-clip palette):

  dither   calm avg   burst avg   err        full 29.97 fps   15 fps
  bayer2   53.8 K     71.2 K      2.35/255   22.12 MB       11.06 MB
  line8    43.6 K     59.7 K      2.46/255   18.19 MB        9.09 MB
  none     43.0 K     58.2 K      2.08/255   17.85 MB        8.92 MB

Two structural facts fell out of the same run:
- per-frame independent vs one sequential stream: 98% — the 64 KiB
  LZSS window cannot reach back a full 153.6 KiB frame anyway, so
  cross-frame matching buys almost nothing. Random frame access is
  essentially free: no keyframes, no rewind-on-loop.
- 32 KiB window instead of 64: +1.3% asset size, saves 2 x 32 KiB of
  DRAM (the main ring, static, plus the span technique's heap ring).
  Taken.

Verdict: NO dither mode fits 29.97 fps (needs ~2.9x more flash than
the 8 MB chip), and 15 fps misses the ~7.65 MB budget too. 10 fps with
the CURRENT look (2x2 Bayer) projects to ~7.4 MB and plays essentially
real-time — the wire only shows ~10.5 full frames/s at 40 MHz anyway.
DECISION: keep the codec identical (2x2 Bayer + median3 + global
256-colour palette rebuilt over the whole clip), bake at 10 fps.

### The v2 asset scheme (firmware/msp3520-video-lab)

- base.vlb2 "VLB2": ALL 122 frames (12.2 s at 10 fps = the whole
  12.045 s source), each frame INDEPENDENTLY RLE->LZSS compressed
  (2.78x on this content; per frame min 23K / avg 53K / max 86K),
  u32 offset table after the header for random access. 6,735,381 B.
- palette.pal: full-clip palette. Mean decode error 2.79/255 — the
  same number the 20-frame cut got; 256 colours travel well.
- merge2.bin 64 KiB: merge2[(a<<8)|b] = palette entry nearest to the
  average of colours a and b. Techniques 11a/11b build their 240/160-
  wide sources at runtime through it (two LUT steps for 3:1) — the
  old half.idx/third.idx assets are gone.
- mbmap.bin "VLM1": per-frame 16x16 macroblock dirty bitmaps (600
  bits/frame, frame 0 all-dirty) — technique 8 reads the bitmap
  instead of memcmp-ing two frames in RAM.
- labvid.c/h (new, pure C99 so it also compiles on the host): the
  VLB2 decoder = the v6/v7 video.c LZSS layer (struct-ified, 32 KiB
  ring) + the 1-byte-payload RLE layer, decoding on demand into ONE
  480x320 DRAM staging frame cached per frame index. Every technique
  reads the staged frame; the same decoder doubles as the previous-
  frame row iterator for technique 9 (safe because the staged frame
  is cached — the ring is free once staging is done).
- Technique changes (and only these): 9 span delta is computed LIVE
  (zero-tolerance diff vs the previous frame — tolerance 0 means the
  panel reconverges to the source every frame, so there is no drift
  to bound and no canvas to track); 13 reads rows p::4 of the staged
  frame (same wire shape as the prepacked blob); 8 is mbmap-driven;
  14's producer stages frames before expanding blocks; blk + pp_blk
  merged into one shared 2-block pool (players never run concurrently
  and the producer is done-gated before play_pingpong returns).
  2/3 scanlines, 1/4/5/6/7/10/11a/11b/12/15: wire-faithful, only the
  frame source changed (diffed hunk-by-hunk against the 55307db
  original before building).

### Verification (every gate green before shipping)

- tools/encode_lab.py: per-frame LZSS round-trip + RLE expansion ==
  the quantized frame; then the whole blob re-read from disk and
  re-decoded through the firmware walk. In-script, both directions.
- tools/verify_assets.py: headers, offsets monotonic, all 122 frames
  decode via the Python mirror, mbmap recomputed bit-for-bit from the
  decoded frames, merge2 recomputed from the palette (65536 entries),
  budget gate 6,810,845 / 7,650,000 B.
- tools/host_test_labvid.py (new): compiles main/labvid.c UNMODIFIED
  with gcc -Wall -Wextra, decodes the real blob, and compares per-
  frame FNV-1a hashes against the Python mirror — 122/122 match,
  combined hash, row-iterator frame-0 identity, bounds + cache
  re-stage. This gate caught two REAL bugs before they could ship:
  1. `z->head.ring` vs `z->ring` (a slip while splitting the v6/v7
     flat state into head + ring — instant compile error, the cheap
     kind).
  2. The stream base: blob offsets are relative to the END of the
     offset table, but the decoder read `blob + off` — so frame 0
     "decoded" the "VLB2" magic as LZSS (first token: match off
     19523 = 0x4C42+1 = "LB"). The host trace made it obvious in
     one run. (The harness itself had a third bug — an aliased
     staging-buffer hash — also fixed. None of the three could have
     been caught by the Python-only verifier: that was the point of
     compiling the device code.)
- IDF v6.1 build: clean, 7,044,256 B of the 8,323,072 B factory
  partition (84.6% used). DRAM: 127,199 B of statics leave a 214,561 B
  heap pool — the 153,600 B staging frame is heap-allocated at boot
  (first build had it static and the link overflowed dram0_0_seg by
  64,832 B), leaving ~61 KiB for task stacks and the ping-pong
  producer/consumer. Getting there needed one more buffer change:
  play_macro's run[3][23040] (69 KB) was merged into the shared
  s_blk[2] pool with drain_to(4) — provably safe because one window
  queues exactly 4 transactions (3 command + 1 data) and TX_RING_SLOTS
  is 16, so live+queued stays <= 8 <= 16 and drain_to(4) retires the
  2-back window's transactions before its buffer is refilled (the
  same arithmetic that makes the row path's 3 buffers + drain_to(8)
  safe: reuse distance 3 windows = 12 transactions > 8). The
  technique-9 iterator reuses the main decoder instead of a second
  heap instance — no interleaving exists in play_spans.

### SD-card verdict (the owner's question)

The FULL video fits — every technique now plays all 12.2 s (122
frames @ 10 fps, ~real-time on the wire) with the codec look you have
already seen on the panel. No SD card needed for this clip. What does
NOT fit on 8 MB flash: full 29.97 fps (17.9-22.1 MB depending on
dither) or a library of clips (~5.6 MB/min at this quality). When the
SD reader does get wired, the same VLB2 decoder streams from a file
unchanged — only the blob source (flash mmap -> FATFS read) moves.

## 21. (2026-11-16) Post-recycle recovery: reflash needed, not rework

Owner reported "the current one only has less than 3 secs of footage"
after the WORKLOG 20 delivery. Diagnosis: the repo state was correct
(base.vlb2 = 122 frames, 6,735,381 B — verified by header parse and
verify_assets.py), but the board was still running the WORKLOG 19
build (20 frames @ 10 fps = 2.0 s loop) — the full-clip package had
never been flashed. No code or asset work was needed.

Environment recovery (sandbox recycled again — 8th time):
- .idf/ toolchain, firmware build/, download/ all wiped; re-ran
  scripts/idf_setup.sh (ESP-IDF v6.1 clone + esp32s3 tools, ~10 min).
- Git divergence: the owner had manually uploaded INTRO.mkv to
  assets/ via the GitHub web UI (6e5ea18 "Add files via upload",
  sha256-identical to upload/INTRO.mkv). Local main rebased onto it
  and pushed as 96cc9fc. .gitignore now excludes .idf_setup*.log
  (the previous session's auto-commit had tracked the setup log).

Verification re-run (nothing trusted from memory):
- verify_assets.py: ALL CHECKS PASSED (122/122 frames decode, mbmap
  bit-exact, merge2 recomputed, budget 6,810,845 / 7,650,000 B).
- host_test_labvid.py: HOST GATE PASSED (device labvid.c compiled
  unmodified with gcc, 122/122 per-frame hashes match the Python
  mirror, combined hash eda25db4).
- Clean rebuild: app 7,044,256 B (byte-identical size to the WORKLOG
  20 build), embedded VLB2 header at 0xBA24 reads 480x320, 122
  frames. Driver audit at source level: LCD_MADCTL 0x28, init-table
  COLMOD 0x66, LCD_BPP 3. (Raw binary byte-pattern counts are
  meaningless on a 7 MB compressed image — the 0x55/0x36 hits were
  coincidental matches inside video data; audited the source instead.)
- Packages regenerated: download/msp3520-video-lab/ (cleaned source)
  + msp3520-video-lab-firmware/ (bootloader, partition table, 7.04 MB
  app, 7.11 MB merged full image) + both zips. Zero __pycache__.

Action for the owner: flash msp3520_video_lab_full.bin (the merged
image) at 0x0 — see FLASH-INSTRUCTIONS.txt in the firmware package.
Every technique then plays the full 12.2 s clip in a loop; the <3 s
loop disappears. 84% of the factory partition used; 29.97 fps full
rate still does not fit (that needs SD, per §20).

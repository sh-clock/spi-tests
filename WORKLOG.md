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

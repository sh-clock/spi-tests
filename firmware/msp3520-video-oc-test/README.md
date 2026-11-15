# MSP3520 Video Player + SPI Clock Sweep — ESP32-S3 + ESP-IDF (v7)

The second MSP3520 test program: it plays **your video** (`Hill[729].mp4`,
converted and embedded — 119 source frames, 15 fps, 7.9 s → 173 fields)
fullscreen on the 480×320 display at every SPI clock from **10 MHz to
80 MHz** in 5 MHz steps, and records your pass/fail verdict per frequency.
v7 plays **field-interlaced with a DIRTY SCHEDULE and line dither**: every
field is one 240-px half of the screen carrying a **fresh source sample**
(motion advances at **~21.7 steps/s at 40 MHz**), but a field now carries
**only the 16-line blocks that changed** vs the previous field of the same
half — the ILI9488's GRAM keeps the rest, so wire bytes follow motion and
static scenes leave the bus idle. Pixel data is dithered offline
(row-constant Bayer thresholds) so gradient banding becomes fine
frame-stable texture. The interlace trade (a stationary mid-screen seam,
~46 ms shear on fast pans) is unchanged from v6; see §1.

```
msp3520-video-oc-test/
├── CMakeLists.txt        ← project file
├── partitions.csv        ← custom partition table (one big 8 MB app slot)
├── sdkconfig.defaults    ← 8 MB flash, console, 240 MHz CPU
├── main/
│   ├── CMakeLists.txt    ← embeds video.bin into the firmware image
│   ├── video.bin         ← THE CONVERTED VIDEO (~5.8 MB, generated)
│   ├── video.h/.c        ← tiny streaming decoder (LZSS + RLE + bitmaps)
│   ├── msp3520.h/.c      ← display driver, block/run-streaming edition
│   └── main.c            ← sweep, dirty-run pusher, pacing, verdicts
└── tools/                ← converter + host-side verification (repo copy)
```

**Wiring is identical to the first project** (same pins, same COM/UART USB
port, same build/flash procedure — see that project's README for the
step-by-step install guide if you haven't set up ESP-IDF yet). Quick table:

| MSP3520 pin | ESP32-S3 GPIO | MSP3520 pin | ESP32-S3 GPIO |
|---|---|---|---|
| 1 VCC | 5V | 6 SDI (MOSI) | GPIO11 |
| 2 GND | GND | 7 SCK | GPIO12 |
| 3 CS | GPIO10 | 8 LED (backlight) | GPIO47 |
| 4 RESET | GPIO14 | 9 SDO (MISO) | GPIO13 |
| 5 DC/RS | GPIO9 | 10–14 (touch) | not connected |

**One hardware requirement specific to this project:** the video lives
inside the firmware image, so the app is ~6.0 MB. The standard
ESP32-S3-DevKitC-1 has 8 MB of flash (N8/N8R2 and N16 modules all work).
That is the only requirement — v7 needs **no PSRAM** (fields stream
straight from the decoder to the wire; no full frame is ever staged).

---

## 1. Field-interlaced playback with a dirty schedule — the honest physics

A full RGB666 frame is 480×320×3 = 460,800 bytes over the wire. The 40 MHz
bus (the panel's practical ceiling — see the sweep) therefore tops out at
**~10.85 full frames/s**, whatever the firmware does. A half-frame is
230,400 bytes, so the same bus carries **~21.7 fields/s**.

**v6 (field-native content):** the blob itself is baked as fields. Field
*k* is one 240-px half (even *k* = left, odd = right) of the source frame
at time *k*/21.70 s — every field carries a **fresh temporal sample**,
one field period after the previous one. Result at 40 MHz: **~21.7
fields/s, motion advancing at double cadence** vs the progressive builds.

**v7 (dirty schedule):** the ILI9488 is a frame store — whatever a field
wrote stays on screen until overwritten. v7 exploits that: the converter
compares each field's 16-line blocks against the previous field **of the
same half** (field *k*-2) and stores/sends only the blocks whose wire
bytes differ. Each field in the blob starts with a u32 block bitmap
followed by the present blocks' data; the player groups present blocks
into contiguous **runs**, and each run streams as one address window
(CASET+PASET) + one RAMWR session of 16-line DMA blocks. A field where
nothing changed carries bitmap 0 — no SPI at all, just pacing. On the
Hill clip (nearly everything moves) 3,300 of 3,460 blocks are present
(95.4%) and 8 fields are empty; on static-heavy content the savings grow
fast. The status line shows the live **bus utilization** so you can watch
the wire go quiet when the scene holds still.

The trade (the classic interlace one — accepted since v6):

* Each field covers half the screen; the other half is one field period
  (46 ms) older. There is a **stationary vertical seam at mid-screen**,
  and during fast horizontal pans the two sides shear by ~46 ms of motion
  across that seam.
* Per-pixel update rate is unchanged (~10.85 Hz at 40 MHz) — what doubles
  is the *global* motion cadence: something on screen advances every
  46 ms instead of every 92 ms.
* The 15 fps source only has 15 unique samples/s: 173 fields sample the
  119 frames, so v7 displays ~15 unique temporal samples/s vs
  progressive's ~10.85 (+38%) at a 2× display cadence — not a full 2× of
  *new* information (that would need a ≥21.7 fps source; convert any such
  clip with the same command and v7 plays it at the full 21.7).

(Historical note: v1–v3 used ROW interlacing — even/odd rows — which
combed on every moving edge and needed 4 SPI transactions per line; v4
went progressive; v5 tried same-frame column halves as a cadence A/B; v6
baked the column split into the content itself; v7 adds the dirty
schedule and dither on top of v6's layout. True even/odd *row* fields
would still need one address window per line — 1280 transactions/frame —
which is why the wire-cheap realization of interlacing on the ILI9488 is
the half-*window* split.)

## 2. How the video got into the firmware

You gave me `Hill[729].mp4` (320×240, 15 fps, 119 frames, 7.9 s, h264+aac).
The conversion pipeline (all steps verified by an in-script roundtrip
assert — and for the dirty layout by a full GRAM simulation that must
equal the intended half content after **every** field):

1. **Crop + scale.** The source has 16-row black bars top and bottom (the
   real content is 320×208, aspect 1.54 vs the display's 1.5). The bars are
   cropped and the content stretched to 480×320 — true fullscreen with a
   2.6 % stretch you cannot see. (Audio is dropped: no speaker on this
   setup.)
2. **Median filter (3×3).** The h264 encode leaves dithering noise that
   wrecks compression; the filter removes it with no visible loss on this
   animation-style content.
3. **RGB666 quantization + line dither.** Colors become the 3-bytes-per-
   pixel format the ILI9488 takes over SPI, with R/G/B kept to 5/6/5
   significant bits (the low bits of each wire byte are zero). On top of
   that, `--dither` applies **row-constant Bayer thresholds** (8-row
   period, first column of the 8×8 matrix, amplitude ±½ quantization
   step): each display line gets one constant offset, so RLE runs survive
   exactly as without dither, while vertical gradients get PAL-style
   per-line error averaging that breaks contour banding into fine
   frame-stable texture. (A full 8×8 2-D grain was tried first — it masks
   banding slightly better but breaks RLE runs down to ~2 px and inflated
   the blob to 8.57 MB, past the 8 MB flash slot; see the repo WORKLOG
   §17. Rows sitting exactly on a quantization level never flip.)
4. **Dirty field-halves scan layout** (`--scan field-halves --dirty`, the
   shipped blob's mode since v7): each field is one 240-px half of the
   source frame at its own timestamp, alternating left/right at the field
   rate (`--field-rate`, default 21.70 = the 40 MHz wire ceiling; keep it
   in sync with `FIELD_HZ_X100` in `main.c`). A 16-line block is stored
   only if it differs from the same half's previous field; each field
   starts with its u32 block bitmap. Fields 0/1 are fully present (nothing
   precedes them), so a loop restart repaints the whole panel.
   (`--scan progressive` and the legacy `--scan interlaced` remain; the
   plain v6 `--scan field-halves` without `--dirty` also still works — the
   player streams such MIV2 blobs exactly like v6, all blocks present.)
5. **Per-line RLE.** Runs of `[count-1][R][G][B]`, counts summing to 240
   per half-line.
6. **LZSS.** A small home-grown LZ (64 KiB window, 3-byte match tokens)
   squeezes repetition across lines. No zlib/deflate on the device needed —
   the decoder is ~200 lines of C you can read.

Result: 39,859,200 bytes of raw pixel data → **5,805,916 bytes (9.4×)**,
embedded into the firmware and flashed with `idf.py flash` — no extra
upload step. The dirty schedule itself is verified twice: the converter's
GRAM simulation, and on the build machine the exact `video.c` that runs
on the ESP32-S3 decodes every field of the blob while a second GRAM
simulation confirms the panel state equals the dithered source half after
each of the 173 fields (`tools/host_test_video.py`).

To embed a different video (any resolution/aspect/fps up to a few tens of
seconds):

```
python3 tools/convert_video.py YOURCLIP.mp4 main/video.bin --scan field-halves --dither --dirty
```

`convert_video.py --help` documents every knob (crop, fps re-timing,
field rate, dither on/off, dirty on/off). Keep the output under ~7.5 MB
(the script prints the size and verifies the roundtrip every time). Then
rebuild and re-flash. Requirements: Python 3 with `numpy` and `Pillow`,
plus a working `ffmpeg`.

> The converter in `tools/` is the current one (MIV2 + MIV3, all scan
> layouts, dither + dirty). The shipped `video.bin` is MIV3 (flags=12 =
> field-halves | dirty); a progressive or legacy blob is rejected at boot
> with a clear message.

---

## 3. Build, flash, run

Same three steps as the first project (VSCode + ESP-IDF extension, or
`idf.py set-target esp32s3 && idf.py build flash monitor`):

1. Open this folder, set the target to **esp32s3**.
2. Plug the USB cable into the **COM/UART** port, select that serial port.
3. **Build → Flash → Monitor.** First build takes a few minutes; flashing
   writes a ~6 MB image — that is the video going in.

The console session looks like this:

```
  ============================================================
   MSP3520 VIDEO player + SPI clock sweep (v7, DIRTY-RECT)
   ESP32-S3 + ESP-IDF  |  sweep 10..80 MHz in 5 MHz steps
  ============================================================

  Embedded video: 119 source frames @ 15 fps -> 173 fields
  (one field = one 240-px half of a FRESH source frame),
  stored in 5670 KiB (9.4x smaller than the raw 38925 KiB of pixel data)
  ...
  [ 1/15] 10 MHz
  playing: 45 fields, 8.9 fields/s, 97.0% bus   [y/n/r/q/d]  ← every 0.5 s
   -> clean? [y]es [n]o [r]eplay [q]uit:
```

**Keys work DURING playback** (single keypress, no Enter):

| key | meaning |
|---|---|
| `y` | clean — PASS this frequency |
| `n` | artifacts — FAIL this frequency |
| `r` | restart the video at this frequency |
| `q` | quit the sweep |
| `d` | cycle SCK/MOSI GPIO drive capability 0→1→2→3 (80 MHz experiment) |

Each frequency plays the video on loop for 10 s; if you don't press
anything you get the same question at the end. The measured fields/s and
the **bus utilization** (share of the SPI clock actually carrying pixels)
are printed live and go into the summary:

```
  ======================== SUMMARY ========================
   MHz  fields/s  verdict
  --------------------------------------------------------
   10     8.9    PASS
   40    20.6    PASS
   60    20.6    PASS      ← 35-60 all run at 40 MHz actual
   80    16.4    FAIL      ← snow; 80 is 4x the panel's spec
  ========================================================
  Highest CLEAN frequency : 40 MHz  (…)
  Suggested for projects  : 35 MHz or lower
```

Reading the two live numbers:

* **fields/s** is the display cadence (paced to 21.70 when the bus keeps
  up). At low clocks it is bus-limited — and with the dirty schedule it
  is *content*-limited too: fewer changed blocks per field = less wire
  per field = higher cadence on the same clock. A sudden *drop* compared
  to the previous step is itself a sign of corruption — dropped bytes
  cost both picture quality and throughput.
* **% bus** is wire bits / (elapsed × clock). Near 100% = every field is
  full-size (everything moving). It falls when the scene holds still —
  that is the dirty schedule skipping unchanged blocks, on purpose.

After the sweep you can re-test any frequency (type `63` + Enter, or just
Enter to skip), and finally there is a **continuous playback mode**: type a
frequency and the video loops at it until you press a key — the "enjoy
your work" mode.

## 4. What corruption looks like on moving video

* `n` verdicts: **snow/noise** in flat areas (sky), **torn horizontal
  stripes** (a line's pixels shifted sideways), **color speckles**, a
  **frozen picture** while the fps counter stalls, large **garbage
  blocks**. With the dirty schedule, also: **stale rectangles** (a region
  showing an older frame than the rest — that means a skipped block was
  NOT actually unchanged; the GRAM gates make this a converter bug, not
  an overclock symptom, but it would show up here).
* Normal, *not* failures: slower playback at low clocks; a one-field
  "tear" exactly when the video loops back to field 0 (the loop restarts
  mid-scroll, every ~8.0 s); the stationary mid-screen seam and the
  ~46 ms shear across it during fast pans (§1); the fine horizontal
  dither texture in gradients (row-constant by design — look closely at
  the sky, it should be stable, not shimmering).

Moving video is a much harsher test than static bars: minutes of
continuous streaming with every byte checked by your eyes. A display that
passes here will pass anything you throw at it later.

## 5. Troubleshooting

| Symptom | Fix |
|---|---|
| Build error: `binary too large` / partition overflow | The 8 MB flash settings didn't apply: delete the generated `sdkconfig` (keep `sdkconfig.defaults`!) and build again; make sure `partitions.csv` is in the project root. |
| Flashes, then boot loops / `invalid header` | Flash size mismatch: `idf.py erase-flash`, then rebuild+reflash with `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` (set in `sdkconfig.defaults`). |
| `FATAL: embedded video.bin failed to open` | `main/video.bin` missing or not a converter output — regenerate with `tools/convert_video.py`. |
| `FATAL: v7 streams FIELD-INTERLACED blobs (flags bit2)` | `video.bin` is a progressive/legacy blob — regenerate with `--scan field-halves` (add `--dither --dirty` for the v7 format). |
| Boot screen (dark blue) never appears | Wiring: see the first project's troubleshooting table (LED/backlight, DC/RESET, SCK/MOSI). |
| Video plays but rows look shuffled / halves swapped | That would be a driver bug, not overclocking — re-check the wiring first (SCK and MOSI are adjacent on both headers and easy to swap). |
| fps stuck far below the table values at high MHz | Signal quality: shorter wires, reseat jumpers; at the 65–80 steps also try the `d` key + a 47–100 Ω series resistor on SCK. |
| Seam/shear visible during motion | Expected (§1) — the accepted interlace trade. Static content is pixel-identical to progressive. |
| Fine horizontal stripes in gradients | That is the line dither (§2 step 3) — frame-stable texture, not a fault. It replaced the contour banding of v6. |
| Video loops with a visible jump | Normal: the clip is 7.9 s and loops. |

## 6. Code walkthrough (what changed vs v6)

**`video.c`** — the decoder, ~200 lines. `video_next_field()` starts a
field: for MIV3 blobs it pulls the u32 block bitmap from the RLE layer
(bit b = 16-line block b of this half is present); for MIV2 blobs it
returns an all-present bitmap and consumes nothing (v6 blobs play
unchanged). `video_read_line()` pulls one display line through two
layers: LZSS (64 KiB ring buffer, literal/match tokens — the format is
documented in `video.h`) and per-line RLE (`[count-1][R][G][B]` runs).
Clean blocks are simply absent from the stream — nothing is skipped at
runtime, the data was never there. Peak RAM: ~67 KiB, regardless of
video length; the decoder is strictly forward-only (no frame seek).

**`msp3520.c`** — same pins, panel init and reset as before. The push
path drives the D/C line from the SPI driver's `pre_cb` (the same
mechanism the official `esp_lcd` component uses), which makes **queued,
interrupt-driven transactions** possible. `msp3520_queue_half_block()`
submits one 16-line (11,520 B) pure-data DMA transaction; with two blocks
in flight, the CPU decodes the next block while DMA pushes the previous
one. v7 adds `msp3520_prepare_half_run(half, y0, y1)` — the half-window
addressing narrowed to a line range, so one contiguous run of dirty
blocks streams as a single RAMWR session (the row pointer wraps inside
the window exactly as for full halves; ILI9488 datasheet §5.2.24/§6.3).
The old full-height `msp3520_prepare_half_stream()` is now a wrapper.

**`main.c`** — the sweep logic. `push_field(half, bitmap)` walks the
bitmap, merges present blocks into runs, and streams each run
(window → CS low + RAMWR → 16-line DMA blocks → retire → CS high);
buffers alternate by queue order, not block index. A zero bitmap returns
without touching SPI. `play_segment()` paces fields to 21.70/s when the
bus is fast (matching the converter's bake rate), polls your keys
non-blocking, and measures fields/s plus actual wire bytes (the live
`% bus` figure). The half for each field comes from
`video_field_index()` parity, which stays correct across `r` restarts
and loop wraps. The verdict/summary/re-test flow matches the earlier
builds so both consoles feel the same.

## 7. Going further

* Convert your own clips (`tools/convert_video.py`, §2) — keep them under
  ~7.5 MB; static-heavy clips get much cheaper with `--dirty`.
* Try `PLAY_SECONDS` and the `FREQ_*` defines in `main.c`.
* The `d` key + wiring levers (series-R on SCK, GND alongside, short
  wires) are the complete remaining $0 experiment space for 80 MHz —
  see the repo WORKLOG §12/§13 for the research behind it.
* The dirty schedule is the embedded realization of LVGL-style partial
  refresh, baked offline; the same run-streaming primitive
  (window + RAMWR + blocks) is what an on-device animation loop would
  call directly. For live video with on-the-fly dirty detection you
  would keep the previous frame in PSRAM and diff blocks in RAM — the
  blob format already has the vocabulary (bitmaps + runs).
* For real GUIs at your measured max clock, look at `esp_lcd` + LVGL
  next; the queued-push pattern in `msp3520.c` is a hand-rolled taste of
  what `esp_lcd` does for you.

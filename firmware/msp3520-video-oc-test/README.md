# MSP3520 Video Player + SPI Clock Sweep — ESP32-S3 + ESP-IDF (v5)

The second MSP3520 test program: it plays **your video** (`Hill[729].mp4`,
converted and embedded — 119 frames, 15 fps, 7.9 s) fullscreen on the
480×320 display at every SPI clock from **10 MHz to 80 MHz** in 5 MHz
steps, and records your pass/fail verdict per frequency. Default playback
is **progressive** (one full-frame update per frame); a live **`i` key
toggles column-interlaced mode** — a cadence experiment, see §1.

```
msp3520-video-oc-test/
├── CMakeLists.txt        ← project file
├── partitions.csv        ← custom partition table (one big 8 MB app slot)
├── sdkconfig.defaults    ← 8 MB flash, console, 240 MHz CPU
├── main/
│   ├── CMakeLists.txt    ← embeds video.bin into the firmware image
│   ├── video.bin         ← THE CONVERTED VIDEO (~7.1 MB, generated)
│   ├── video.h/.c        ← tiny streaming decoder (LZSS + RLE)
│   ├── msp3520.h/.c      ← display driver, block-streaming edition
│   └── main.c            ← sweep, playback pacing, verdicts, summary
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
inside the firmware image, so the app is ~7.5 MB. The standard
ESP32-S3-DevKitC-1 has 8 MB of flash (N8/N8R2 and N16 modules all work).
**The `i` (interlace) mode additionally needs PSRAM** for its frame stage
(N8R2/N8R8 class modules). PSRAM is **off in the default build** (so the
sweep runs on every board); to use `i`, enable it once via
`idf.py menuconfig` → Component config → ESP PSRAM (quad mode for N8R2,
octal for N8R8), then rebuild. Without it, `i` prints
`interlace unavailable` and playback stays progressive.

---

## 1. Progressive vs the `i` experiment — the honest physics

A full frame is 480×320×3 bytes = 460,800 bytes over the wire. At 15 fps
that is 55.3 Mbit/s; the 40 MHz bus (the panel's practical ceiling — see
the sweep) carries 3.6864 Mbit per frame → **~10.8 fps is the hard
ceiling** for fullscreen RGB666, whatever the firmware does.

**Default (progressive):** each frame is ONE full-screen update — the
address window is re-asserted, then one RAMWR + 20 pure-data DMA blocks of
16 lines (25 SPI transactions per frame; v3 needed 1280). ~10.5 fps
measured at 40 MHz. No interlacing artifacts of any kind.

**Press `i` (column-interlace):** each frame goes out as TWO half-screen
updates — the left 240-px half, then the right half ~46 ms later. Each
half is a contiguous address window, so it streams at full efficiency:
**~21 updates/s** at 40 MHz. The status line shows both numbers, e.g.
`10.3 fps (20.6 upd/s)`.

What this can and cannot do (research-verified — four independent
literature sweeps, summarised in the repo WORKLOG §14):

* **CAN:** double the *update cadence* (21 regular ~46 ms updates/s
  instead of ~10.5 irregular full-frame steps). Whether that *looks*
  smoother is content-dependent and a matter of taste — this build exists
  so you can A/B it with your own eyes.
* **CANNOT:** raise the frame rate. Interlacing halves the bytes per
  *field*, never the bytes per *picture* — no broadcast system ever
  gained unique pictures/s from it (480i60 = 30 pictures/s, 576i50 = 25).
  Its temporal gain only ever applied to field-native sources (50i/60i
  cameras); this video is 15 fps *progressive*, so there is no extra
  motion information to send. Every pixel still updates at ~10.85 Hz.
* **Cost:** a stationary vertical seam at mid-screen; during fast
  horizontal pans the two sides can be ~46 ms of motion apart (shear at
  the seam). Static scenes: identical to progressive.

If the interlaced mode looks worse to you, leave it off — nothing else
changed. (Historical note: v1–v3 of this project used ROW interlacing —
even/odd rows — which combed on every moving edge and needed 4 SPI
transactions per line. v4 removed it; v5's column version is a different,
cheaper variant: contiguous windows, one seam instead of combing.)

## 2. How the video got into the firmware

You gave me `Hill[729].mp4` (320×240, 15 fps, 119 frames, 7.9 s, h264+aac).
The conversion pipeline (all steps verified by an in-script roundtrip
assert — every decoded line must equal the source pixels exactly):

1. **Crop + scale.** The source has 16-row black bars top and bottom (the
   real content is 320×208, aspect 1.54 vs the display's 1.5). The bars are
   cropped and the content stretched to 480×320 — true fullscreen with a
   2.6 % stretch you cannot see. (Audio is dropped: no speaker on this
   setup.)
2. **Median filter (3×3).** The h264 encode leaves dithering noise that
   wrecks compression; the filter removes it with no visible loss on this
   animation-style content.
3. **RGB666 + green-LSB drop.** Colors become the 3-bytes-per-pixel format
   the ILI9488 takes over SPI. Dropping the green LSB (effective RGB555)
   roughly doubled compression; on an 18-bit panel showing this content it
   is not visible.
4. **Progressive scan layout** (`--scan progressive`, the default since
   v4): plain frame-after-frame line order. (The legacy `--scan interlaced`
   even/odd-field layout of v1–v3 still exists for compatibility.)
5. **Per-line RLE.** Runs of `[count-1][R][G][B]`, counts summing to 480
   per display line.
6. **LZSS.** A small home-grown LZ (64 KiB window, 3-byte match tokens)
   squeezes repetition across lines. No zlib/deflate on the device needed —
   the decoder is ~150 lines of C you can read.

Result: 54,835,200 bytes of raw pixels → **7,254,058 bytes (7.6×)**,
embedded into the firmware and flashed with `idf.py flash` — no extra
upload step. The decoder was verified on the build machine: the exact
`video.c` that runs on the ESP32-S3 decodes all 38,080 display lines
byte-identical to the reference expansion (`tools/host_test_video.py`).

To embed a different video (any resolution/aspect/fps up to a few tens of
seconds):

```
python3 tools/convert_video.py YOURCLIP.mp4 main/video.bin --drop-green-lsb
```

`convert_video.py --help` documents every knob (crop, fps re-timing,
filters). Keep the output under ~7 MB (the script prints the size and
verifies the roundtrip every time). Then rebuild and re-flash.
Requirements: Python 3 with `numpy` and `Pillow`, plus a working `ffmpeg`.

> **Repo status note:** the converter copy in this repo is still the
> v1–v3 version (magic `MIV1`, even/odd field layout) — it produces
> blobs this player **rejects** by design. The shipped `video.bin` is
> already the progressive `MIV2` format. Porting the progressive
> converter into the repo is pending; ask, and it will be finished.

---

## 3. Build, flash, run

Same three steps as the first project (VSCode + ESP-IDF extension, or
`idf.py set-target esp32s3 && idf.py build flash monitor`):

1. Open this folder, set the target to **esp32s3**.
2. Plug the USB cable into the **COM/UART** port, select that serial port.
3. **Build → Flash → Monitor.** First build takes a few minutes; flashing
   writes a ~7.5 MB image — that is the video going in.

The console session looks like this:

```
  ============================================================
   MSP3520 VIDEO player + SPI clock sweep (v5)
   ESP32-S3 + ESP-IDF  |  sweep 10..80 MHz in 5 MHz steps
  ============================================================

  Embedded video: 119 frames @ 15 fps (7.9 s), 119 fields,
  stored in 7084 KiB (7.6x smaller than the raw 53549 KiB of pixel data)

  Wire format: 3 bytes/pixel (COLMOD 0x66). S3 clock
  reality: 35-60 MHz requests run at 40 MHz, 65-80 MHz at
  80 MHz (no other steps exist).
  ...
  [ 1/15] 10 MHz
  playing: 24 frames, 2.7 fps   [y/n/r/q/d/i]      ← updates every 0.5 s
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
| `i` | toggle column-interlace on/off (§1; takes effect next frame) |

Each frequency plays the video on loop for 10 s; if you don't press
anything you get the same question at the end. The measured fps is printed
live and goes into the summary:

```
  ======================== SUMMARY ========================
   MHz      fps     verdict
  --------------------------------------------------------
   10     2.7    PASS
   40    10.5    PASS
   60    10.5    PASS      ← 35-60 all run at 40 MHz actual
   80     8.2    FAIL      ← snow; 80 is 4x the panel's spec
  ========================================================
  Highest CLEAN frequency : 40 MHz  (…)
  Suggested for projects  : 35 MHz or lower
```

Reading the fps column:

* **~10.5** = bus-limited at 40 MHz actual — that IS the RGB666 wire
  ceiling (2.7 fps at 10 MHz, 5.4 at 20, 8.1 at 30). A sudden *drop*
  compared to the previous step is itself a sign of corruption — dropped
  bytes cost both picture quality and throughput.
* With `i` on, the same ceiling applies — the counter then also shows the
  update rate (`10.3 fps (20.6 upd/s)`).

After the sweep you can re-test any frequency (type `63` + Enter, or just
Enter to skip), and finally there is a **continuous playback mode**: type a
frequency and the video loops at it until you press a key — the "enjoy
your work" mode.

## 4. What corruption looks like on moving video

* `n` verdicts: **snow/noise** in flat areas (sky), **torn horizontal
  stripes** (a line's pixels shifted sideways), **color speckles**, a
  **frozen picture** while the fps counter stalls, large **garbage
  blocks**.
* Normal, *not* failures: slower playback at low clocks; a one-frame
  "tear" exactly when the video loops back to frame 0 (the loop restarts
  mid-scroll, every 7.9 s); in `i` mode only — the mid-screen seam and
  the ~46 ms shear across it during fast pans (§1).

Moving video is a much harsher test than static bars: minutes of
continuous streaming with every byte checked by your eyes. A display that
passes here will pass anything you throw at it later.

## 5. Troubleshooting

| Symptom | Fix |
|---|---|
| Build error: `binary too large` / partition overflow | The 8 MB flash settings didn't apply: delete the generated `sdkconfig` (keep `sdkconfig.defaults`!) and build again; make sure `partitions.csv` is in the project root. |
| Flashes, then boot loops / `invalid header` | Flash size mismatch: `idf.py erase-flash`, then rebuild+reflash with `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` (set in `sdkconfig.defaults`). |
| `FATAL: embedded video.bin failed to open` | `main/video.bin` missing or not a converter output — regenerate with `tools/convert_video.py`. |
| Boot screen (dark blue) never appears | Wiring: see the first project's troubleshooting table (LED/backlight, DC/RESET, SCK/MOSI). |
| Video plays but rows look shuffled / halves swapped | That would be a driver bug, not overclocking — re-check the wiring first (SCK and MOSI are adjacent on both headers and easy to swap). |
| fps stuck far below the table values at high MHz | Signal quality: shorter wires, reseat jumpers; at the 65–80 steps also try the `d` key + a 47–100 Ω series resistor on SCK. |
| `[interlace unavailable: no memory for the frame stage]` | Either the module has no PSRAM (N8), or PSRAM is not enabled in the build — the default build ships with it OFF so the sweep works on every board. Menuconfig → Component config → ESP PSRAM, then rebuild (§1). |
| Seam/shear visible in `i` mode | Expected during fast motion (§1); press `i` again to return to progressive. |
| Video loops with a visible jump | Normal: the clip is 7.9 s and loops. |

## 6. Code walkthrough (what changed vs the first project)

**`video.c`** — the decoder, ~150 lines. `video_read_line()` pulls one
display line through two layers: LZSS (64 KiB ring buffer, literal/match
tokens — the format is documented in `video.h`) and per-line RLE
(`[count-1][R][G][B]` runs). Peak RAM: ~67 KiB, regardless of video
length; the decoder is strictly forward-only (no frame seek).

**`msp3520.c`** — same pins, panel init and reset as before. The push
path drives the D/C line from the SPI driver's `pre_cb` (the same
mechanism the official `esp_lcd` component uses), which makes **queued,
interrupt-driven transactions** possible. `msp3520_queue_block()` submits
one 16-line (23,040 B) pure-data DMA transaction; with two blocks in
flight, the CPU decodes the next block while DMA pushes the previous one.
v5 adds `msp3520_prepare_half_stream()` (a 240-px half-window: CASET
0..239 or 240..479, full height — every RAMWR starts at the window
origin, ILI9488 datasheet §5.2.24) and `msp3520_queue_half_block()`
(16 half-window lines, 11,520 B).

**`main.c`** — the sweep logic. `push_frame()` streams one progressive
frame (20 blocks); `push_ilace_frame()` (v5) decodes the frame once into
a 460,800 B PSRAM stage (the decoder cannot seek back), then streams it
as two half-frames, slicing each 16-line half-block out of the stage
with plain `memcpy`s. `play_segment()` loops frames for the test
duration, paces them to 15 fps when the bus is fast, polls your keys
non-blocking, and measures fps. The verdict/summary/re-test flow matches
the first project so both consoles feel the same.

## 7. Going further

* Convert your own clips (`tools/convert_video.py`, §2) — keep them under
  ~7 MB.
* Try `PLAY_SECONDS` and the `FREQ_*` defines in `main.c`.
* The `d` key + wiring levers (series-R on SCK, GND alongside, short
  wires) are the complete remaining $0 experiment space for 80 MHz —
  see the repo WORKLOG §12/§13 for the research behind it.
* For real GUIs at your measured max clock, look at `esp_lcd` + LVGL
  next; the queued-push pattern in `msp3520.c` is a hand-rolled taste of
  what `esp_lcd` does for you.

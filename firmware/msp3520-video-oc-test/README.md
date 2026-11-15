# MSP3520 Video Player + SPI Clock Sweep — ESP32-S3 + ESP-IDF (v6)

The second MSP3520 test program: it plays **your video** (`Hill[729].mp4`,
converted and embedded — 119 source frames, 15 fps, 7.9 s → 173 fields)
fullscreen on the 480×320 display at every SPI clock from **10 MHz to
80 MHz** in 5 MHz steps, and records your pass/fail verdict per frequency.
v6 plays **field-interlaced**: every field is one 240-px half of the screen
carrying a **fresh source sample**, so motion advances at **~21.7 steps/s
at 40 MHz** — double the ~10.85 full-frame rate of the progressive v4/v5
builds. The trade (the classic interlace one, accepted): each field is
half the picture, so there is a stationary mid-screen seam and fast pans
shear across it; see §1.

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
inside the firmware image, so the app is ~5.6 MB. The standard
ESP32-S3-DevKitC-1 has 8 MB of flash (N8/N8R2 and N16 modules all work).
That is the only requirement — v6 needs **no PSRAM** (fields stream
straight from the decoder to the wire; no full frame is ever staged).

---

## 1. Field-interlaced playback — the honest physics

A full RGB666 frame is 480×320×3 = 460,800 bytes over the wire. The 40 MHz
bus (the panel's practical ceiling — see the sweep) therefore tops out at
**~10.85 full frames/s**, whatever the firmware does. A half-frame is
230,400 bytes, so the same bus carries **~21.7 fields/s**.

**v5 (progressive / same-frame halves):** one full-screen update per
source frame — ~10.5 fps measured at 40 MHz, no artifacts. v5's `i` key
split each frame into two half-screen updates, but both halves carried the
SAME frame: the wire still moved every byte of every picture, so the
picture rate could not move (interlacing halves the bytes per *field*,
never the bytes per *picture*). Honest, but not a speedup.

**v6 (field-native content):** the blob itself is baked as fields. Field
*k* is one 240-px half (even *k* = left, odd = right) of the source frame
at time *k*/21.70 s — every field carries a **fresh temporal sample**, one
field period after the previous one. The player streams them straight
into alternating half-windows (16-line DMA blocks, ~23 SPI transactions
per field, no PSRAM — no full frame ever exists in RAM). Result at
40 MHz: **~21.7 fields/s, motion advancing at double cadence** vs the
progressive build. Static content is pixel-identical to progressive.

The trade (the classic interlace one — accepted for this build):

* Each field covers half the screen; the other half is one field period
  (46 ms) older. There is a **stationary vertical seam at mid-screen**,
  and during fast horizontal pans the two sides shear by ~46 ms of motion
  across that seam.
* Per-pixel update rate is unchanged (~10.85 Hz at 40 MHz) — what doubles
  is the *global* motion cadence: something on screen advances every
  46 ms instead of every 92 ms.
* The 15 fps source only has 15 unique samples/s: 173 fields sample the
  119 frames, so v6 displays ~15 unique temporal samples/s vs
  progressive's ~10.85 (+38%) at a 2× display cadence — not a full 2× of
  *new* information (that would need a ≥21.7 fps source; convert any such
  clip with the same command and v6 plays it at the full 21.7).

(Historical note: v1–v3 used ROW interlacing — even/odd rows — which
combed on every moving edge and needed 4 SPI transactions per line; v4
went progressive; v5 tried same-frame column halves as a cadence A/B; v6
bakes the column split into the content itself. True even/odd *row* fields
would still need one address window per line — 1280 transactions/frame —
which is why the wire-cheap realization of interlacing on the ILI9488 is
the half-*window* split.)

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
4. **Field-halves scan layout** (`--scan field-halves`, the shipped
   blob's mode since v6): each field is one 240-px half of the source
   frame at its own timestamp, alternating left/right at the field rate
   (`--field-rate`, default 21.70 = the 40 MHz wire ceiling; keep it in
   sync with `FIELD_HZ_X100` in `main.c`). (`--scan progressive`
   whole-frame layout — what v4/v5 shipped — and the legacy
   `--scan interlaced` even/odd-line layout of v1–v3 also remain.)
5. **Per-line RLE.** Runs of `[count-1][R][G][B]`, counts summing to 480
   per display line.
6. **LZSS.** A small home-grown LZ (64 KiB window, 3-byte match tokens)
   squeezes repetition across lines. No zlib/deflate on the device needed —
   the decoder is ~150 lines of C you can read.

Result: 39,859,200 bytes of raw pixels → **5,304,991 bytes (7.5×)**,
embedded into the firmware and flashed with `idf.py flash` — no extra
upload step. The decoder was verified on the build machine: the exact
`video.c` that runs on the ESP32-S3 decodes all 55,360 field lines
byte-identical to the reference expansion (`tools/host_test_video.py`).

To embed a different video (any resolution/aspect/fps up to a few tens of
seconds):

```
python3 tools/convert_video.py YOURCLIP.mp4 main/video.bin --scan field-halves
```

`convert_video.py --help` documents every knob (crop, fps re-timing,
filters, field rate). Keep the output under ~7 MB (the script prints the
size and verifies the roundtrip every time). Then rebuild and re-flash.
Requirements: Python 3 with `numpy` and `Pillow`, plus a working `ffmpeg`.

> The converter in `tools/` is the current one (MIV2, all three scan
> layouts). The shipped `video.bin` is `--scan field-halves` (flags=4);
> a progressive or legacy blob is rejected at boot with a clear message.

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
   MSP3520 VIDEO player + SPI clock sweep (v6, FIELD-INTERLACED)
   ESP32-S3 + ESP-IDF  |  sweep 10..80 MHz in 5 MHz steps
  ============================================================

  Embedded video: 119 source frames @ 15 fps -> 173 fields
  (one field = one 240-px half of a FRESH source frame),
  stored in 5181 KiB (7.5x smaller than the raw 38925 KiB of pixel data)

  Wire format: 3 bytes/pixel (COLMOD 0x66). S3 clock
  reality: 35-60 MHz requests run at 40 MHz, 65-80 MHz at
  80 MHz (no other steps exist).
  ...
  [ 1/15] 10 MHz
  playing: 54 fields, 5.4 fields/s   [y/n/r/q/d]      ← updates every 0.5 s
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
anything you get the same question at the end. The measured fps is printed
live and goes into the summary:

```
  ======================== SUMMARY ========================
   MHz  fields/s  verdict
  --------------------------------------------------------
   10     5.4    PASS
   40    20.6    PASS
   60    20.6    PASS      ← 35-60 all run at 40 MHz actual
   80    16.4    FAIL      ← snow; 80 is 4x the panel's spec
  ========================================================
  Highest CLEAN frequency : 40 MHz  (…)
  Suggested for projects  : 35 MHz or lower
```

Reading the fields/s column:

* **~20.6** = bus-limited at 40 MHz actual — that IS the RGB666 wire
  ceiling for half-frames (5.4 fields/s at 10 MHz, 10.9 at 20, 16.3 at
  30). A sudden *drop* compared to the previous step is itself a sign of
  corruption — dropped bytes cost both picture quality and throughput.

After the sweep you can re-test any frequency (type `63` + Enter, or just
Enter to skip), and finally there is a **continuous playback mode**: type a
frequency and the video loops at it until you press a key — the "enjoy
your work" mode.

## 4. What corruption looks like on moving video

* `n` verdicts: **snow/noise** in flat areas (sky), **torn horizontal
  stripes** (a line's pixels shifted sideways), **color speckles**, a
  **frozen picture** while the fps counter stalls, large **garbage
  blocks**.
* Normal, *not* failures: slower playback at low clocks; a one-field
  "tear" exactly when the video loops back to field 0 (the loop restarts
  mid-scroll, every ~8.0 s); the stationary mid-screen seam and the
  ~46 ms shear across it during fast pans (§1) — that is the accepted
  interlace trade, visible whenever content moves.

Moving video is a much harsher test than static bars: minutes of
continuous streaming with every byte checked by your eyes. A display that
passes here will pass anything you throw at it later.

## 5. Troubleshooting

| Symptom | Fix |
|---|---|
| Build error: `binary too large` / partition overflow | The 8 MB flash settings didn't apply: delete the generated `sdkconfig` (keep `sdkconfig.defaults`!) and build again; make sure `partitions.csv` is in the project root. |
| Flashes, then boot loops / `invalid header` | Flash size mismatch: `idf.py erase-flash`, then rebuild+reflash with `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` (set in `sdkconfig.defaults`). |
| `FATAL: embedded video.bin failed to open` | `main/video.bin` missing or not a converter output — regenerate with `tools/convert_video.py`. |
| `FATAL: v6 streams FIELD-INTERLACED blobs (flags bit2)` | `video.bin` is a progressive/legacy blob — regenerate with `--scan field-halves`. |
| Boot screen (dark blue) never appears | Wiring: see the first project's troubleshooting table (LED/backlight, DC/RESET, SCK/MOSI). |
| Video plays but rows look shuffled / halves swapped | That would be a driver bug, not overclocking — re-check the wiring first (SCK and MOSI are adjacent on both headers and easy to swap). |
| fps stuck far below the table values at high MHz | Signal quality: shorter wires, reseat jumpers; at the 65–80 steps also try the `d` key + a 47–100 Ω series resistor on SCK. |
| Seam/shear visible during motion | Expected (§1) — the accepted interlace trade. Static content is pixel-identical to progressive. |
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

**`main.c`** — the sweep logic. `push_field(half)` streams one field
straight from the decoder into a 240-px half-window (20 DMA blocks of 16
lines, ~23 SPI transactions; no staging, no PSRAM — v5 needed a 460,800 B
frame stage because its blob held whole frames and the decoder cannot
seek back; v6's blob holds fields, so the decoder walks exactly what the
wire needs). `play_segment()` loops fields for the test duration, paces
them to 21.70/s when the bus is fast (matching the converter's bake
rate), polls your keys non-blocking, and measures fields/s. The
verdict/summary/re-test flow matches the first project so both consoles
feel the same.

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

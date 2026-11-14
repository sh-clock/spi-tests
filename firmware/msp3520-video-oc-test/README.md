# MSP3520 Interlaced Video Player + SPI Clock Sweep — ESP32-S3 + ESP-IDF

The second MSP3520 test program: it plays **your video** (`Hill[729].mp4`,
converted and embedded — 119 frames, 15 fps, 7.9 s) fullscreen on the
480×320 display **using interlacing**, at every SPI clock from **10 MHz to
80 MHz** in 5 MHz steps, and records your pass/fail verdict per frequency.

```
msp3520-video-oc-test/
├── CMakeLists.txt        ← project file
├── partitions.csv        ← custom partition table (one big 8 MB app slot)
├── sdkconfig.defaults    ← 8 MB flash, console, 240 MHz CPU
├── main/
│   ├── CMakeLists.txt    ← embeds video.bin into the firmware image
│   ├── video.bin         ← THE CONVERTED VIDEO (5.4 MB, generated)
│   ├── video.h/.c        ← tiny streaming decoder (LZSS + RLE)
│   ├── msp3520.h/.c      ← display driver, streaming edition
│   └── main.c            ← sweep, playback pacing, verdicts, summary
└── tools/
    └── convert_video.py  ← the converter, so you can embed other videos
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
inside the firmware image, so the app is ~6 MB. The standard
ESP32-S3-DevKitC-1 has 8 MB of flash (N8/N8R2 and N16 modules all work);
`sdkconfig.defaults` and `partitions.csv` already configure everything.

---

## 1. What "interlaced" means here (and why it is the point)

A full frame is 480×320×3 bytes = 460,800 bytes over the wire. At 15 fps
that is 55.3 Mbit/s — more than an 80 MHz SPI bus can carry (it manages
~21.7 fps at best, and much less once the panel init overhead is paid).

**Interlacing halves the per-update cost**, exactly like old analog TV:
each frame is sent as two *fields* —

* field 0: display rows 0, 2, 4, …, 318 (the even rows), 160 lines,
* field 1: display rows 1, 3, 5, …, 319 (the odd rows), 160 lines.

A field is 230,400 bytes, and fields alternate 30× per second: the screen
is fully refreshed 15× per second, but each individual burst — and each
moment the bus is busy — only carries half a frame. 15 fps now needs only
~55 MHz of SPI clock instead of ~83 MHz.

Two visible consequences you should know **before** judging a test step:

* **Combing is normal.** For 1/30 s, a fast-moving horizontal edge shows a
  sawtooth "comb" because the even rows show the newer position while the
  odd rows still show the previous one. That is inherent to interlacing,
  not a fault.
* **Slow-motion at low clocks is normal.** Below ~60 MHz the bus cannot
  keep up with 30 fields/s, so the video simply plays slower (2.7 fps at
  10 MHz). The firmware paces playback to natural speed whenever the bus
  finishes a field early.

What is **not** normal: noise/snow, torn or shifted stripes, wrong colors,
frozen or garbage regions — those are corruption (press `n`).

## 2. How the video got into the firmware

You gave me `Hill[729].mp4` (320×240, 15 fps, 119 frames, 7.9 s, h264+aac).
The conversion I ran (and re-ran to produce the shipped `main/video.bin`)
was:

```
python3 tools/convert_video.py Hill[729].mp4 main/video.bin \
    --drop-green-lsb --chain 48 --store 96
```

Step by step:

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
4. **Interlace split.** Each frame becomes an even-rows field and an
   odd-rows field (30 fields/s).
5. **Per-line RLE.** Runs of `[count-1][R][G][B]`, counts summing to 480
   per display line.
6. **LZSS.** A small home-grown LZ (64 KiB window, 3-byte match tokens)
   squeezes repetition across lines and fields. No zlib/deflate on the
   device needed — the decoder is ~80 lines of C you can read.

Result: 54,835,200 bytes of raw pixels → **5,548,128 bytes (9.9×)**,
embedded into the firmware and flashed with `idf.py flash` — no extra
upload step. Both decoder layers were verified on my build machine: the
exact `video.c` that runs on the ESP32-S3 decodes all 38,080 display lines
byte-identical to the reference expansion.

To embed a different video (any resolution/aspect/fps up to a few tens of
seconds):

```
python3 tools/convert_video.py YOURCLIP.mp4 main/video.bin \
    --crop iw:ih:0:0 --fps 15 --drop-green-lsb
```

`--crop iw:ih:0:0` disables cropping; `--fps 15` re-times the clip. Keep
the output under ~7 MB (the script prints the size and verifies the
roundtrip every time). Then rebuild and re-flash. Requirements: Python 3
with `numpy` and `Pillow`, plus a working `ffmpeg` on your PATH.

---

## 3. Build, flash, run

Same three steps as the first project (VSCode + ESP-IDF extension v5.5):

1. `File → Open Folder…` → this folder. Set the target to **esp32s3**
   (status bar → chip name).
2. Plug the USB cable into the **COM/UART** port, select that serial port.
3. **Build → Flash → Monitor.** First build takes a few minutes; flashing
   writes a ~6 MB image, so the flash step is visibly longer than usual —
   that is the video going in.

The console session looks like this:

```
  ============================================================
   MSP3520 interlaced VIDEO player + SPI clock sweep
   ESP32-S3 + ESP-IDF  |  sweep 10..80 MHz in 5 MHz steps
  ============================================================

  Embedded video: 119 frames @ 15 fps (7.9 s), 238 fields,
  stored in 5418 KiB (9.9x smaller than the raw 53549 KiB of pixel data)

  INTERLACING: each frame goes out as 2 fields (even rows,
  then odd rows) = half the bytes per screen update. Faint
  combing on fast motion is NORMAL. ...
  ...
  [ 1/15] 10 MHz
  playing: 24 frames, 2.4 fps   [y/n/r/q]      ← updates every 0.5 s
  playing: 27 frames, 2.7 fps   [y/n/r/q]
   -> clean? [y]es [n]o [r]eplay [q]uit:
```

**Keys work DURING playback** (single keypress, no Enter): `y` clean,
`n` artifacts, `r` restart this frequency, `q` stop the sweep. Each
frequency plays the video on loop for 10 s; if you don't press anything
you get the same question at the end. The measured fps is printed live and
goes into the summary:

```
  ======================== SUMMARY ========================
   MHz      fps     verdict
  --------------------------------------------------------
   10     2.7    PASS
   40    10.8    PASS
   60    14.9    PASS
   70    15.0    PASS
   75    15.0    FAIL      ← e.g. snow in the sky at 0:04
   80     6.1    FAIL      ← bus errors also cost throughput
  ========================================================
  Highest CLEAN frequency : 70 MHz  (…)
  Suggested for projects  : 65 MHz or lower
```

Reading the fps column:

* **15.0** = the bus keeps up with natural speed (fields are paced) — from
  about 60 MHz upward, assuming clean playback.
* **Below 15.0** = bus-limited: the number is what the bus actually
  delivered (2.7 fps at 10 MHz, 5.4 at 20, 8.1 at 30, 10.8 at 40, 13.6 at
  50). A sudden *drop* compared to the previous step is itself a sign of
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
* Normal, *not* failures: faint interlace combing on moving edges; slower
  playback at low clocks; a one-frame "tear" exactly when the video loops
  back to frame 0 (the loop restarts mid-scroll) — loop restarts happen
  every 7.9 s at natural speed.

Moving video is a much harsher test than the first project's static bars:
minutes of continuous streaming with every byte checked by your eyes. A
display that passes here will pass anything you throw at it later.

## 5. Troubleshooting

| Symptom | Fix |
|---|---|
| Build error: `binary too large` / partition overflow | The 8 MB flash settings didn't apply: delete the generated `sdkconfig` (keep `sdkconfig.defaults`!) and build again; make sure `partitions.csv` is in the project root. |
| Flashes, then boot loops / `invalid header` | Flash size mismatch: `idf.py erase-flash`, then rebuild+reflash with `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` (set in `sdkconfig.defaults`). |
| `FATAL: embedded video.bin failed to open` | `main/video.bin` missing or replaced by something that is not a converter output — regenerate with `tools/convert_video.py`. |
| Boot screen (dark blue) never appears | Wiring: see the first project's troubleshooting table (LED/backlight, DC/RESET, SCK/MOSI). |
| Video plays but rows look shuffled / every other line stale | That would be a driver bug, not overclocking — please re-check the wiring first (SCK and MOSI are adjacent on both headers and easy to swap). |
| fps stuck far below the table values at high MHz | Something else loading the CPU? A debug probe? Otherwise suspect signal quality: shorter wires, reseat jumpers. |
| Combing on fast motion | Normal interlacing (§1). |
| Video loops with a visible jump | Normal: the clip is 7.9 s and loops. |

## 6. Code walkthrough (what changed vs the first project)

**`video.c`** — the decoder, ~150 lines. `video_read_line()` pulls one
display line through two layers: LZSS (64 KiB ring buffer, literal/match
tokens — the format is documented in `video.h`) and per-line RLE
(`[count-1][R][G][B]` runs). Peak RAM: ~67 KiB, regardless of video length;
nothing per-frame is ever buffered. `video_rewind()` resets to field 0.

**`msp3520.c`** — same pins, panel init and reset as before. New is the
push path: the D/C line is driven from the SPI driver's `pre_cb`
(pre-transaction callback — the same mechanism the official `esp_lcd`
component uses), which makes **queued, interrupt-driven transactions**
possible. `msp3520_queue_line()` submits four transactions per line
(PASET, its params, RAMWR, 1440 pixels) and returns immediately; with two
lines in flight, the CPU decodes line *k+1* from flash while DMA is still
pushing line *k*. That overlap is why even 80 MHz playback leaves the CPU
mostly idle.

**`main.c`** — the sweep logic. `push_field()` streams one interlaced
field (160 lines of one parity); `play_segment()` loops fields for the
test duration, paces them to 30/s when the bus is fast, polls your keys
non-blocking (`uart_read_bytes` with zero timeout), and measures fps. The
verdict/summary/re-test flow matches the first project so both consoles
feel the same.

## 7. Going further

* Convert your own clips (`tools/convert_video.py`, §2) — keep them under
  ~7 MB.
* Try `PLAY_SECONDS` and the `FREQ_*` defines in `main.c`.
* For real GUIs at your measured max clock, look at `esp_lcd` + LVGL next;
  the queued-push pattern in `msp3520.c` is a hand-rolled taste of what
  `esp_lcd` does for you.


# msp3520-video-lab — video technique testbed (INTRO.mkv, FULL clip)

A standalone ESP-IDF project that plays the ENTIRE test video
(`upload/INTRO.mkv`, DVD rip: 720x480 anamorphic MPEG-2, 29.97 fps,
12.045 s) with every playback technique under test, selected live from
the `esp-idf_monitor` console. A technique loops until any key is pressed,
then the menu returns. Same wiring, panel and 40 MHz clock as all
previous projects (see `../msp3520-video-oc-test/README.md`).

## The source

The encoder (`tools/encode_lab.py`) desqueezes the anamorphic frame
(SAR 8:9 -> 640x480), detects and crops the letterbox bars (47/51 px),
center-crops to 3:2 (540-px-equivalent) and scales to 480x320 —
pan-and-scan, no geometry distortion. The WHOLE clip is baked: 122
frames sampled at 10 fps (= all 12.2 s of content; the wire can only
show ~10.5 full frames/s at 40 MHz, so this plays essentially
real-time). Measured, full 29.97 fps would need 17.9-22.1 MB depending
on dither — impossible on the 8 MB flash without an SD card (see
WORKLOG 20 for the measurement table).

All techniques share one codec: 8-bit indexed frames with a global
256-colour palette whose entries ARE the ILI9488 RGB666 wire bytes,
2x2 Bayer ordered dithering, 3x3 median pre-filter (kills MPEG-2 noise
so static areas are bit-stable). Mean decode error 2.79/255. One index
expands to 3 wire bytes through a 768-byte LUT in DRAM. Every frame is
INDEPENDENTLY compressed (RLE runs, then the v6/v7 LZSS with a 32 KiB
window — 2.78x on this content), so any frame is reachable in any
order; the firmware (`main/labvid.c`) decodes on demand into one
480x320 DRAM staging frame and every technique reads the staged frame.

## Assets (all embedded in the app, 6.81 MB total)

| file | technique | frames | size |
|---|---|---|---|
| base.vlb2 compressed 480x320 | ALL (the staged source) | 122 | 6,735,381 B |
| palette.pal 256 x RGB666 | all | - | 768 B |
| merge2.bin 64K index-merge LUT | 11a/11b | - | 65,536 B |
| mbmap.bin macroblock dirty bits | 8 | 122 | 9,160 B |

What used to be four extra flash assets is now derived at runtime from
the staged frames: 11a/11b decimate 2:1/3:1 through merge2 (nearest
palette entry to the average of each index group), 13 reads rows p::4
of the staged frame, 9 diffs the staged frame against the previous
frame live, and 8 reads the encoder's dirty bitmaps (mbmap.bin).

`tools/verify_assets.py` round-trips every blob with the firmware's own
decode logic: all 122 frames decode exactly, mbmap is recomputed
bit-for-bit from the decoded frames, merge2 is recomputed from the
palette. `tools/host_test_labvid.py` goes one step further: it compiles
`main/labvid.c` UNMODIFIED with gcc, decodes the real blob on the host,
and compares per-frame FNV-1a hashes against the Python mirror
(122/122 must match).

## Menu (115200 on either USB port)

```
1    pure 4:1 quad interlace        80 rows/field   115.2 KB
2    static scanlines + 2:1 (CRT)   80 rows/field   115.2 KB
3    static scanlines + 4:1 (fast)  40 rows/field    57.6 KB
4    pure 8:1 octuple interlace     40 rows/field    57.6 KB
5    3:1 triple interlace          ~107 rows/field  153.6 KB
6    column 4:1 interlace          120 cols/field   115.2 KB
7a   quincunx (half-width lines)   320 half-rows    230.4 KB
7b   quincunx + 2:1 rows           160 half-rows    115.2 KB
8    macroblock 16x16 delta grid   adaptive         variable
9    span delta (runtime, prev f)  adaptive         variable
10   LFSR stochastic 4:1           80 random rows   115.2 KB
11a  pixel 2x doubling (240-wide)  full frame       460.8 KB
11b  pixel 3x doubling (160-wide)  full frame       460.8 KB
12   8-bit indexed full frame       256-colour       460.8 KB
13   pre-interlaced 4:1 rows       80 rows/field    115.2 KB
14   dual-core ping-pong DMA       full frame       460.8 KB
15   pure 2:1 row interlace (base) 160 rows/field   230.4 KB
c    set SPI clock (10/15/20/26.667/40 MHz lattice)
i    asset info
```

During playback a stats line prints every 2 s: fields/s, MB/s on the
SPI bus, and a final line when stopped.

## Implementation notes

- **Row/stripe/span techniques** use a new queued-window session API in
  the driver (`msp3520_session_begin/qcaset/qpaset/qramwr/qdata/...
  /session_end`): CS stays LOW for the whole field while window commands
  and pixel data are queued as DMA transactions (the pre_cb callback
  flips D/C per transaction). One window costs 11 wire bytes (~0.8% on a
  1440-byte row) — this is the queued-DMA answer to the v1-v3 polling
  mistake (1280 polled transactions per frame).
- **Full-frame techniques** (11/12/14) use the v6/v7 fixed-window block
  streaming (1 RAMWR + 20 blocks per field).
- **Scanline techniques** (2/3) clear the panel to black once at start;
  odd physical rows stay dark, even rows carry the even source lines.
- **Column interlace** (6) needs no MADCTL change: MADCTL is already
  0x28 (MV=1), so a 1-px CASET window selects one full-height column.
- **Macroblock grid** (8) sends only 16x16 blocks whose encoder-computed
  bitmap bit is set (mbmap.bin: block differs from the previous frame —
  the median+exact-key encode makes static areas bit-identical), groups
  dirty blocks into runs, one window per run, full-frame fallback above
  66% dirty.
- **Span delta** (9) is computed LIVE: the staged frame is diffed
  against the previous frame (tolerance 0, so the panel reconverges to
  the source every frame — nothing can drift) while the previous frame
  streams row-by-row through the same decoder (the staged frame is
  cached, so its ring is free).
  Fragmented rows (> 32 spans or > 92% coverage) collapse to one
  full-row window. Calm frames are a few KB; burst frames are big.
- **Pixel doubling** (11a/11b) builds the 240/160-wide source from the
  staged 480-wide frame through the merge2 LUT (nearest palette entry
  to the average of each 2/3-index group; 3:1 is two LUT steps) and
  stretches it back in the DMA buffer — same wire bytes as a full
  frame, the test shows the storage trade-off.
- **Dual-core** (14): core 0 stages frames and expands blocks into
  double buffers, core 1 queues them to SPI, semaphore handshake per
  buffer, FIFO-correct retire ordering (both in-flight blocks retire
  before CS goes high at field boundaries). Compare with 12 — which
  pays the frame decode serially — the difference is the pipeline.
- Fields play back-to-back (bus-limited); the source frame advances
  every `ratio` fields, so a slower bus = slow-motion content (same
  semantics as the earlier players).
- The panel is initialized lazily on the first selection, so the menu
  comes up even with no display attached (and under QEMU, which stalls
  on the first SPI transaction — see WORKLOG 18).

## Expected field rates at 40 MHz (bus-limited, RGB666)

57.6 KB/field ~ 86/s max (expect ~60-70 measured), 115.2 KB ~ 43/s max
(~35-40), 153.6 KB ~ 32/s (~27-30), 230.4 KB ~ 21.7/s (~20), 460.8 KB
~ 10.9/s (~10.5). Adaptive techniques (8/9) swing between the calm and
burst extremes live — that is the point of the cut.

## Build / flash

```
idf.py set-target esp32s3
idf.py build flash monitor
```

Assets are rebuilt from the source video with
`python3 tools/encode_lab.py` (needs ffmpeg + numpy + scipy), checked
with `python3 tools/verify_assets.py`, and the exact device decoder is
gated on the host with `python3 tools/host_test_labvid.py` (compiles
main/labvid.c with gcc against the real blob). Flashing the ~7 MB app
takes a few minutes at 460800 baud.

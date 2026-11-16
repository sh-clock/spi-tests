/*
 * assets.h — access to the embedded lab assets (see tools/encode_lab.py).
 *
 * The FULL 12 s clip is one compressed base asset (VLB2, decoded by
 * labvid.c into a DRAM staging frame). Everything the techniques need is
 * derived from that at runtime:
 *   - the palette LUT (index -> 3 ILI9488 RGB666 wire bytes, in DRAM)
 *   - merge2.bin: 64 KiB index-merge LUT — merge2[(a<<8)|b] is the
 *     palette index nearest to the average of colours a and b. The pixel
 *     doubling techniques build their 240/160-wide sources from the
 *     staged 480-wide frame through this table (two steps for 3:1).
 *   - mbmap.bin: per-frame 16x16 macroblock dirty bitmaps (VLM1) — which
 *     blocks differ from the PREVIOUS frame, exactly what technique 8
 *     needs without holding two frames in RAM.
 *
 * Blob headers (little-endian):
 *   base.vlb2  "VLB2" u16 w u16 h u16 frames u32 offset[frames+1]
 *              (see labvid.h)
 *   mbmap.bin  "VLM1" u16 bw u16 bh u16 frames, then frames bitmaps of
 *              bw*bh bits each (bit = block differs from previous frame;
 *              frame 0 is all-dirty), row-major, MSB first within a byte
 *   palette.pal 256 * 3 wire RGB666 bytes
 *   merge2.bin  65536 bytes, merge2[(a<<8)|b] as above
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Parse and validate every blob (called once at boot, before any SPI use).
 * Returns false (with a message on the console) on any mismatch. */
bool lab_assets_init(void);

/* The compressed base clip (open with labvid_open — done by
 * lab_assets_init; afterwards labvid_* works). */
uint32_t lab_base_frames(void);

/* 256 RGB666 wire triplets, copied to DRAM at init (index -> wire bytes). */
extern const uint8_t (*lab_lut)[3];

/* The 64 KiB merge LUT (flash, read-only). */
const uint8_t *lab_merge2(void);

/* Macroblock dirty bitmaps: pointer to frame 0's bitmap (bw*bh bits each,
 * frames of them back to back). */
const uint8_t *lab_mbmap(void);
uint16_t lab_mbmap_frames(void);

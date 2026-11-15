/*
 * assets.h — access to the embedded lab assets (see tools/encode_lab.py).
 *
 * All frame data is 8-bit indexed colour with ONE global 256-entry palette
 * whose entries are already ILI9488 RGB666 wire bytes; players expand one
 * index to 3 wire bytes through the 768-byte LUT copied to DRAM at boot.
 *
 * Blob headers (little-endian):
 *   "VLB1" u16 w u16 h u16 frames   [base.idx, half.idx, third.idx]
 *          then frames * h * w index bytes, row-major, frame-major.
 *   "VLP1" u16 w u16 h u16 frames u16 phases   [pre_i4.idx]
 *          then frames * phases fields of (h/phases)*w bytes each
 *          (field-major 4:1 interlace: field = frame f, phase p has the
 *          rows p, p+4, p+8, ... stored contiguously — exactly the bytes
 *          that get sent, in order).
 *   "VLD1" u16 w u16 h u16 frames   [delta.spans]
 *          then, per frame: for each of h rows: u8 nspans, then nspans
 *          spans of (u16 x0, u16 len, len index bytes) — the pixels whose
 *          palette colour changed by more than the encoder tolerance vs
 *          the previous frame. Frame 0 is full rows. Sequential stream.
 *   palette.pal: 256 * 3 wire RGB666 bytes.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const uint8_t *data;    /* first frame / first field                 */
    uint16_t       w, h;    /* frame geometry (480x320 / 240x320 / ...)   */
    uint16_t       frames;  /* number of source frames                    */
    uint16_t       phases;  /* fields per frame (pre_i4: 4; others: 1)    */
} lab_idx_t;

typedef struct {
    const uint8_t *blob;    /* start of the header                        */
    const uint8_t *frames; /* first byte of frame 0's row data            */
    uint16_t       frames_n;
} lab_delta_t;

/* Parse and validate every blob (called once at boot, before any SPI use).
 * Returns false (with a message on the console) on any mismatch. */
bool lab_assets_init(void);

const lab_idx_t   *lab_base(void);
const lab_idx_t   *lab_prei4(void);
const lab_idx_t   *lab_half(void);
const lab_idx_t   *lab_third(void);
const lab_delta_t *lab_delta(void);

/* 256 RGB666 wire triplets, copied to DRAM at init (index -> wire bytes). */
extern const uint8_t (*lab_lut)[3];

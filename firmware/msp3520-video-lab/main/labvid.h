/*
 * labvid.h — compressed frame provider for the video lab (blob "VLB2").
 *
 * The full 12 s clip is stored as ONE base asset of independently
 * compressed frames, so any frame is reachable in any order (the
 * techniques loop, skip and wrap freely). Frames are 8-bit palette
 * indices, 480x320, exactly like the old uncompressed base.idx — only
 * the storage changed.
 *
 * Blob layout (little-endian):
 *   "VLB2" u16 w u16 h u16 frames
 *   u32 offset[frames+1]   offset[f] = first byte of frame f's stream,
 *                          offset[frames] = end of the last stream
 *   per frame (independent):
 *     RLE  one stream of rows; per row, runs of [count-1][index]
 *          whose counts sum to w (same shape as the v6/v7 wire codec,
 *          with a 1-byte payload)
 *     LZSS the v6/v7 stack on top: 32 KiB history ring (measured cost vs
 *          64 KiB: +1.3% asset size, saves 2 x 32 KiB of DRAM), literal =
 *          1 byte, match = 3 bytes (off-1 big-endian 16 bit, len-4), one
 *          flag byte per 8 tokens. The encoder never emits an offset
 *          larger than LABVID_RING_SIZE, so the decoder ring is
 *          sufficient.
 *
 * DRAM: one 480x320 staging frame is heap-allocated at open (153.6 KB —
 * static BSS would overflow dram0_0_seg next to the player buffers);
 * the decoder (ring included, ~33 KB) stays static and doubles as the
 * previous-frame row iterator for the runtime span-delta technique.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define LABVID_RING_BITS  15                     /* 32 KiB ring           */
#define LABVID_RING_SIZE  (1u << LABVID_RING_BITS)
#define LABVID_MAX_W      480
#define LABVID_MAX_H      320

/* Parse and validate the blob (offsets monotonic, payload length exact).
 * Returns false on any mismatch. */
bool labvid_open(const uint8_t *blob, uint32_t blob_len);

/* Number of frames in the blob. */
uint32_t labvid_frames(void);

/* Decode frame f into the staging buffer and return it (NULL on a corrupt
 * stream). Repeated calls with the same f are free — the staged frame is
 * cached until a different frame is requested. */
const uint8_t *labvid_stage(uint32_t f);

/* --- previous-frame row iterator ---------------------------------------- */

/* Rewind the decoder to frame f and stream its rows (any order). Uses
 * the SAME decoder that labvid_stage() stages with — safe because the
 * staged frame is cached in its own buffer, so once a frame is staged
 * the decode ring is free. Do not interleave rows_* with staging a
 * different frame mid-stream (the span player never does). */
bool labvid_rows_begin(uint32_t f);

/* Stream the next row (w index bytes) of the frame chosen above. */
bool labvid_rows_next(uint8_t *dst);

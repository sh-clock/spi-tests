/*
 * video.h — decoder for the compressed interlaced video blob (video.bin).
 *
 * Blob layout (produced by tools/convert_video.py, all values little-endian):
 *
 *   offset 0   "MIV2" magic           ("MIV3" for v7 dirty blobs)
 *   offset 4   u16 width            480
 *   offset 6   u16 height           320
 *   offset 8   u16 frames           e.g. 119
 *   offset 10  u16 flags            bit1: progressive
 *                                    bit2: field-halves (v6)
 *                                    bit3: dirty blocks (v7, needs bit2)
 *   offset 12  u32 rle_len          size of the decoded RLE stream (bytes)
 *   offset 16  u32 fields           progressive: == frames; halves: fields
 *   offset 20  u32 bpp              3 = RGB666 wire format (2 = RGB565)
 *   offset 24  LZSS stream
 *
 * The LZSS stream decodes into an RLE byte stream. The RLE stream is a
 * sequence of LINE blocks: each line block is a series of runs
 *   [count-1][px...]   (count = 1..256 pixels of one color, bpp bytes each)
 * Scan layouts (flags):
 *   bit1 (progressive): a FRAME is `height` = 320 line blocks of `width`
 *     = 480 pixels, frames back to back (v4/v5 players).
 *   bit2 (field-halves, v6): a FIELD is `height` = 320 line blocks of
 *     `width`/2 = 240 pixels, fields back to back. Field 0, 2, 4... are
 *     the LEFT half (x 0..239), odd fields the RIGHT half (x 240..479) —
 *     and every field carries the source frame at its own timestamp, so
 *     consecutive fields are one field period apart in content
 *     (field-native interlacing).
 *   bit3 (dirty, v7 — on top of bit2, magic "MIV3"): each FIELD starts
 *     with a little-endian u32 BLOCK BITMAP (bit b = 16-line block b of
 *     this half is present), followed by the line runs of the present
 *     blocks ONLY. A block is present only if its wire bytes differ from
 *     the previous field that wrote the same half (field k-2) — the
 *     ILI9488 GRAM persists, so unchanged blocks are neither stored nor
 *     re-sent. Fields 0/1 are fully present; a field identical to its
 *     same-half predecessor carries bitmap 0 and no data at all.
 *     video_read_line() must be called ONLY for the lines of PRESENT
 *     blocks — the data of clean blocks is not in the stream.
 *
 * LZSS token format (verified byte-identical on the host):
 *   1 flag byte per 8 tokens, MSB first
 *   flag bit 1: literal  = next 1 byte
 *   flag bit 0: match    = next 3 bytes: [offH][offL][lenB]
 *                         offset = ((offH<<8)|offL) + 1   (1..65535 back)
 *                         length = lenB + 4               (4..259)
 *   The decoder keeps a 64 KiB ring buffer of decoded history.
 *
 * Usage:
 *   video_open(blob, len);                  // once at boot
 *   video_next_field(&bitmap);              // start a field (1x per field)
 *   video_read_line(buf);                   // 16x per PRESENT block
 *   video_rewind();                         // loop back to field 0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VIDEO_LINE_BYTES   (480 * 3)   /* line buffer size: max over blob bpp */

/* header flag bits (see the layout comment above) */
#define VIDEO_FLAG_PROGRESSIVE   0x0002
#define VIDEO_FLAG_FIELD_HALVES  0x0004
#define VIDEO_FLAG_DIRTY         0x0008

/* blocks per field (16-line blocks over the 320-line half height) */
#define VIDEO_FIELD_BLOCKS       (320 / 16)

typedef struct __attribute__((packed)) {
    char     magic[4];      /* "MIV2" / "MIV3"                          */
    uint16_t width;         /* 480                                      */
    uint16_t height;        /* 320                                      */
    uint16_t frames;        /* number of source frames                  */
    uint16_t flags;         /* bit1 progressive, bit2 halves, bit3 dirty */
    uint32_t rle_len;       /* decoded RLE stream size in bytes         */
    uint32_t fields;        /* progressive: == frames; halves: fields   */
    uint32_t bpp;           /* wire bytes/px: 3 = RGB666 (SPI), 2 = RGB565  */
} video_hdr_t;

/* Parse the header and reset the decoder to field 0. */
bool video_open(const uint8_t *blob, uint32_t blob_len);

/* Header of the opened video (NULL before video_open). */
const video_hdr_t *video_hdr(void);

/* Reset the decoder to the very first line of field 0 (loop point). */
void video_rewind(void);

/* Index of the field video_next_field() will start next (0-based; 0 after
 * video_open/video_rewind). The player uses its parity to pick the
 * half-window (even = left) — this stays correct across 'r' restarts and
 * the loop wrap, unlike a player-side counter. */
uint32_t video_field_index(void);

/* Start the next field and report which 16-line blocks it carries.
 * For dirty blobs (MIV3) this pulls the u32 block bitmap from the stream
 * (bit b set = block b present, blocks 0..19 bottom-up in the half);
 * for MIV2 blobs it returns an all-present bitmap without consuming
 * anything. Must be called once before the field's video_read_line()s.
 * Returns false after the last field (or on a truncated stream) — the
 * caller is expected to video_rewind() and start over. */
bool video_next_field(uint32_t *bitmap);

/* Decode the next display line into dst (VIDEO_LINE_BYTES bytes).
 * Only call this for lines of PRESENT blocks (see video_next_field).
 * Returns false if the stream ended (should not happen for a valid blob —
 * the player loops with video_rewind() instead). */
bool video_read_line(uint8_t *dst);

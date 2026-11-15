/*
 * video.h — decoder for the compressed interlaced video blob (video.bin).
 *
 * Blob layout (produced by tools/convert_video.py, all values little-endian):
 *
 *   offset 0   "MIV2" magic
 *   offset 4   u16 width            480
 *   offset 6   u16 height           320
 *   offset 8   u16 frames           e.g. 119
 *   offset 10  u16 flags            bit0: 0 = even display lines first
 *   offset 12  u32 rle_len          size of the decoded RLE stream (bytes)
 *   offset 16  u32 fields           == frames (progressive blobs)
 *   offset 20  u32 bpp              (2 = RGB565 wire format)
 *   offset 24  LZSS stream
 *
 * The LZSS stream decodes into an RLE byte stream. The RLE stream is a
 * sequence of LINE blocks: each line block is a series of runs
 *   [count-1][px...]   (count = 1..256 pixels of one color, bpp bytes each)
 * Scan layouts (flags):
 *   bit1 (progressive): a FRAME is `height` = 320 line blocks of `width`
 *     = 480 pixels, frames back to back (v4/v5 players).
 *   bit2 (field-halves, v6 — the only layout THIS player accepts): a
 *     FIELD is `height` = 320 line blocks of `width`/2 = 240 pixels,
 *     fields back to back. Field 0, 2, 4... are the LEFT half (x 0..239),
 *     odd fields the RIGHT half (x 240..479) — and every field carries
 *     the source frame at its own timestamp, so consecutive fields are
 *     one field period apart in content (field-native interlacing).
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
 *   video_read_line(buf);                   // 320x per field
 *   video_rewind();                         // loop back to field 0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VIDEO_LINE_BYTES   (480 * 3)   /* line buffer size: max over blob bpp */

typedef struct __attribute__((packed)) {
    char     magic[4];      /* "MIV2"                                   */
    uint16_t width;         /* 480                                      */
    uint16_t height;        /* 320                                      */
    uint16_t frames;        /* number of source frames                  */
    uint16_t flags;         /* bit1: progressive, bit2: field-halves (v6) */
    uint32_t rle_len;       /* decoded RLE stream size in bytes         */
    uint32_t fields;        /* progressive: == frames; v6: half-frames  */
    uint32_t bpp;           /* wire bytes/px: 3 = RGB666 (SPI), 2 = RGB565  */
} video_hdr_t;

/* Parse the header and reset the decoder to field 0. */
bool video_open(const uint8_t *blob, uint32_t blob_len);

/* Header of the opened video (NULL before video_open). */
const video_hdr_t *video_hdr(void);

/* Reset the decoder to the very first line of field 0 (loop point). */
void video_rewind(void);

/* Decode the next display line into dst (VIDEO_LINE_BYTES bytes).
 * Returns false if the stream ended (should not happen for a valid blob —
 * the player loops with video_rewind() instead). */
bool video_read_line(uint8_t *dst);

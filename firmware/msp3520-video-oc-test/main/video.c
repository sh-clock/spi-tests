/*
 * video.c — decoder for the compressed interlaced video blob.
 *
 * Two decoding layers, both tiny on purpose (a beginner can read every
 * line of this file):
 *
 *   LZSS  — general "replace repeated byte sequences with back-references"
 *           decompression, 64 KiB history ring. The flash image is ~6.7 MB
 *           and expands to ~13.3 MB of RLE data.
 *   RLE   — per display line, runs of [count-1][hi][lo] expand into the
 *           960-byte RGB565 line that goes on the wire.
 *
 * Both layers are streamed: at no point does a whole frame exist in RAM.
 * Peak RAM use = 64 KiB ring + 259 B pending + the caller's line buffers.
 */
#include <string.h>
#include "video.h"

#define LZ_RING_SIZE   65536u
#define LZ_MAX_PENDING 259u

typedef struct {
    /* blob */
    const uint8_t *src;        /* LZSS stream (blob + header)      */
    uint32_t       src_len;
    uint32_t       src_pos;
    /* LZSS state */
    uint8_t        ring[LZ_RING_SIZE];
    uint32_t       pos;        /* total bytes decoded so far       */
    uint8_t        flags;
    int            nbits;
    /* bytes of the current token waiting to be served */
    uint8_t        pend[LZ_MAX_PENDING];
    uint16_t       pend_len;
    uint16_t       pend_idx;
    /* header */
    video_hdr_t    hdr;
    uint16_t       line_px;    /* pixels per RLE line: 480 progressive,
                                * 240 field-halves (v6) */
} video_state_t;

static video_state_t s;

/* ---------------- LZSS layer ---------------- */

/* Serve one decoded byte. Returns false when the stream is exhausted. */
static bool lz_byte(uint8_t *out)
{
    if (s.pend_idx < s.pend_len) {
        *out = s.pend[s.pend_idx++];
        return true;
    }

    if (s.nbits == 0) {
        if (s.src_pos >= s.src_len) {
            return false;
        }
        s.flags = s.src[s.src_pos++];
        s.nbits = 8;
    }
    int bit = s.flags & 0x80;
    s.flags = (uint8_t)(s.flags << 1);
    s.nbits--;

    if (bit) {                                    /* literal */
        if (s.src_pos >= s.src_len) {
            return false;
        }
        s.pend[0] = s.src[s.src_pos++];
        s.ring[s.pos & (LZ_RING_SIZE - 1)] = s.pend[0];
        s.pos += 1;
        s.pend_len = 1;
        s.pend_idx = 0;
        *out = s.pend[s.pend_idx++];
        return true;
    }

    /* match: 3 bytes = offset (16 bit, +1) and length (+4) */
    if (s.src_pos + 3 > s.src_len) {
        return false;
    }
    uint32_t off = ((uint32_t)s.src[s.src_pos] << 8 | s.src[s.src_pos + 1]) + 1;
    uint32_t len = (uint32_t)s.src[s.src_pos + 2] + 4;
    s.src_pos += 3;
    if (len > LZ_MAX_PENDING) {                   /* corrupt data guard */
        len = LZ_MAX_PENDING;
    }
    for (uint32_t k = 0; k < len; k++) {
        uint8_t v = s.ring[(s.pos - off + k) & (LZ_RING_SIZE - 1)];
        s.ring[(s.pos + k) & (LZ_RING_SIZE - 1)] = v;
        s.pend[k] = v;
    }
    s.pos += len;
    s.pend_len = (uint16_t)len;
    s.pend_idx = 0;
    *out = s.pend[s.pend_idx++];
    return true;
}

static void lz_reset(void)
{
    s.src_pos = 0;
    s.pos = 0;
    s.flags = 0;
    s.nbits = 0;
    s.pend_len = 0;
    s.pend_idx = 0;
}

/* ---------------- public API ---------------- */

bool video_open(const uint8_t *blob, uint32_t blob_len)
{
    if (blob_len < sizeof(video_hdr_t)) {
        return false;
    }
    /* memcpy: the packed struct may be unaligned-safe, but this way it is
     * guaranteed on every core (the blob lives in flash, mapped read-only) */
    memcpy(&s.hdr, blob, sizeof(video_hdr_t));
    if (memcmp(s.hdr.magic, "MIV2", 4) != 0) {
        return false;
    }
    if (s.hdr.width != 480 || s.hdr.height != 320) {
        return false;                            /* built for this panel */
    }
    if (s.hdr.bpp != 2 && s.hdr.bpp != 3) {
        return false;                            /* 2 = RGB565, 3 = RGB666 */
    }
    const uint16_t scan = s.hdr.flags & 0x6;
    if (scan != 0x2 && scan != 0x4) {
        return false;   /* need bit1 (progressive) or bit2 (field-halves);
                         * bit2 is what the v6 player streams — main.c
                         * rejects bit1 blobs with its own message */
    }
    s.line_px = (scan == 0x4) ? 240 : 480;
    s.src = blob + sizeof(video_hdr_t);
    s.src_len = blob_len - sizeof(video_hdr_t);
    lz_reset();
    return true;
}

const video_hdr_t *video_hdr(void)
{
    return &s.hdr;
}

void video_rewind(void)
{
    lz_reset();
}

bool video_read_line(uint8_t *dst)
{
    const int bpp = (s.hdr.bpp == 3) ? 3 : 2;
    const int width = s.line_px;
    int px = 0;
    while (px < width) {
        uint8_t cb, pxb[3];
        if (!lz_byte(&cb)) {
            return false;                        /* stream exhausted */
        }
        for (int k = 0; k < bpp; k++) {
            if (!lz_byte(&pxb[k])) {
                return false;                    /* stream exhausted */
            }
        }
        int cnt = (int)cb + 1;                   /* stored as count-1: 1..256 */
        int take = cnt;
        if (px + take > width) {                 /* corrupt data guard */
            take = width - px;
        }
        for (int i = 0; i < take; i++) {
            for (int k = 0; k < bpp; k++) {
                *dst++ = pxb[k];
            }
        }
        px += take;
    }
    return true;
}

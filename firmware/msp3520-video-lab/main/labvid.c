/*
 * labvid.c — VLB2 decoder for the video lab (see labvid.h for the format).
 *
 * The LZSS layer is a struct-ified port of the v6/v7 video.c decoder
 * (same token format, same ring semantics — that decoder shipped and was
 * host-verified byte-identical in WORKLOG §6/§7). The RLE layer is the
 * v6/v7 per-line run format with a 1-byte payload (palette index).
 *
 * This file is pure C99 (no ESP-IDF includes): the host gate compiles it
 * with gcc and decodes the whole blob, so the exact device code is what
 * gets verified.
 */
#include <string.h>
#include <stdlib.h>
#include "labvid.h"

typedef struct {
    const uint8_t *src;        /* compressed stream                    */
    uint32_t       src_len;
    uint32_t       src_pos;
    uint32_t       pos;        /* total bytes decoded so far           */
    uint8_t        flags;
    int            nbits;
    uint16_t       pend_len;   /* bytes of the current token pending   */
    uint16_t       pend_idx;
    uint8_t        pend[259];
} lz_head_t;

typedef struct {
    lz_head_t head;
    uint8_t   ring[LABVID_RING_SIZE];
} lz_t;

static const uint8_t *s_data;           /* streams start (past the table) */
static const uint8_t *s_off;           /* offset table (LE u32 entries)   */
static uint32_t        s_frames;
static int             s_w, s_h;
static uint32_t        s_last = 0xFFFFFFFFu;

/* The staging frame is heap-allocated at open: 153.6 KB of static BSS
 * on top of the player buffers overflows dram0_0_seg at link time
 * (this build has ~65 KB more statics than the segment holds). Same
 * RAM, but allocated once at boot when the heap is one contiguous
 * region — and the failure mode becomes a clean message at boot
 * instead of a link error. */
static uint8_t        *s_stage;
static lz_t            s_main;

/* ---------------- LZSS layer (port of video.c) ------------------------- */

static bool lz_byte(lz_t *z, uint8_t *out)
{
    if (z->head.pend_idx < z->head.pend_len) {
        *out = z->head.pend[z->head.pend_idx++];
        return true;
    }

    if (z->head.nbits == 0) {
        if (z->head.src_pos >= z->head.src_len) {
            return false;
        }
        z->head.flags = z->head.src[z->head.src_pos++];
        z->head.nbits = 8;
    }
    int bit = z->head.flags & 0x80;
    z->head.flags = (uint8_t)(z->head.flags << 1);
    z->head.nbits--;

    if (bit) {                                    /* literal */
        if (z->head.src_pos >= z->head.src_len) {
            return false;
        }
        z->head.pend[0] = z->head.src[z->head.src_pos++];
        z->ring[z->head.pos & (LABVID_RING_SIZE - 1)] = z->head.pend[0];
        z->head.pos += 1;
        z->head.pend_len = 1;
        z->head.pend_idx = 0;
        *out = z->head.pend[z->head.pend_idx++];
        return true;
    }

    /* match: 3 bytes = offset (16 bit, +1) and length (+4) */
    if (z->head.src_pos + 3 > z->head.src_len) {
        return false;
    }
    uint32_t off = ((uint32_t)z->head.src[z->head.src_pos] << 8
                  | z->head.src[z->head.src_pos + 1]) + 1;
    uint32_t len = (uint32_t)z->head.src[z->head.src_pos + 2] + 4;
    z->head.src_pos += 3;
    if (len > sizeof(z->head.pend)) {             /* corrupt data guard */
        len = sizeof(z->head.pend);
    }
    if (off > LABVID_RING_SIZE) {                 /* encoder contract   */
        return false;
    }
    for (uint32_t k = 0; k < len; k++) {
        uint8_t v = z->ring[(z->head.pos - off + k) & (LABVID_RING_SIZE - 1)];
        z->ring[(z->head.pos + k) & (LABVID_RING_SIZE - 1)] = v;
        z->head.pend[k] = v;
    }
    z->head.pos += len;
    z->head.pend_len = (uint16_t)len;
    z->head.pend_idx = 0;
    *out = z->head.pend[z->head.pend_idx++];
    return true;
}

static void lz_reset(lz_t *z, const uint8_t *src, uint32_t len)
{
    memset(&z->head, 0, sizeof(z->head));
    z->head.src = src;
    z->head.src_len = len;
}

/* ---------------- RLE layer: one row of index runs ---------------------- */

static bool lz_row(lz_t *z, uint8_t *dst)
{
    int px = 0;
    while (px < s_w) {
        uint8_t cb, idx;
        if (!lz_byte(z, &cb) || !lz_byte(z, &idx)) {
            return false;                          /* stream exhausted   */
        }
        int take = (int)cb + 1;                    /* stored as count-1  */
        if (px + take > s_w) {                     /* corrupt data guard */
            take = s_w - px;
        }
        memset(dst + px, idx, (size_t)take);
        px += take;
    }
    return true;
}

/* ---------------- public API -------------------------------------------- */

bool labvid_open(const uint8_t *blob, uint32_t blob_len)
{
    if (blob_len < 10) {
        return false;
    }
    if (memcmp(blob, "VLB2", 4) != 0) {
        return false;
    }
    uint32_t w = blob[4] | (blob[5] << 8);
    uint32_t h = blob[6] | (blob[7] << 8);
    uint32_t frames = blob[8] | (blob[9] << 8);
    if (frames == 0 || w == 0 || w > LABVID_MAX_W || h == 0 || h > LABVID_MAX_H) {
        return false;
    }
    uint32_t table = 10 + 4 * (frames + 1);
    if (blob_len < table) {
        return false;
    }
    /* offsets: little-endian u32, monotonic, last == payload end */
    const uint8_t *p = blob + 10;
    uint32_t prev = 0;
    for (uint32_t i = 0; i <= frames; i++) {
        uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8
                   | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        p += 4;
        if (v < prev) {
            return false;
        }
        prev = v;
    }
    if (prev != blob_len - table) {
        return false;
    }

    s_off = blob + 10;                 /* byte-wise reads stay aligned-safe */
    s_data = blob + table;             /* offsets are table-relative        */
    s_frames = frames;
    s_w = (int)w;
    s_h = (int)h;
    s_last = 0xFFFFFFFFu;
    if (s_stage == NULL) {
        s_stage = malloc((size_t)w * h);
        if (s_stage == NULL) {
            return false;              /* boot-time heap exhausted          */
        }
    }
    return true;
}

static uint32_t off_at(uint32_t i)
{
    const uint8_t *p = s_off + 4 * i;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8
         | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t labvid_frames(void)
{
    return s_frames;
}

const uint8_t *labvid_stage(uint32_t f)
{
    if (f >= s_frames) {
        return NULL;
    }
    if (f == s_last) {
        return s_stage;                    /* cached — the decode was free */
    }
    lz_reset(&s_main, s_data + off_at(f), off_at(f + 1) - off_at(f));
    uint8_t *dst = s_stage;
    for (int y = 0; y < s_h; y++) {
        if (!lz_row(&s_main, dst)) {
            return NULL;                           /* corrupt stream     */
        }
        dst += s_w;
    }
    s_last = f;
    return s_stage;
}

bool labvid_rows_begin(uint32_t f)
{
    if (f >= s_frames) {
        return false;
    }
    lz_reset(&s_main, s_data + off_at(f), off_at(f + 1) - off_at(f));
    return true;
}

bool labvid_rows_next(uint8_t *dst)
{
    return lz_row(&s_main, dst);
}

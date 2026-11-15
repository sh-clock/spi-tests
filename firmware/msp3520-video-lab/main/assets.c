/*
 * assets.c — parse the embedded lab blobs and expose them to the players.
 */
#include <string.h>
#include <stdio.h>
#include "assets.h"

/* EMBED_FILES symbols (dots in file names become underscores; the asm
 * alias binds the C name to the generated _binary_* symbol) */
extern const uint8_t base_idx_start[]    asm("_binary_base_idx_start");
extern const uint8_t base_idx_end[]      asm("_binary_base_idx_end");
extern const uint8_t pre_i4_idx_start[]  asm("_binary_pre_i4_idx_start");
extern const uint8_t pre_i4_idx_end[]    asm("_binary_pre_i4_idx_end");
extern const uint8_t half_idx_start[]    asm("_binary_half_idx_start");
extern const uint8_t half_idx_end[]      asm("_binary_half_idx_end");
extern const uint8_t third_idx_start[]   asm("_binary_third_idx_start");
extern const uint8_t third_idx_end[]     asm("_binary_third_idx_end");
extern const uint8_t delta_spans_start[] asm("_binary_delta_spans_start");
extern const uint8_t delta_spans_end[]   asm("_binary_delta_spans_end");
extern const uint8_t palette_pal_start[] asm("_binary_palette_pal_start");
extern const uint8_t palette_pal_end[]   asm("_binary_palette_pal_end");

static lab_idx_t    s_base, s_prei4, s_half, s_third;
static lab_delta_t  s_delta;
static uint8_t      s_lut_mem[256][3];
const uint8_t      (*lab_lut)[3] = s_lut_mem;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static bool parse_idx(const uint8_t *start, const uint8_t *end,
                      const char *magic, uint16_t phases,
                      const char *name, lab_idx_t *out)
{
    size_t len = (size_t)(end - start);
    size_t hdr = 10 + (phases > 1 ? 2 : 0);
    if (len < hdr || memcmp(start, magic, 4) != 0) {
        printf("assets: %s bad magic/length\n", name);
        return false;
    }
    out->w      = rd16(start + 4);
    out->h      = rd16(start + 6);
    out->frames = rd16(start + 8);
    out->phases = phases;
    out->data   = start + hdr;
    size_t want = (size_t)out->frames * out->phases
                * (out->h / phases) * out->w;
    if (out->frames == 0 || len != hdr + want) {
        printf("assets: %s size mismatch (have %u, want %u)\n", name,
               (unsigned)len, (unsigned)(hdr + want));
        return false;
    }
    return true;
}

bool lab_assets_init(void)
{
    if (!parse_idx(base_idx_start, base_idx_end, "VLB1", 1, "base.idx", &s_base)
     || !parse_idx(pre_i4_idx_start, pre_i4_idx_end, "VLP1", 4, "pre_i4.idx", &s_prei4)
     || !parse_idx(half_idx_start, half_idx_end, "VLB1", 1, "half.idx", &s_half)
     || !parse_idx(third_idx_start, third_idx_end, "VLB1", 1, "third.idx", &s_third)) {
        return false;
    }

    size_t plen = (size_t)(palette_pal_end - palette_pal_start);
    size_t dlen = (size_t)(delta_spans_end - delta_spans_start);
    if (plen != 768) {
        printf("assets: palette.pal is %u B, want 768\n", (unsigned)plen);
        return false;
    }
    if (dlen < 10 || memcmp(delta_spans_start, "VLD1", 4) != 0) {
        printf("assets: delta.spans bad magic/length\n");
        return false;
    }
    s_delta.blob     = delta_spans_start;
    s_delta.frames   = delta_spans_start + 10;
    s_delta.frames_n = rd16(delta_spans_start + 8);
    if (s_delta.frames_n == 0) {
        printf("assets: delta.spans has no frames\n");
        return false;
    }

    memcpy(s_lut_mem, palette_pal_start, 768);

    printf("assets: base %uf %ux%u | pre-i4 %uf x%u fields | half %uf %uw"
           " | third %uf %uw | delta %uf | palette 256\n",
           s_base.frames, s_base.w, s_base.h,
           s_prei4.frames, s_prei4.phases,
           s_half.frames, s_half.w,
           s_third.frames, s_third.w,
           s_delta.frames_n);
    return true;
}

const lab_idx_t *lab_base(void)   { return &s_base; }
const lab_idx_t *lab_prei4(void)  { return &s_prei4; }
const lab_idx_t *lab_half(void)   { return &s_half; }
const lab_idx_t *lab_third(void)  { return &s_third; }
const lab_delta_t *lab_delta(void) { return &s_delta; }

/*
 * assets.c — parse the embedded lab blobs and expose them to the players.
 */
#include <string.h>
#include <stdio.h>
#include "assets.h"
#include "labvid.h"

/* EMBED_FILES symbols (dots in file names become underscores; the asm
 * alias binds the C name to the generated _binary_* symbol) */
extern const uint8_t base_vlb2_start[]   asm("_binary_base_vlb2_start");
extern const uint8_t base_vlb2_end[]     asm("_binary_base_vlb2_end");
extern const uint8_t palette_pal_start[] asm("_binary_palette_pal_start");
extern const uint8_t palette_pal_end[]   asm("_binary_palette_pal_end");
extern const uint8_t merge2_bin_start[]  asm("_binary_merge2_bin_start");
extern const uint8_t merge2_bin_end[]    asm("_binary_merge2_bin_end");
extern const uint8_t mbmap_bin_start[]   asm("_binary_mbmap_bin_start");
extern const uint8_t mbmap_bin_end[]     asm("_binary_mbmap_bin_end");

static uint8_t s_lut_mem[256][3];
const uint8_t (*lab_lut)[3] = s_lut_mem;

static const uint8_t *s_merge2;
static const uint8_t *s_mbmap;
static uint16_t s_mbmap_frames;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool lab_assets_init(void)
{
    size_t base_len = (size_t)(base_vlb2_end - base_vlb2_start);
    if (!labvid_open(base_vlb2_start, (uint32_t)base_len)) {
        printf("assets: base.vlb2 failed validation\n");
        return false;
    }

    size_t plen = (size_t)(palette_pal_end - palette_pal_start);
    if (plen != 768) {
        printf("assets: palette.pal is %u B, want 768\n", (unsigned)plen);
        return false;
    }
    memcpy(s_lut_mem, palette_pal_start, 768);

    size_t mlen = (size_t)(merge2_bin_end - merge2_bin_start);
    if (mlen != 65536) {
        printf("assets: merge2.bin is %u B, want 65536\n", (unsigned)mlen);
        return false;
    }
    s_merge2 = merge2_bin_start;

    size_t blen = (size_t)(mbmap_bin_end - mbmap_bin_start);
    if (blen < 10 || memcmp(mbmap_bin_start, "VLM1", 4) != 0) {
        printf("assets: mbmap.bin bad magic/length\n");
        return false;
    }
    uint16_t bw = rd16(mbmap_bin_start + 4);
    uint16_t bh = rd16(mbmap_bin_start + 6);
    s_mbmap_frames = rd16(mbmap_bin_start + 8);
    if (bw != 30 || bh != 20 || s_mbmap_frames == 0
        || blen != 10 + (size_t)s_mbmap_frames * 75) {
        printf("assets: mbmap.bin geometry %ux%u x%uf, size %u\n",
               bw, bh, s_mbmap_frames, (unsigned)blen);
        return false;
    }
    s_mbmap = mbmap_bin_start + 10;

    if (s_mbmap_frames != labvid_frames()) {
        printf("assets: mbmap has %u frames, base has %lu\n",
               s_mbmap_frames, (unsigned long)labvid_frames());
        return false;
    }

    printf("assets: base %lu frames (compressed) | palette 256 | merge2 64K"
           " | mbmap %lu x 600 bits\n",
           (unsigned long)labvid_frames(),
           (unsigned long)s_mbmap_frames);
    return true;
}

uint32_t lab_base_frames(void)
{
    return labvid_frames();
}

const uint8_t *lab_merge2(void)
{
    return s_merge2;
}

const uint8_t *lab_mbmap(void)
{
    return s_mbmap;
}

uint16_t lab_mbmap_frames(void)
{
    return s_mbmap_frames;
}

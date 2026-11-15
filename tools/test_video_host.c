/* Host-side test harness for main/video.c — NOT part of the firmware.
 * Decodes the whole blob with the exact device code and dumps every
 * display line; scripts/host_test_video.py compares it against the
 * Python reference expansion. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "video.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <blob> <out lines>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open blob"); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)len);
    if (fread(blob, 1, (size_t)len, f) != (size_t)len) { perror("read"); return 2; }
    fclose(f);

    if (!video_open(blob, (uint32_t)len)) {
        fprintf(stderr, "video_open failed\n");
        return 1;
    }
    const video_hdr_t *h = video_hdr();
    printf("hdr: %ux%u, %u frames, %u fields, rle_len=%u\n",
           h->width, h->height, h->frames, h->fields, h->rle_len);

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror("open out"); return 2; }
    static uint8_t line[VIDEO_LINE_BYTES];
    uint32_t unit_lines = (h->flags & 0x2) ? h->height : h->height / 2;
    uint32_t total_lines = h->fields * unit_lines;
    uint32_t ok_lines = 0;
    for (uint32_t i = 0; i < total_lines; i++) {
        if (!video_read_line(line)) {
            fprintf(stderr, "decode ended early at line %u of %u\n", i, total_lines);
            break;
        }
        fwrite(line, 1, (size_t)(480 * h->bpp), o);
        ok_lines++;
    }
    fclose(o);
    printf("decoded %u/%u lines (%u bytes)\n",
           ok_lines, total_lines, (unsigned)(ok_lines * 480 * h->bpp));
    return ok_lines == total_lines ? 0 : 1;
}

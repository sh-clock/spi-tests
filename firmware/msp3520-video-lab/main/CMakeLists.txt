# The "main" component: lab code + the embedded asset blobs.
# base.vlb2 carries the WHOLE clip (per-frame RLE+LZSS); the other blobs
# are the palette LUT, the index-merge LUT (11a/11b runtime decimation)
# and the macroblock dirty bitmaps (technique 8).
idf_component_register(
    SRCS "main.c" "msp3520.c" "players.c" "assets.c" "labvid.c"
    INCLUDE_DIRS "."
    # EMBED_FILES places each blob into the firmware image and exposes it
    # as _binary_<name>_start/_end symbols (dots become underscores).
    EMBED_FILES "base.vlb2" "palette.pal" "merge2.bin" "mbmap.bin"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_uart esp_timer
)

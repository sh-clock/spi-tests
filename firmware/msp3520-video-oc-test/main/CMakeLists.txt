# The "main" component: application code + the embedded video.
idf_component_register(
    SRCS "main.c" "msp3520.c" "video.c"
    INCLUDE_DIRS "."
    # EMBED_FILES places video.bin into the firmware image and exposes it
    # as _binary_video_bin_start / _binary_video_bin_end symbols.
    EMBED_FILES "video.bin"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_uart esp_timer
)

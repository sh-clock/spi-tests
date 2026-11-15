# The "main" component: lab code + the six embedded asset blobs.
idf_component_register(
    SRCS "main.c" "msp3520.c" "players.c" "assets.c"
    INCLUDE_DIRS "."
    # EMBED_FILES places each blob into the firmware image and exposes it
    # as _binary_<name>_start/_end symbols (dots become underscores).
    EMBED_FILES "base.idx" "pre_i4.idx" "half.idx" "third.idx"
                "delta.spans" "palette.pal"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_uart esp_timer
)

idf_component_register(
    SRCS "src/sparkbot_lvgl_display.cc" "src/sparkbot_lvgl_renderer.cc" "src/sparkbot_emoji_assets.cc"
         "src/gif/gifdec.c" "src/gif/lvgl_gif.cc"
    INCLUDE_DIRS "include" "src"
    REQUIRES voicelife_contracts voicelife_voice
    PRIV_REQUIRES esp_driver_spi esp_lcd esp_partition spi_flash
)

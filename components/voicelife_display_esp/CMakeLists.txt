idf_component_register(
    SRCS "src/ssd1306_status_display.cc" "src/ssd1306_presentation_adapter.cc" "src/sparkbot_presentation_adapter.cc"
    INCLUDE_DIRS "include" "src" "src/generated"
    REQUIRES voicelife_contracts voicelife_voice
    PRIV_REQUIRES esp_lcd driver
)

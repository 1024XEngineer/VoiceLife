idf_component_register(
    SRCS "src/ssd1306_status_display.cc"
    INCLUDE_DIRS "include" "src" "src/generated"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES esp_lcd driver
)

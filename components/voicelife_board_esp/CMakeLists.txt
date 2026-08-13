idf_component_register(
    SRCS
        "src/sparkbot_profile.cc"
        "src/gpio46_power_arbiter.cc"
        "src/esp32s3_board_probe.cc"
        "src/sparkbot_wake_model_assets.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES esp_hw_support esp_partition esp_psram esp_system spi_flash
)

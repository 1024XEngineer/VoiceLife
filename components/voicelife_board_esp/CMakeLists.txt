idf_component_register(
    SRCS
        "src/sparkbot_profile.cc"
        "src/sparkbot_imu.cc"
        "src/gpio46_power_arbiter.cc"
        "src/esp32s3_board_probe.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES esp_hw_support esp_partition esp_psram esp_system esp_timer spi_flash esp_driver_i2c freertos
                  espressif2022__bmi270 espressif__i2c_bus
)

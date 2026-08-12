idf_component_register(
    SRCS "main.cc" "platform_assemblies.cc"
    INCLUDE_DIRS "."
    PRIV_REQUIRES log voicelife_runtime voicelife_im voicelife_contracts voicelife_voice voicelife_board_esp voicelife_display_esp voicelife_display_sparkbot esp_driver_gpio
)

idf_component_register(
    SRCS "src/runtime.cc" "src/im_runtime_bootstrap.cc" "src/linx_mcp_bridge.cc" "src/linx_ota_bootstrap.cc" "src/schedule_mcp_tools.cc"
    INCLUDE_DIRS "include" "src"
    REQUIRES voicelife_contracts voicelife_voice voicelife_audio_esp
    PRIV_REQUIRES voicelife_mcp voicelife_linx voicelife_linx_esp voicelife_audio_esp voicelife_schedule voicelife_im nvs_flash nvs_sec_provider esp_timer esp_http_client esp-tls esp_wifi esp_netif lwip esp_event spi_flash esp_partition esp_psram esp_app_format esp_driver_gpio esp_driver_usb_serial_jtag led_strip
)

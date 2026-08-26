idf_component_register(
    SRCS "src/runtime.cc" "src/bootstrap/storage_bootstrap.cc" "src/im_runtime_bootstrap.cc"
         "src/schedule_reminder_im_adapter.cc"
         "src/linx_mcp_bridge.cc" "src/linx_ota_bootstrap.cc" "src/wifi_provisioning.cc"
         "src/wifi_provisioning_esp.cc"
         "src/im_binding_mcp_tools.cc" "src/im_binding_presentation.cc"
         "src/serial_voice_test.cc" "src/usb_serial_frame_router.cc"
    INCLUDE_DIRS "include" "src"
    PRIV_INCLUDE_DIRS "../voicelife_im/src/transport"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES voicelife_application voicelife_runtime_esp voicelife_mcp voicelife_voice voicelife_linx
                  voicelife_linx_esp voicelife_audio_esp voicelife_storage_memory
                  voicelife_display_esp voicelife_schedule voicelife_im voicelife_storage_fatfs
                  voicelife_storage_sqlite voicelife_timing_esp nvs_flash nvs_sec_provider esp_timer esp_http_client esp-tls esp_wifi
                  esp_netif lwip esp_event esp_http_server spi_flash esp_partition esp_psram esp_app_format esp_driver_gpio
                  esp_driver_usb_serial_jtag led_strip
)

if(NOT CONFIG_VOICELIFE_STORAGE_FATFS OR NOT CONFIG_VOICELIFE_STORAGE_SQLITE)
    message(FATAL_ERROR "设备 Runtime 必须同时启用 FATFS 与 SQLite 持久化存储")
endif()

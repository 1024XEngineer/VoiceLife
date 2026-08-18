idf_component_register(
    SRCS "src/esp_interaction_task_host.cc" "src/esp_runtime.cc" "src/esp_runtime_workers.cc"
         "src/esp_runtime_board.cc" "src/esp_runtime_interaction.cc" "src/esp_runtime_event_loop.cc"
         "src/bootstrap/storage_bootstrap.cc"
         "src/im_runtime_bootstrap.cc" "src/linx_mcp_bridge.cc" "src/linx_ota_bootstrap.cc"
         "src/wifi_provisioning.cc" "src/wifi_provisioning_esp.cc" "src/im_binding_mcp_tools.cc"
         "src/im_binding_presentation.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../voicelife_runtime/include" "src"
    REQUIRES voicelife_application voicelife_contracts
    PRIV_REQUIRES voicelife_mcp voicelife_voice voicelife_linx voicelife_linx_esp voicelife_audio_esp
                  voicelife_display_esp voicelife_schedule voicelife_im voicelife_storage_fatfs
                  voicelife_storage_sqlite nvs_flash nvs_sec_provider esp_timer esp_http_client esp-tls esp_wifi
                  esp_netif lwip esp_event esp_http_server spi_flash esp_partition esp_psram esp_app_format
                  esp_driver_gpio esp_driver_usb_serial_jtag led_strip freertos
)

if(NOT CONFIG_VOICELIFE_STORAGE_FATFS OR NOT CONFIG_VOICELIFE_STORAGE_SQLITE)
    message(FATAL_ERROR "Runtime 日程持久化必须同时启用 VOICELIFE_STORAGE_FATFS 和 VOICELIFE_STORAGE_SQLITE")
endif()

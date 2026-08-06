idf_component_register(
    SRCS
        "src/websocket_fragment_assembler.cc"
        "src/esp_websocket_transport.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts voicelife_linx
    PRIV_REQUIRES esp_websocket_client esp-tls esp_event esp_timer
)

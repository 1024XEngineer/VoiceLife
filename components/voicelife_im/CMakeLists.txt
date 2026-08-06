idf_component_register(
    SRCS
        "src/im_reporting_channel.cc"
        "src/im_action_channel.cc"
        "src/im_sse.cc"
        "src/im_wire.cc"
        "src/im_endpoint.cc"
        "src/transport/esp_http_transport.cc"
        "src/transport/esp_action_stream_transport.cc"
        "src/transport/esp_clock.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES esp_http_client mbedtls
)

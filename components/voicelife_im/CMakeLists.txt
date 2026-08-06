idf_component_register(
    SRCS
        "src/im_reporting_channel.cc"
        "src/im_wire.cc"
        "src/transport/esp_http_transport.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES esp_http_client mbedtls
)

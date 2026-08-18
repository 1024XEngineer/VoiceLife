idf_component_register(
    SRCS "src/esp_timing_runtime.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts voicelife_timing
    PRIV_REQUIRES esp_timer freertos
)

idf_component_register(
    SRCS
        "src/audio_board_profile.cc"
        "src/esp32s3_audio_probe.cc"
        "src/pcm_frame_assembler.cc"
        "src/esp32s3_pcm_audio_port.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts voicelife_voice
    PRIV_REQUIRES esp_driver_i2c esp_driver_i2s
)

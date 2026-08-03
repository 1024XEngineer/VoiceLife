idf_component_register(
    SRCS
        "src/linx_json_codec.cc"
        "src/linx_speech_provider.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts voicelife_voice
)

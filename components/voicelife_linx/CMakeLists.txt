idf_component_register(
    SRCS
        "src/linx_json_codec.cc"
        "src/linx_json_reader.cc"
        "src/linx_ota.cc"
        "src/linx_speech_provider.cc"
        "../../third_party/cjson/cJSON.c"
    INCLUDE_DIRS "include" "../../third_party/cjson"
    REQUIRES voicelife_contracts voicelife_voice
)

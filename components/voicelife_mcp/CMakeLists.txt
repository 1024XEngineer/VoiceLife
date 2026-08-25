idf_component_register(
    SRCS "src/mcp_server.cc" "src/mcp_json_writer.cc" "src/tools/audio_mcp_tools.cc" "src/tools/schedule_mcp_tools.cc"
         "src/tools/schedule_mcp_tools_input.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts voicelife_im
    PRIV_REQUIRES voicelife_schedule yyjson
)

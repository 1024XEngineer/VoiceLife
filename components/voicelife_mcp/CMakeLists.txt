idf_component_register(
    SRCS "src/mcp_server.cc" "src/mcp_json_writer.cc" "src/tools/schedule_rule_mcp_tools.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES voicelife_schedule yyjson
)

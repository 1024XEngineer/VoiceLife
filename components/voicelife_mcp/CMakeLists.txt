idf_component_register(
    SRCS "src/mcp_server.cc" "src/mcp_json_writer.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES yyjson
)

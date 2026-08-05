idf_component_register(
    SRCS "src/mcp_server.cc"
    INCLUDE_DIRS "include"
    REQUIRES json voicelife_application voicelife_contracts
)

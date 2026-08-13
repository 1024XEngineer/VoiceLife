idf_component_register(
    SRCS "src/mcp_server.cc" "src/mcp_json_writer.cc" "src/json_rpc_endpoint.cc" "src/mcp_request_executor.cc" "src/schedule_tools.cc"
         "src/mcp_schedule_application.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts voicelife_schedule
    PRIV_REQUIRES yyjson freertos
)

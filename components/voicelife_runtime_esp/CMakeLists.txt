idf_component_register(
    SRCS "src/esp_interaction_task_host.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_application
    PRIV_REQUIRES voicelife_mcp freertos
)

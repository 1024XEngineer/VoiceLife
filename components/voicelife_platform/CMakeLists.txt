idf_component_register(
    SRCS "src/in_memory_calendar_store.cc" "src/sequential_id_generator.cc" "src/system_clock.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts voicelife_application
)

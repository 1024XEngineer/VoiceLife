idf_component_register(
    SRCS "src/timing_task_loop.cc"
    INCLUDE_DIRS "include"
    REQUIRES freertos voicelife_contracts voicelife_timing
    PRIV_REQUIRES log
)

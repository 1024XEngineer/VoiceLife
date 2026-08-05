idf_component_register(
    SRCS
        "src/timing_policy.cc"
        "src/timing_task_service.cc"
        "src/timing_task_service_stubs.cc"
        "src/timing_task_state.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
)

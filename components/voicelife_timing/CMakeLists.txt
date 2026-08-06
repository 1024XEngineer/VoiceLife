idf_component_register(
    SRCS
        "src/timing_policy.cc"
        "src/timing_task_service.cc"
        "src/timing_task_service_calendar.cc"
        "src/timing_task_service_reminder_triggers.cc"
        "src/timing_task_service_reminder_rules.cc"
        "src/timing_task_service_stubs.cc"
        "src/timing_task_state.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
)

idf_component_register(
    SRCS
        "src/occurrence_planner.cc"
        "src/timing_policy.cc"
        "src/timing_task_service.cc"
        "src/timing_task_service_advance.cc"
        "src/timing_task_service_calendar.cc"
        "src/timing_task_service_delete_reminder_rule.cc"
        "src/timing_task_service_reminder_triggers.cc"
        "src/timing_task_service_reminder_rules.cc"
        "src/timing_task_service_dismiss_reminder_trigger.cc"
        "src/timing_task_service_snooze_reminder_trigger.cc"
        "src/timing_task_state.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
)

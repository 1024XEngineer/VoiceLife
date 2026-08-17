idf_component_register(
    SRCS
        "src/helpers/schedule_create_helpers.cc"
        "src/mock/schedule_mock_data.cc"
        "src/helpers/schedule_occurrence_helpers.cc"
        "src/helpers/schedule_query_helpers.cc"
        "src/helpers/schedule_operation_helpers.cc"
        "src/mock/schedule_operation_mock_data.cc"
        "src/helpers/schedule_operation_query_helpers.cc"
        "src/helpers/schedule_rule_result_helpers.cc"
        "src/helpers/schedule_rule_update_helpers.cc"
        "src/service/schedule_service.cc"
        "src/service/schedule_operation_service.cc"
        "src/service/schedule_rule_service_helpers.cc"
        "src/service/schedule_rule_service.cc"
        "src/factory/schedule_factory.cc"
        "src/rules/schedule_time_rules.cc"
        "src/calendar.cc"
        "src/rules/recurrence_planner.cc"
        "src/helpers/schedule_undo_helpers.cc"
        "src/helpers/schedule_update_helpers.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
)

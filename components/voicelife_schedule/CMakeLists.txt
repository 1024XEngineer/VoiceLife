idf_component_register(
    SRCS
        "src/schedule_create_helpers.cc"
        "src/schedule_mock_data.cc"
        "src/schedule_query_helpers.cc"
        "src/schedule_operation_helpers.cc"
        "src/schedule_operation_mock_data.cc"
        "src/schedule_operation_query_helpers.cc"
        "src/schedule_service.cc"
        "src/schedule_time_rules.cc"
        "src/schedule_undo_helpers.cc"
        "src/schedule_update_helpers.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
)

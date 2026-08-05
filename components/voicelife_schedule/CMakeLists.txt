idf_component_register(
    SRCS
        "src/schedule_create_helpers.cc"
        "src/schedule_mock_data.cc"
        "src/schedule_query_helpers.cc"
        "src/schedule_service.cc"
        "src/schedule_time_rules.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
)

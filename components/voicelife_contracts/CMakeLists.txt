idf_component_register(
    SRCS "src/status.cc" "src/json.cc" "src/im/notification_intent.cc" "src/im/schedule_receipt.cc"
         "src/im/schedule_query_result.cc"
         "src/im/reminder_action_result.cc" "src/im/reminder_action_command.cc"
         "src/im/voice_reminder_action_status.cc"
         "src/im/notification_submission.cc" "src/im/pairing_session.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES yyjson
)

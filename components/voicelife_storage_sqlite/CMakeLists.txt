set(voicelife_storage_sqlite_sources)
if(CONFIG_VOICELIFE_STORAGE_SQLITE)
    list(APPEND voicelife_storage_sqlite_sources
        "src/mapping/schedule_row_mapper.cc"
        "src/mapping/schedule_reminder_task_row_mapper.cc"
        "src/mapping/operation_row_mapper.cc"
        "src/mapping/schedule_rule_row_mapper.cc"
        "src/mapping/schedule_exception_row_mapper.cc"
        "src/schema/migrations/v001_create_schedule.cc"
        "src/schema/migrations/v002_create_schedule_operation.cc"
        "src/schema/migrations/v003_create_schedule_rule.cc"
        "src/schema/migrations/v004_create_operation_record.cc"
        "src/schema/migrations/v005_add_schedule_reminder_task_id.cc"
        "src/schema/migrations/v006_create_schedule_reminder_task.cc"
        "src/schema/migrations/v007_add_schedule_reminder_task_event.cc"
        "src/schema/migrations/v008_add_reminder_action_state.cc"
        "src/schema/sqlite_schema.cc"
        "src/schema/voicelife_schema.cc"
        "src/sql/operation_sql.cc"
        "src/sql/schedule_sql.cc"
        "src/sql/schedule_reminder_task_sql.cc"
        "src/sql/schedule_rule_sql.cc"
        "src/sql/schedule_exception_sql.cc"
        "src/sqlite_database.cc"
        "src/sqlite_schedule_repository.cc"
        "src/sqlite_schedule_reminder_task_repository.cc"
        "src/sqlite_schedule_rule_repository.cc"
    )
endif()

# ESP-IDF 在 Kconfig 值可用前解析组件依赖图；不能用
# CONFIG_VOICELIFE_STORAGE_SQLITE 条件化 sqlite3 依赖，否则启用 SQLite 的第二次
# 配置会加入源码，却不会把 sqlite3 的 include path 和链接依赖带进来。
# 固件构建入口会先运行 scripts/prepare_sqlite.py，提供这个固定本地组件。
idf_component_register(
    SRCS ${voicelife_storage_sqlite_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES voicelife_contracts voicelife_schedule
    PRIV_REQUIRES sqlite3
)

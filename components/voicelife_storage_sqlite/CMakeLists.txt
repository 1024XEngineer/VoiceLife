set(voicelife_storage_sqlite_sources)
set(voicelife_storage_sqlite_private_requires)

if(CONFIG_VOICELIFE_STORAGE_SQLITE)
    list(APPEND voicelife_storage_sqlite_private_requires sqlite3)
    list(APPEND voicelife_storage_sqlite_sources
        "src/mapping/schedule_row_mapper.cc"
        "src/mapping/operation_row_mapper.cc"
        "src/mapping/schedule_rule_row_mapper.cc"
        "src/mapping/schedule_exception_row_mapper.cc"
        "src/schema/migrations/v001_create_schedule.cc"
        "src/schema/migrations/v002_create_schedule_operation.cc"
        "src/schema/migrations/v003_create_schedule_rule.cc"
        "src/schema/migrations/v004_create_operation_record.cc"
        "src/schema/sqlite_schema.cc"
        "src/schema/voicelife_schema.cc"
        "src/sql/operation_sql.cc"
        "src/sql/schedule_sql.cc"
        "src/sql/schedule_rule_sql.cc"
        "src/sql/schedule_exception_sql.cc"
        "src/sqlite_database.cc"
        "src/sqlite_schedule_repository.cc"
        "src/sqlite_schedule_rule_repository.cc"
    )
endif()

# SQLite 被禁用时不声明 managed component 依赖，内存 Storage Profile 不应解析无用的 sqlite3。
idf_component_register(
    SRCS ${voicelife_storage_sqlite_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES voicelife_contracts voicelife_schedule
    PRIV_REQUIRES ${voicelife_storage_sqlite_private_requires}
)

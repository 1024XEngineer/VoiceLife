set(voicelife_storage_sqlite_sources)

if(CONFIG_VOICELIFE_STORAGE_SQLITE)
    list(APPEND voicelife_storage_sqlite_sources
        "src/mapping/schedule_row_mapper.cc"
        "src/mapping/operation_row_mapper.cc"
        "src/mapping/schedule_rule_row_mapper.cc"
        "src/mapping/schedule_exception_row_mapper.cc"
        "src/schema/migrations/v001_create_schedule.cc"
        "src/schema/migrations/v002_create_schedule_operation.cc"
        "src/schema/migrations/v003_create_schedule_rule.cc"
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

# 私有依赖必须无条件声明：ESP-IDF 的早期组件扫描阶段不加载 sdkconfig，
# 若把 PRIV_REQUIRES 放入 CONFIG 分支，启用配置后不会将 sqlite3 纳入构建图。
idf_component_register(
    SRCS ${voicelife_storage_sqlite_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES voicelife_contracts voicelife_schedule
    PRIV_REQUIRES sqlite3
)

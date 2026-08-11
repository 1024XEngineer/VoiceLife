set(voicelife_storage_sqlite_sources)
set(voicelife_storage_sqlite_private_requires)

if(CONFIG_VOICELIFE_STORAGE_SQLITE)
    list(APPEND voicelife_storage_sqlite_sources
        "src/mapping/schedule_row_mapper.cc"
        "src/sql/schedule_sql.cc"
        "src/sqlite_database.cc"
        "src/sqlite_schedule_repository.cc"
    )
    list(APPEND voicelife_storage_sqlite_private_requires sqlite3)
endif()

idf_component_register(
    SRCS ${voicelife_storage_sqlite_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES voicelife_contracts voicelife_schedule
    PRIV_REQUIRES ${voicelife_storage_sqlite_private_requires}
)

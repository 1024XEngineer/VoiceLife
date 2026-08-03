idf_component_register(
    SRCS "src/sqlite_timing_task_store.cc" "src/sqlite_timing_task_store_driver.cc"
    INCLUDE_DIRS "include"
    REQUIRES voicelife_timing
    PRIV_REQUIRES sqlite3
)

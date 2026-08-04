idf_component_register(
    SRCS "main.cc"
    REQUIRES sqlite3 fatfs wear_levelling esp_timer esp_system
    PRIV_REQUIRES esp_app_format
)

idf_component_register(
    SRCS "src/runtime.cc" "src/bootstrap/storage_bootstrap.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES voicelife_mcp voicelife_voice voicelife_linx voicelife_linx_esp voicelife_audio_esp
                  voicelife_storage_fatfs voicelife_storage_sqlite
)

if(CONFIG_VOICELIFE_STORAGE_FATFS AND NOT CONFIG_VOICELIFE_STORAGE_SQLITE)
    message(FATAL_ERROR "Runtime 存储 Profile 必须同时启用 VOICELIFE_STORAGE_FATFS 和 VOICELIFE_STORAGE_SQLITE")
endif()

if(CONFIG_VOICELIFE_STORAGE_SQLITE AND NOT CONFIG_VOICELIFE_STORAGE_FATFS)
    message(FATAL_ERROR "Runtime 存储 Profile 必须同时启用 VOICELIFE_STORAGE_FATFS 和 VOICELIFE_STORAGE_SQLITE")
endif()

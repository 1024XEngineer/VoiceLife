set(voicelife_storage_fatfs_sources)
if(CONFIG_VOICELIFE_STORAGE_FATFS)
    list(APPEND voicelife_storage_fatfs_sources "src/fatfs_volume.cc")
endif()

# 私有依赖必须无条件声明：ESP-IDF 的早期组件扫描阶段不加载 sdkconfig，
# 若把 PRIV_REQUIRES 放入 CONFIG 分支，启用配置后实际编译可能找不到 IDF 头文件。
idf_component_register(
    SRCS ${voicelife_storage_fatfs_sources}
    INCLUDE_DIRS "include"
    REQUIRES voicelife_contracts
    PRIV_REQUIRES fatfs esp_partition
)

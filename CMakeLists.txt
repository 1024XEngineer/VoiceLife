cmake_minimum_required(VERSION 3.22)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/third_party/yyjson")

# SQLite 源码由根目录固定版本准备脚本生成，供正式固件和实板探针复用。
set(VOICELIFE_SQLITE_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/sqlite3")
if(EXISTS "${VOICELIFE_SQLITE_COMPONENT_DIR}/CMakeLists.txt")
    list(APPEND EXTRA_COMPONENT_DIRS "${VOICELIFE_SQLITE_COMPONENT_DIR}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "0.1.0")
project(voicelife)

# SparkBot assets 分区镜像：构建期按官方格式生成（12B 头 + 44B/项表 + ZZ 数据）。
# 这是 SparkBot 的板级产物，只包含显示 GIF 和官方文本字体。ESP-SR MultiNet
# 模型由 managed component 生成到独立的 model 分区，PCB Profile 不生成、也不依赖它。
if(CONFIG_VOICELIFE_BOARD_ESP_SPARKBOT)
    set(SPARKBOT_GIF_DIR "${CMAKE_SOURCE_DIR}/components/voicelife_display_esp/assets/esp-sparkbot/mascot/gifs")
    set(SPARKBOT_COMMON_FONT "${CMAKE_SOURCE_DIR}/components/voicelife_display_esp/assets/esp-sparkbot/fonts/font_noto_sans_common_16_4.bin")
    set(SPARKBOT_ASSETS_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/build_sparkbot_assets.py")
    file(GLOB SPARKBOT_GIF_FILES "${SPARKBOT_GIF_DIR}/*.gif")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/sparkbot_assets.bin"
        COMMAND python3 "${SPARKBOT_ASSETS_SCRIPT}"
                --gif-dir "${SPARKBOT_GIF_DIR}"
                --common-font "${SPARKBOT_COMMON_FONT}"
                --output "${CMAKE_BINARY_DIR}/sparkbot_assets.bin"
        DEPENDS "${SPARKBOT_ASSETS_SCRIPT}" "${SPARKBOT_COMMON_FONT}" ${SPARKBOT_GIF_FILES}
        COMMENT "生成 SparkBot assets 分区镜像"
    )
    add_custom_target(sparkbot_assets ALL DEPENDS "${CMAKE_BINARY_DIR}/sparkbot_assets.bin")
endif()

# SQLite 业务数据独立于 SparkBot 显示 assets/model 分区。镜像仅供首次受控
# 初始化或实板恢复使用；既不加入默认构建，也不加入 flash。输出文件不能命名为
# voicelife.bin，否则会覆盖同目录的应用固件。
if(CONFIG_VOICELIFE_STORAGE_FATFS)
    partition_table_get_partition_info(VOICELIFE_STORAGE_SIZE "--partition-name voicelife" "size")
    if(NOT VOICELIFE_STORAGE_SIZE)
        message(FATAL_ERROR "已启用 FATFS storage，但分区表中不存在 voicelife 分区")
    endif()
    idf_build_get_property(VOICELIFE_IDF_PATH IDF_PATH)
    idf_build_get_property(VOICELIFE_PYTHON PYTHON)
    set(VOICELIFE_STORAGE_INITIAL_IMAGE "${CMAKE_BINARY_DIR}/voicelife_initial.bin")
    add_custom_command(
        OUTPUT "${VOICELIFE_STORAGE_INITIAL_IMAGE}"
        COMMAND "${VOICELIFE_PYTHON}" "${VOICELIFE_IDF_PATH}/components/fatfs/wl_fatfsgen.py"
                "${CMAKE_SOURCE_DIR}/components/voicelife_storage_fatfs/initial_volume"
                --long_name_support --use_default_datetime --partition_size "${VOICELIFE_STORAGE_SIZE}"
                --output_file "${VOICELIFE_STORAGE_INITIAL_IMAGE}" --sector_size 4096
        DEPENDS "${CMAKE_SOURCE_DIR}/components/voicelife_storage_fatfs/initial_volume/.gitkeep"
        COMMENT "生成首次初始化用 VoiceLife 日程数据卷镜像"
    )
    add_custom_target(voicelife_storage_image DEPENDS "${VOICELIFE_STORAGE_INITIAL_IMAGE}")
endif()

cmake_minimum_required(VERSION 3.22)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/third_party/yyjson")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "0.1.0")
project(voicelife)

# SparkBot assets 分区镜像：构建期按官方格式生成（12B 头 + 44B/项表 + ZZ 数据）。
# 生成物记录 SHA-256；烧录到 assets 分区属于实板阶段，不在本构建内写入。
set(SPARKBOT_GIF_DIR "${CMAKE_SOURCE_DIR}/components/voicelife_display_esp/assets/esp-sparkbot/mascot/gifs")
set(SPARKBOT_ASSETS_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/build_sparkbot_assets.py")
file(GLOB SPARKBOT_GIF_FILES "${SPARKBOT_GIF_DIR}/*.gif")
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/sparkbot_assets.bin"
    COMMAND python3 "${SPARKBOT_ASSETS_SCRIPT}"
            --gif-dir "${SPARKBOT_GIF_DIR}"
            --output "${CMAKE_BINARY_DIR}/sparkbot_assets.bin"
    DEPENDS "${SPARKBOT_ASSETS_SCRIPT}" ${SPARKBOT_GIF_FILES}
    COMMENT "生成 SparkBot assets 分区镜像"
)
add_custom_target(sparkbot_assets ALL DEPENDS "${CMAKE_BINARY_DIR}/sparkbot_assets.bin")

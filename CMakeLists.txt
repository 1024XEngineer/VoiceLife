cmake_minimum_required(VERSION 3.22)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/third_party/yyjson")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "0.1.0")
project(voicelife)

# SparkBot assets 分区镜像：构建期按官方格式生成（12B 头 + 44B/项表 + ZZ 数据）。
# 这是 SparkBot 的板级产物，PCB Profile 不生成、也不依赖它。
if(CONFIG_VOICELIFE_BOARD_ESP_SPARKBOT)
    set(SPARKBOT_GIF_DIR "${CMAKE_SOURCE_DIR}/components/voicelife_display_esp/assets/esp-sparkbot/mascot/gifs")
    set(SPARKBOT_COMMON_FONT "${CMAKE_SOURCE_DIR}/components/voicelife_display_esp/assets/esp-sparkbot/fonts/font_noto_sans_common_14_1.bin")
    set(SPARKBOT_WAKENET_MODEL_DIR
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-sr/model/wakenet_model/wn9l_nihaoxiaozhi_tts3")
    set(SPARKBOT_ASSETS_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/build_sparkbot_assets.py")
    file(GLOB SPARKBOT_GIF_FILES "${SPARKBOT_GIF_DIR}/*.gif")
    file(GLOB SPARKBOT_WAKENET_MODEL_FILES "${SPARKBOT_WAKENET_MODEL_DIR}/*")
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/sparkbot_assets.bin"
        COMMAND python3 "${SPARKBOT_ASSETS_SCRIPT}"
                --gif-dir "${SPARKBOT_GIF_DIR}"
                --common-font "${SPARKBOT_COMMON_FONT}"
                --wake-model-dir "${SPARKBOT_WAKENET_MODEL_DIR}"
                --output "${CMAKE_BINARY_DIR}/sparkbot_assets.bin"
        DEPENDS "${SPARKBOT_ASSETS_SCRIPT}" "${SPARKBOT_COMMON_FONT}" ${SPARKBOT_GIF_FILES} ${SPARKBOT_WAKENET_MODEL_FILES}
        COMMENT "生成 SparkBot assets 分区镜像"
    )
    add_custom_target(sparkbot_assets ALL DEPENDS "${CMAKE_BINARY_DIR}/sparkbot_assets.bin")
endif()

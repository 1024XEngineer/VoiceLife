cmake_minimum_required(VERSION 3.22)

list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/voicelife_contracts"
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/voicelife_timing"
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/voicelife_timing_esp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/voicelife_timing_esp/test"
    "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/yyjson"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(voicelife_timing_esp_unity)

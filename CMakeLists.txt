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

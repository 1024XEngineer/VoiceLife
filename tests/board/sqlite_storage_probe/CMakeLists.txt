cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/sqlite3")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voicelife_sqlite_storage_probe)

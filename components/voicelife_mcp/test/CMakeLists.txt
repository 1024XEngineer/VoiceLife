cmake_minimum_required(VERSION 3.20)
project(voicelife_mcp_host_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Werror -Wpedantic -Wmissing-field-initializers
                        -Wno-error=missing-field-initializers)
endif()

get_filename_component(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_library(contracts STATIC "${ROOT_DIR}/components/voicelife_contracts/src/status.cc")
target_include_directories(contracts PUBLIC "${ROOT_DIR}/components/voicelife_contracts/include")

add_library(yyjson STATIC "${ROOT_DIR}/third_party/yyjson/yyjson.c")
target_include_directories(yyjson PUBLIC "${ROOT_DIR}/third_party/yyjson")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(yyjson PRIVATE -w)
endif()

add_library(mcp STATIC "${ROOT_DIR}/components/voicelife_mcp/src/mcp_server.cc"
                       "${ROOT_DIR}/components/voicelife_mcp/src/mcp_json_writer.cc")
target_include_directories(mcp PUBLIC "${ROOT_DIR}/components/voicelife_mcp/include")
target_link_libraries(mcp PUBLIC contracts)
target_link_libraries(mcp PRIVATE yyjson)

add_executable(mcp_server_test mcp_server_test.cc)
target_include_directories(mcp_server_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}"
                                                   "${CMAKE_CURRENT_LIST_DIR}/../../../tests/host")
target_link_libraries(mcp_server_test PRIVATE mcp yyjson)

enable_testing()
add_test(NAME mcp_server_test COMMAND mcp_server_test)

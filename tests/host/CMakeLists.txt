cmake_minimum_required(VERSION 3.20)
project(voicelife_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VOICELIFE_ENABLE_COVERAGE "Instrument host tests for gcov coverage reports" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Werror -Wpedantic -Wmissing-field-initializers)
endif()

if(VOICELIFE_ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "VOICELIFE_ENABLE_COVERAGE currently requires GCC/gcov")
    endif()
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
endif()

set(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")

function(add_voicelife_library target component)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC "${ROOT_DIR}/components/${component}/include")
endfunction()

add_voicelife_library(contracts voicelife_contracts
    "${ROOT_DIR}/components/voicelife_contracts/src/status.cc")
add_voicelife_library(schedule voicelife_schedule
    "${ROOT_DIR}/components/voicelife_schedule/src/schedule_module.cc")
target_link_libraries(schedule PUBLIC contracts)
add_voicelife_library(timing voicelife_timing
    "${ROOT_DIR}/components/voicelife_timing/src/timing_policy.cc")
target_link_libraries(timing PUBLIC contracts)
add_voicelife_library(mcp voicelife_mcp
    "${ROOT_DIR}/components/voicelife_mcp/src/mcp_tool_gateway.cc")
target_link_libraries(mcp PUBLIC contracts)
add_voicelife_library(voice voicelife_voice
    "${ROOT_DIR}/components/voicelife_voice/src/voice_session_coordinator.cc")
target_link_libraries(voice PUBLIC contracts)

enable_testing()

function(add_voicelife_test target labels)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "${labels}")
endfunction()

add_voicelife_test(schedule_contract_test "unit;schedule" schedule_contract_test.cc)
target_link_libraries(schedule_contract_test PRIVATE schedule)

add_voicelife_test(status_test "unit;contracts" status_test.cc)
target_link_libraries(status_test PRIVATE contracts)

add_voicelife_test(timing_policy_test "unit;timing" timing_policy_test.cc)
target_link_libraries(timing_policy_test PRIVATE timing)

add_voicelife_test(mcp_tool_gateway_test "unit;mcp" mcp_tool_gateway_test.cc)
target_link_libraries(mcp_tool_gateway_test PRIVATE mcp)

add_voicelife_test(voice_session_coordinator_test "unit;voice" voice_session_coordinator_test.cc)
target_link_libraries(voice_session_coordinator_test PRIVATE voice)

add_voicelife_test(im_gateway_contract_test "unit;im" im_gateway_contract_test.cc)
target_compile_definitions(im_gateway_contract_test PRIVATE VOICELIFE_SOURCE_DIR="${ROOT_DIR}")

add_voicelife_test(runtime_smoke_test "integration;runtime" runtime_smoke_test.cc)
target_link_libraries(runtime_smoke_test PRIVATE mcp voice)

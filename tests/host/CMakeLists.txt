cmake_minimum_required(VERSION 3.20)
project(voicelife_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Werror -Wpedantic -Wmissing-field-initializers)
endif()

set(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")

function(add_voicelife_library target component)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC "${ROOT_DIR}/components/${component}/include")
endfunction()

add_voicelife_library(contracts voicelife_contracts
    "${ROOT_DIR}/components/voicelife_contracts/src/status.cc")
add_voicelife_library(schedule voicelife_schedule
    "${ROOT_DIR}/components/voicelife_schedule/src/schedule_policy.cc")
target_link_libraries(schedule PUBLIC contracts)
add_voicelife_library(timing voicelife_timing
    "${ROOT_DIR}/components/voicelife_timing/src/timing_policy.cc")
target_link_libraries(timing PUBLIC contracts)
add_voicelife_library(application voicelife_application
    "${ROOT_DIR}/components/voicelife_application/src/calendar_application.cc")
target_link_libraries(application PUBLIC contracts schedule timing)
add_voicelife_library(mcp voicelife_mcp
    "${ROOT_DIR}/components/voicelife_mcp/src/mcp_tool_gateway.cc")
target_link_libraries(mcp PUBLIC contracts application)
add_voicelife_library(voice voicelife_voice
    "${ROOT_DIR}/components/voicelife_voice/src/voice_session_coordinator.cc")
target_link_libraries(voice PUBLIC contracts)
add_voicelife_library(im voicelife_im
    "${ROOT_DIR}/components/voicelife_im/src/im_gateway_adapter.cc")
target_link_libraries(im PUBLIC contracts application)
add_voicelife_library(platform voicelife_platform
    "${ROOT_DIR}/components/voicelife_platform/src/in_memory_calendar_store.cc"
    "${ROOT_DIR}/components/voicelife_platform/src/sequential_id_generator.cc"
    "${ROOT_DIR}/components/voicelife_platform/src/system_clock.cc")
target_link_libraries(platform PUBLIC contracts application)

enable_testing()

function(add_voicelife_test target labels)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "${labels}")
endfunction()

add_voicelife_test(schedule_policy_test "unit;schedule" schedule_policy_test.cc)
target_link_libraries(schedule_policy_test PRIVATE schedule)

add_voicelife_test(timing_policy_test "unit;timing" timing_policy_test.cc)
target_link_libraries(timing_policy_test PRIVATE timing)

add_voicelife_test(calendar_application_test "unit;application" calendar_application_test.cc)
target_link_libraries(calendar_application_test PRIVATE application)

add_voicelife_test(mcp_tool_gateway_test "unit;mcp" mcp_tool_gateway_test.cc)
target_link_libraries(mcp_tool_gateway_test PRIVATE mcp)

add_voicelife_test(voice_session_coordinator_test "unit;voice" voice_session_coordinator_test.cc)
target_link_libraries(voice_session_coordinator_test PRIVATE voice)

add_voicelife_test(im_gateway_adapter_test "unit;im" im_gateway_adapter_test.cc)
target_link_libraries(im_gateway_adapter_test PRIVATE im)

add_voicelife_test(runtime_smoke_test "integration;runtime" runtime_smoke_test.cc)
target_link_libraries(runtime_smoke_test PRIVATE mcp voice im platform)

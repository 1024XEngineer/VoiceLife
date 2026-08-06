cmake_minimum_required(VERSION 3.20)
project(voicelife_host_tests LANGUAGES C CXX)

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

add_library(yyjson STATIC "${ROOT_DIR}/third_party/yyjson/yyjson.c")
target_include_directories(yyjson PUBLIC "${ROOT_DIR}/third_party/yyjson")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(yyjson PRIVATE -w)
endif()

function(add_voicelife_library target component)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC "${ROOT_DIR}/components/${component}/include")
endfunction()

add_voicelife_library(contracts voicelife_contracts
    "${ROOT_DIR}/components/voicelife_contracts/src/status.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/json.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/notification_intent.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/schedule_receipt.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/reminder_action_result.cc")
target_link_libraries(contracts PRIVATE yyjson)
add_voicelife_library(im voicelife_im
    "${ROOT_DIR}/components/voicelife_im/src/im_reporting_channel.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_wire.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_endpoint.cc")
target_link_libraries(im PUBLIC contracts)
add_voicelife_library(schedule voicelife_schedule
    "${ROOT_DIR}/components/voicelife_schedule/src/schedule_create_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/schedule_mock_data.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/schedule_query_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/schedule_service.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/schedule_time_rules.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/schedule_update_helpers.cc")
target_include_directories(schedule PRIVATE "${ROOT_DIR}/components/voicelife_schedule/src")
target_link_libraries(schedule PUBLIC contracts)
add_voicelife_library(timing voicelife_timing
    "${ROOT_DIR}/components/voicelife_timing/src/timing_policy.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_service.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_service_stubs.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_state.cc")
target_link_libraries(timing PUBLIC contracts)
add_voicelife_library(mcp voicelife_mcp
    "${ROOT_DIR}/components/voicelife_mcp/src/mcp_tool_gateway.cc")
target_link_libraries(mcp PUBLIC contracts)
add_voicelife_library(voice voicelife_voice
    "${ROOT_DIR}/components/voicelife_voice/src/voice_provider_registry.cc"
    "${ROOT_DIR}/components/voicelife_voice/src/voice_session.cc"
    "${ROOT_DIR}/components/voicelife_voice/src/voice_session_coordinator.cc")
target_link_libraries(voice PUBLIC contracts)
add_voicelife_library(linx voicelife_linx
    "${ROOT_DIR}/components/voicelife_linx/src/linx_json_codec.cc"
    "${ROOT_DIR}/components/voicelife_linx/src/linx_speech_provider.cc")
target_link_libraries(linx PUBLIC voice)
add_voicelife_library(storage_sqlite voicelife_storage_sqlite
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/storage_protocol.cc")
target_link_libraries(storage_sqlite PUBLIC contracts)

enable_testing()

function(add_voicelife_test target labels)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "${labels}")
endfunction()

add_voicelife_test(schedule_contract_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_contract_test.cc")
target_link_libraries(schedule_contract_test PRIVATE schedule)

add_voicelife_test(schedule_create_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_create_test.cc")
target_link_libraries(schedule_create_test PRIVATE schedule)

add_voicelife_test(schedule_query_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_query_test.cc")
target_link_libraries(schedule_query_test PRIVATE schedule)

add_voicelife_test(schedule_delete_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_delete_test.cc")
target_link_libraries(schedule_delete_test PRIVATE schedule)

add_voicelife_test(schedule_update_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_update_test.cc")
target_link_libraries(schedule_update_test PRIVATE schedule)

add_voicelife_test(status_test "unit;contracts" status_test.cc)
target_link_libraries(status_test PRIVATE contracts)

add_voicelife_test(timing_policy_test "unit;timing" timing_policy_test.cc)
target_link_libraries(timing_policy_test PRIVATE timing)

add_voicelife_test(timing_task_types_test "unit;timing" timing_task_types_test.cc)
target_include_directories(timing_task_types_test PRIVATE "${ROOT_DIR}/components/voicelife_timing/include")

add_voicelife_test(timing_task_state_test "unit;timing" timing_task_state_test.cc)
target_link_libraries(timing_task_state_test PRIVATE timing)

add_voicelife_test(timing_task_service_test "unit;timing" timing_task_service_test.cc)
target_link_libraries(timing_task_service_test PRIVATE timing)

add_voicelife_test(timing_task_store_test "unit;timing" timing_task_store_test.cc)
target_link_libraries(timing_task_store_test PRIVATE timing)

add_voicelife_test(timing_task_service_contract_test "unit;timing" timing_task_service_contract_test.cc)
target_link_libraries(timing_task_service_contract_test PRIVATE timing)

add_voicelife_test(mcp_tool_gateway_test "unit;mcp" mcp_tool_gateway_test.cc)
target_link_libraries(mcp_tool_gateway_test PRIVATE mcp)

add_voicelife_test(voice_session_coordinator_test "unit;voice" voice_session_coordinator_test.cc)
target_link_libraries(voice_session_coordinator_test PRIVATE voice)

add_voicelife_test(voice_session_contract_test "unit;voice;contract" voice_session_contract_test.cc)
target_link_libraries(voice_session_contract_test PRIVATE voice)

add_voicelife_test(linx_provider_contract_test "unit;voice;linx;contract" linx_provider_contract_test.cc)
target_link_libraries(linx_provider_contract_test PRIVATE linx)

add_voicelife_test(im_gateway_contract_test "unit;im" im_gateway_contract_test.cc)
target_compile_definitions(im_gateway_contract_test PRIVATE VOICELIFE_SOURCE_DIR="${ROOT_DIR}")

add_voicelife_test(im_contract_parser_test "unit;im" im_contract_parser_test.cc)
target_link_libraries(im_contract_parser_test PRIVATE contracts)
target_compile_definitions(im_contract_parser_test PRIVATE VOICELIFE_SOURCE_DIR="${ROOT_DIR}")

add_voicelife_test(im_json_parser_test "unit;im" im_json_parser_test.cc)
target_link_libraries(im_json_parser_test PRIVATE contracts)

add_voicelife_test(im_contract_validation_test "unit;im" im_contract_validation_test.cc)
target_link_libraries(im_contract_validation_test PRIVATE contracts)

add_voicelife_test(im_reporting_channel_test "unit;im" im_reporting_channel_test.cc)
target_link_libraries(im_reporting_channel_test PRIVATE im contracts)
target_compile_definitions(im_reporting_channel_test PRIVATE VOICELIFE_SOURCE_DIR="${ROOT_DIR}")

add_voicelife_test(runtime_smoke_test "integration;runtime" runtime_smoke_test.cc)
target_link_libraries(runtime_smoke_test PRIVATE mcp voice)

add_voicelife_test(storage_protocol_contract_test "unit;storage;contract"
    storage_protocol_contract_test.cc)
target_link_libraries(storage_protocol_contract_test PRIVATE storage_sqlite)

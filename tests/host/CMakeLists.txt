cmake_minimum_required(VERSION 3.20)
project(voicelife_host_tests LANGUAGES C CXX)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VOICELIFE_ENABLE_COVERAGE "Instrument host tests for gcov coverage reports" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Werror -Wpedantic -Wmissing-field-initializers
                        -Wno-error=missing-field-initializers)
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
# Linx 主机测试使用仓库内固定版本的 cJSON。
add_library(cjson STATIC "${ROOT_DIR}/third_party/cjson/cJSON.c")
target_include_directories(cjson PUBLIC "${ROOT_DIR}/third_party/cjson")
set_target_properties(cjson PROPERTIES LINKER_LANGUAGE C)

function(add_voicelife_library target component)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC "${ROOT_DIR}/components/${component}/include")
endfunction()

add_voicelife_library(contracts voicelife_contracts
    "${ROOT_DIR}/components/voicelife_contracts/src/status.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/json.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/notification_intent.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/schedule_receipt.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/schedule_query_result.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/reminder_action_result.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/reminder_action_status_report.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/reminder_action_command.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/notification_submission.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/pairing_session.cc")
target_link_libraries(contracts PRIVATE yyjson)
add_voicelife_library(application voicelife_application
    "${ROOT_DIR}/components/voicelife_application/src/interaction_orchestrator.cc")
add_voicelife_library(im voicelife_im
    "${ROOT_DIR}/components/voicelife_im/src/im_reporting_channel.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_action_channel.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_sse.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_runtime.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_config_store.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_retry_policy.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_provisioning.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_pairing_client.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_pairing_controller.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_binding_use_case.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_wire.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_endpoint.cc"
    "${ROOT_DIR}/components/voicelife_im/src/transport/im_response_reader.cc")
target_include_directories(im PUBLIC "${ROOT_DIR}/components/voicelife_im/src/transport")
target_link_libraries(im PUBLIC contracts)
add_voicelife_library(timing voicelife_timing
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_runtime.cc")
target_link_libraries(timing PUBLIC contracts)
add_voicelife_library(schedule voicelife_schedule
    "${ROOT_DIR}/components/voicelife_schedule/src/calendar.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/factory/schedule_factory.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_create_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_occurrence_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_operation_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_query_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_rule_result_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_rule_update_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_snapshot_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/service/schedule_operation_service.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/service/schedule_rule_service_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/service/schedule_rule_service.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/service/schedule_reminder_service.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/service/schedule_service.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/rules/recurrence_planner.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/rules/schedule_time_rules.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_update_helpers.cc")
target_include_directories(schedule PRIVATE "${ROOT_DIR}/components/voicelife_schedule/src")
target_link_libraries(schedule PUBLIC contracts timing Threads::Threads)
add_voicelife_library(timing_esp voicelife_timing_esp
    "${ROOT_DIR}/components/voicelife_timing_esp/src/esp_timing_runtime.cc")
target_link_libraries(timing_esp PUBLIC timing contracts)
add_voicelife_library(mcp voicelife_mcp
    "${ROOT_DIR}/components/voicelife_mcp/src/mcp_server.cc"
    "${ROOT_DIR}/components/voicelife_mcp/src/mcp_json_writer.cc"
    "${ROOT_DIR}/components/voicelife_mcp/src/tools/schedule_mcp_tools_input.cc")
target_include_directories(mcp PRIVATE "${ROOT_DIR}/components/voicelife_mcp/src/tools")
target_link_libraries(mcp PUBLIC contracts schedule)
target_link_libraries(mcp PRIVATE yyjson)
add_voicelife_library(voice voicelife_voice
    "${ROOT_DIR}/components/voicelife_voice/src/audio_frame_queue.cc"
    "${ROOT_DIR}/components/voicelife_voice/src/audio_payload.cc"
    "${ROOT_DIR}/components/voicelife_voice/src/audio_aec.cc"
    "${ROOT_DIR}/components/voicelife_voice/src/wake_gate_audio_input.cc"
    "${ROOT_DIR}/components/voicelife_voice/src/voice_interaction_controller.cc"
    "${ROOT_DIR}/components/voicelife_voice/src/voice_provider_registry.cc"
    "${ROOT_DIR}/components/voicelife_voice/src/voice_session.cc"
    "${ROOT_DIR}/components/voicelife_voice/src/voice_session_coordinator.cc")
target_link_libraries(voice PUBLIC contracts)
add_voicelife_library(linx voicelife_linx
    "${ROOT_DIR}/components/voicelife_linx/src/linx_json_codec.cc"
    "${ROOT_DIR}/components/voicelife_linx/src/linx_ota.cc"
    "${ROOT_DIR}/components/voicelife_linx/src/linx_speech_provider.cc")
target_link_libraries(linx PUBLIC voice cjson Threads::Threads)
add_voicelife_library(linx_esp voicelife_linx_esp
    "${ROOT_DIR}/components/voicelife_linx_esp/src/websocket_fragment_assembler.cc")
target_link_libraries(linx_esp PUBLIC contracts linx)
add_voicelife_library(storage_sqlite voicelife_storage_sqlite
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/mapping/operation_row_mapper.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/mapping/schedule_exception_row_mapper.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/mapping/schedule_rule_row_mapper.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/mapping/schedule_row_mapper.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/mapping/schedule_reminder_task_row_mapper.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/migrations/v001_create_schedule.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/migrations/v002_create_schedule_operation.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/migrations/v003_create_schedule_rule.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/migrations/v004_create_operation_record.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/migrations/v005_add_schedule_reminder_task_id.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/migrations/v006_create_schedule_reminder_task.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/migrations/v007_add_schedule_reminder_task_event.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/migrations/v008_add_reminder_action_state.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/sqlite_schema.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/schema/voicelife_schema.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/sql/operation_sql.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/sql/schedule_exception_sql.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/sql/schedule_rule_sql.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/sql/schedule_sql.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/sql/schedule_reminder_task_sql.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/sqlite_database.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/sqlite_schedule_repository.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/sqlite_schedule_reminder_task_repository.cc"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/sqlite_schedule_rule_repository.cc")
target_include_directories(storage_sqlite PRIVATE "${ROOT_DIR}/components/voicelife_storage_sqlite/src")
target_link_libraries(storage_sqlite PUBLIC contracts schedule)
if(TARGET SQLite3::SQLite3)
    target_link_libraries(storage_sqlite PRIVATE SQLite3::SQLite3)
else()
    target_link_libraries(storage_sqlite PRIVATE SQLite::SQLite3)
endif()
add_voicelife_library(storage_fatfs voicelife_storage_fatfs
    "${ROOT_DIR}/components/voicelife_storage_fatfs/src/fatfs_volume.cc")
target_link_libraries(storage_fatfs PUBLIC contracts)
add_voicelife_library(storage_memory voicelife_storage_memory
    "${ROOT_DIR}/components/voicelife_storage_memory/src/memory_schedule_repository.cc"
    "${ROOT_DIR}/components/voicelife_storage_memory/src/memory_schedule_reminder_task_repository.cc")
target_link_libraries(storage_memory PUBLIC contracts schedule Threads::Threads)
add_voicelife_library(audio_esp voicelife_audio_esp
    "${ROOT_DIR}/components/voicelife_audio_esp/src/audio_board_profile.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp32s3_audio_probe.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/pcm_frame_assembler.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp32s3_pcm_audio_port.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp32s3_pcm_i2s_runtime.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp_aec_service.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp_multinet_wake_detector.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp_wakenet_detector.cc")
target_link_libraries(audio_esp PUBLIC contracts voice)
add_voicelife_library(board_esp voicelife_board_esp
    "${ROOT_DIR}/components/voicelife_board_esp/src/sparkbot_profile.cc"
    "${ROOT_DIR}/components/voicelife_board_esp/src/gpio46_power_arbiter.cc"
    "${ROOT_DIR}/components/voicelife_board_esp/src/esp32s3_board_probe.cc"
    )
target_link_libraries(board_esp PUBLIC contracts)
add_voicelife_library(display_esp voicelife_display_esp
    "${ROOT_DIR}/components/voicelife_display_esp/src/ssd1306_presentation_adapter.cc"
    "${ROOT_DIR}/components/voicelife_display_esp/src/ssd1306_status_display.cc"
    "${ROOT_DIR}/components/voicelife_display_esp/src/ssd1306_glyphs.cc"
    "${ROOT_DIR}/components/voicelife_display_esp/src/ssd1306_glyph_assets.cc")
target_link_libraries(display_esp PUBLIC contracts voice)
add_voicelife_library(display_sparkbot voicelife_display_sparkbot
    "${ROOT_DIR}/components/voicelife_display_sparkbot/src/sparkbot_lvgl_display.cc"
    "${ROOT_DIR}/components/voicelife_display_sparkbot/src/sparkbot_lvgl_renderer.cc"
    "${ROOT_DIR}/components/voicelife_display_sparkbot/src/sparkbot_emoji_assets.cc"
    "${ROOT_DIR}/components/voicelife_display_sparkbot/src/display_snapshot_queue.cc"
    "${ROOT_DIR}/components/voicelife_display_sparkbot/src/sparkbot_presentation_adapter.cc")
target_link_libraries(display_sparkbot PUBLIC contracts voice)

enable_testing()

function(add_voicelife_test target labels)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "${labels}")
endfunction()

add_voicelife_test(ssd1306_glyphs_test "unit;display"
    "${CMAKE_CURRENT_LIST_DIR}/ssd1306_glyphs_test.cc")
target_include_directories(ssd1306_glyphs_test PRIVATE "${ROOT_DIR}/components/voicelife_display_esp/src")
target_link_libraries(ssd1306_glyphs_test PRIVATE display_esp)

add_voicelife_test(schedule_contract_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_contract_test.cc")
target_link_libraries(schedule_contract_test PRIVATE schedule)

add_voicelife_test(schedule_create_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_create_test.cc")
target_link_libraries(schedule_create_test PRIVATE schedule)

add_voicelife_test(schedule_query_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_query_test.cc")
target_link_libraries(schedule_query_test PRIVATE schedule)

add_voicelife_test(schedule_recurrence_planner_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_recurrence_planner_test.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/rules/recurrence_planner.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/calendar.cc")
target_include_directories(schedule_recurrence_planner_test PRIVATE
    "${ROOT_DIR}/components/voicelife_schedule/src"
    "${ROOT_DIR}/components/voicelife_schedule/include")

add_voicelife_test(schedule_repository_service_test "unit;schedule;repository"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_repository_service_test.cc")
target_link_libraries(schedule_repository_service_test PRIVATE schedule)

add_voicelife_test(schedule_delete_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_delete_test.cc")
target_link_libraries(schedule_delete_test PRIVATE schedule)

add_voicelife_test(schedule_update_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_update_test.cc")
target_link_libraries(schedule_update_test PRIVATE schedule)

add_voicelife_test(schedule_operation_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_operation_test.cc")
target_link_libraries(schedule_operation_test PRIVATE schedule)

add_voicelife_test(schedule_recent_operation_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_recent_operation_test.cc")
target_link_libraries(schedule_recent_operation_test PRIVATE schedule)

add_voicelife_test(status_test "unit;contracts" status_test.cc)
target_link_libraries(status_test PRIVATE contracts)

add_voicelife_test(timing_contract_test "unit;timing;contract" timing_contract_test.cc)
target_link_libraries(timing_contract_test PRIVATE timing)

add_voicelife_test(timing_registration_test "unit;timing" timing_registration_test.cc)
target_link_libraries(timing_registration_test PRIVATE timing)

add_voicelife_test(timing_cancellation_test "unit;timing" timing_cancellation_test.cc)
target_link_libraries(timing_cancellation_test PRIVATE timing)

add_voicelife_test(timing_due_tasks_test "unit;timing" timing_due_tasks_test.cc)
target_link_libraries(timing_due_tasks_test PRIVATE timing)

add_voicelife_test(timing_runtime_test "unit;timing" timing_runtime_test.cc)
target_link_libraries(timing_runtime_test PRIVATE timing)

add_voicelife_test(timing_esp_runtime_contract_test "unit;timing;esp;contract"
    timing_esp_runtime_contract_test.cc)
target_link_libraries(timing_esp_runtime_contract_test PRIVATE timing_esp)

add_voicelife_test(fatfs_volume_contract_test "unit;storage;fatfs"
    "${ROOT_DIR}/components/voicelife_storage_fatfs/test/fatfs_volume_contract_test.cc")
target_link_libraries(fatfs_volume_contract_test PRIVATE storage_fatfs)

add_voicelife_test(memory_schedule_repository_test "unit;storage;memory"
    "${ROOT_DIR}/components/voicelife_storage_memory/test/memory_schedule_repository_test.cc")
target_link_libraries(memory_schedule_repository_test PRIVATE storage_memory schedule)

add_voicelife_test(memory_schedule_reminder_task_repository_test "unit;storage;memory;reminder"
    "${ROOT_DIR}/components/voicelife_storage_memory/test/memory_schedule_reminder_task_repository_test.cc")
target_link_libraries(memory_schedule_reminder_task_repository_test PRIVATE storage_memory schedule)

add_voicelife_test(mcp_server_test "unit;mcp"
    "${ROOT_DIR}/components/voicelife_mcp/test/mcp_server_test.cc")
target_include_directories(mcp_server_test PRIVATE "${ROOT_DIR}/third_party/yyjson")
target_link_libraries(mcp_server_test PRIVATE mcp)

set(SCHEDULE_MCP_TOOLS_SOURCE "${ROOT_DIR}/components/voicelife_mcp/src/tools/schedule_mcp_tools.cc")
add_library(schedule_mcp_tools_coverage_impl OBJECT "${SCHEDULE_MCP_TOOLS_SOURCE}")
target_include_directories(schedule_mcp_tools_coverage_impl PRIVATE "${ROOT_DIR}/components/voicelife_mcp/src/tools")
target_link_libraries(schedule_mcp_tools_coverage_impl PRIVATE mcp schedule im)

add_library(linx_mcp_bridge_coverage_impl OBJECT
    "${ROOT_DIR}/components/voicelife_runtime/src/linx_mcp_bridge.cc")
target_link_libraries(linx_mcp_bridge_coverage_impl PRIVATE contracts mcp)

add_voicelife_test(schedule_mcp_tools_test "unit;mcp;schedule;runtime" schedule_mcp_tools_test.cc
    $<TARGET_OBJECTS:schedule_mcp_tools_coverage_impl>)
target_link_libraries(schedule_mcp_tools_test PRIVATE mcp schedule im)

add_voicelife_test(schedule_mcp_tools_delete_confirmation_test "unit;mcp;schedule;runtime"
    schedule_mcp_tools_delete_confirmation_test.cc
    $<TARGET_OBJECTS:schedule_mcp_tools_coverage_impl>)
target_link_libraries(schedule_mcp_tools_delete_confirmation_test PRIVATE mcp schedule im)

add_voicelife_test(schedule_mcp_tools_reminder_test "unit;mcp;schedule;reminder"
    schedule_mcp_tools_reminder_test.cc
    $<TARGET_OBJECTS:schedule_mcp_tools_coverage_impl>)
target_include_directories(schedule_mcp_tools_reminder_test PRIVATE
    "${ROOT_DIR}/components/voicelife_storage_memory/include")
target_link_libraries(schedule_mcp_tools_reminder_test PRIVATE mcp schedule timing im storage_memory)

add_voicelife_test(schedule_mcp_tools_branch_test "unit;mcp;schedule;reminder"
    schedule_mcp_tools_branch_test.cc
    $<TARGET_OBJECTS:schedule_mcp_tools_coverage_impl>)
target_include_directories(schedule_mcp_tools_branch_test PRIVATE
    "${ROOT_DIR}/components/voicelife_storage_memory/include")
target_link_libraries(schedule_mcp_tools_branch_test PRIVATE mcp schedule timing im storage_memory)

add_voicelife_test(schedule_mcp_tools_failure_coverage_test "unit;mcp;schedule;runtime"
    schedule_mcp_tools_failure_coverage_test.cc
    $<TARGET_OBJECTS:schedule_mcp_tools_coverage_impl>)
target_link_libraries(schedule_mcp_tools_failure_coverage_test PRIVATE mcp schedule im)

add_voicelife_test(schedule_mcp_tools_recovery_coverage_test "unit;mcp;schedule"
    "${CMAKE_CURRENT_LIST_DIR}/schedule_mcp_tools_recovery_coverage_test.cc"
    $<TARGET_OBJECTS:schedule_mcp_tools_coverage_impl>)
target_link_libraries(schedule_mcp_tools_recovery_coverage_test PRIVATE mcp schedule im)

add_voicelife_test(schedule_mcp_tools_operation_coverage_test "unit;mcp;schedule"
    schedule_mcp_tools_operation_coverage_test.cc
    $<TARGET_OBJECTS:schedule_mcp_tools_coverage_impl>)
target_link_libraries(schedule_mcp_tools_operation_coverage_test PRIVATE mcp schedule im)

add_voicelife_test(schedule_rule_mcp_tools_test "unit;mcp;schedule;runtime" schedule_rule_mcp_tools_test.cc
    "${ROOT_DIR}/components/voicelife_mcp/src/tools/schedule_rule_mcp_tools.cc")
target_include_directories(schedule_rule_mcp_tools_test PRIVATE "${ROOT_DIR}/components/voicelife_mcp/src/tools")
target_link_libraries(schedule_rule_mcp_tools_test PRIVATE mcp schedule)

add_voicelife_test(schedule_reminder_service_test "unit;schedule;timing;reminder"
    schedule_reminder_service_test.cc)
target_link_libraries(schedule_reminder_service_test PRIVATE schedule timing storage_memory)

add_voicelife_test(schedule_reminder_uncovered_branch_test "unit;schedule;timing;reminder"
    schedule_reminder_uncovered_branch_test.cc)
target_link_libraries(schedule_reminder_uncovered_branch_test PRIVATE schedule timing storage_memory)

add_voicelife_test(schedule_reminder_edge_cases_test "unit;schedule;timing;reminder"
    schedule_reminder_edge_cases_test.cc)
target_link_libraries(schedule_reminder_edge_cases_test PRIVATE schedule timing storage_memory)

add_voicelife_test(schedule_reminder_recovery_action_test "unit;schedule;timing;reminder"
    schedule_reminder_recovery_action_test.cc)
target_link_libraries(schedule_reminder_recovery_action_test PRIVATE schedule timing storage_memory)

add_voicelife_test(schedule_reminder_action_execution_test "unit;schedule;timing;reminder"
    schedule_reminder_action_execution_test.cc)
target_link_libraries(schedule_reminder_action_execution_test PRIVATE schedule timing storage_memory)

add_voicelife_test(schedule_reminder_terminal_sync_test "unit;schedule;timing;reminder"
    schedule_reminder_terminal_sync_test.cc)
target_link_libraries(schedule_reminder_terminal_sync_test PRIVATE schedule timing storage_memory)

add_voicelife_test(schedule_rule_service_test "unit;schedule" schedule_rule_service_test.cc)
target_link_libraries(schedule_rule_service_test PRIVATE schedule)

add_voicelife_test(schedule_rule_service_helpers_test "unit;schedule" schedule_rule_service_helpers_test.cc)
target_include_directories(schedule_rule_service_helpers_test PRIVATE "${ROOT_DIR}/components/voicelife_schedule/src/service")
target_link_libraries(schedule_rule_service_helpers_test PRIVATE schedule)

add_voicelife_test(schedule_helpers_test "unit;schedule" schedule_helpers_test.cc)
target_include_directories(schedule_helpers_test PRIVATE "${ROOT_DIR}/components/voicelife_schedule/src")
target_link_libraries(schedule_helpers_test PRIVATE schedule)

add_voicelife_test(schedule_helpers_coverage_test "unit;schedule" schedule_helpers_coverage_test.cc)
target_include_directories(schedule_helpers_coverage_test PRIVATE "${ROOT_DIR}/components/voicelife_schedule/src")
target_link_libraries(schedule_helpers_coverage_test PRIVATE schedule)

add_voicelife_test(mcp_json_writer_coverage_test "unit;mcp" mcp_json_writer_coverage_test.cc)
target_include_directories(mcp_json_writer_coverage_test PRIVATE "${ROOT_DIR}/third_party/yyjson")
target_include_directories(mcp_json_writer_coverage_test PRIVATE "${ROOT_DIR}/components/voicelife_mcp/src")
target_link_libraries(mcp_json_writer_coverage_test PRIVATE mcp)

add_voicelife_test(mcp_server_coverage_test "unit;mcp" mcp_server_coverage_test.cc)
target_include_directories(mcp_server_coverage_test PRIVATE "${ROOT_DIR}/third_party/yyjson")
target_link_libraries(mcp_server_coverage_test PRIVATE mcp)

add_voicelife_test(schedule_mcp_tools_input_test "unit;mcp;schedule;runtime" schedule_mcp_tools_input_test.cc)
target_include_directories(schedule_mcp_tools_input_test PRIVATE "${ROOT_DIR}/components/voicelife_mcp/src/tools")
target_link_libraries(schedule_mcp_tools_input_test PRIVATE mcp schedule)

add_voicelife_test(linx_mcp_bridge_test "unit;mcp;linx;runtime" linx_mcp_bridge_test.cc
    $<TARGET_OBJECTS:linx_mcp_bridge_coverage_impl>
    $<TARGET_OBJECTS:schedule_mcp_tools_coverage_impl>)
target_include_directories(linx_mcp_bridge_test PRIVATE "${ROOT_DIR}/components/voicelife_runtime/src")
target_link_libraries(linx_mcp_bridge_test PRIVATE contracts mcp schedule im)

add_voicelife_test(linx_mcp_bridge_coverage_test "unit;mcp;linx;runtime" linx_mcp_bridge_coverage_test.cc
    $<TARGET_OBJECTS:linx_mcp_bridge_coverage_impl>
    $<TARGET_OBJECTS:schedule_mcp_tools_coverage_impl>)
target_include_directories(linx_mcp_bridge_coverage_test PRIVATE
    "${ROOT_DIR}/components/voicelife_runtime/src"
    "${ROOT_DIR}/components/voicelife_mcp/src"
    "${ROOT_DIR}/third_party/yyjson")
target_link_libraries(linx_mcp_bridge_coverage_test PRIVATE contracts mcp schedule im)

add_voicelife_test(linx_ota_contract_test "unit;linx;contract" linx_ota_contract_test.cc)
target_link_libraries(linx_ota_contract_test PRIVATE contracts linx)

add_voicelife_test(voice_session_coordinator_test "unit;voice" voice_session_coordinator_test.cc)
target_link_libraries(voice_session_coordinator_test PRIVATE voice)

add_voicelife_test(voice_session_contract_test "unit;voice;contract" voice_session_contract_test.cc)
target_link_libraries(voice_session_contract_test PRIVATE voice)

add_voicelife_test(audio_frame_queue_contract_test "unit;voice;audio;contract"
    audio_frame_queue_contract_test.cc)
target_link_libraries(audio_frame_queue_contract_test PRIVATE voice)

add_voicelife_test(audio_aec_service_contract_test "unit;voice;audio;aec;contract"
    audio_aec_service_contract_test.cc)
target_link_libraries(audio_aec_service_contract_test PRIVATE audio_esp)

add_voicelife_test(wake_gate_audio_input_test "unit;voice;audio;contract"
    wake_gate_audio_input_test.cc)
target_link_libraries(wake_gate_audio_input_test PRIVATE voice)

add_voicelife_test(voice_interaction_controller_test "unit;voice;interaction;contract"
    voice_interaction_controller_test.cc)
target_link_libraries(voice_interaction_controller_test PRIVATE voice)

add_voicelife_test(interaction_orchestrator_test "unit;application;interaction;contract"
    interaction_orchestrator_test.cc
    "${ROOT_DIR}/components/voicelife_runtime_esp/src/esp_interaction_task_host.cc")
target_include_directories(interaction_orchestrator_test PRIVATE
    "${ROOT_DIR}/components/voicelife_runtime_esp/include")
target_link_libraries(interaction_orchestrator_test PRIVATE application)

add_voicelife_test(linx_provider_contract_test "unit;voice;linx;contract" linx_provider_contract_test.cc)
target_link_libraries(linx_provider_contract_test PRIVATE linx)

add_voicelife_test(linx_esp_transport_contract_test "unit;voice;linx;esp;contract"
    linx_esp_transport_contract_test.cc)
target_link_libraries(linx_esp_transport_contract_test PRIVATE linx_esp Threads::Threads)

add_voicelife_test(audio_board_profile_contract_test "unit;voice;audio;esp;contract"
    audio_board_profile_contract_test.cc)
target_link_libraries(audio_board_profile_contract_test PRIVATE audio_esp)

add_voicelife_test(sparkbot_profile_contract_test "unit;board;esp;contract"
    sparkbot_profile_contract_test.cc)
target_link_libraries(sparkbot_profile_contract_test PRIVATE board_esp)

add_voicelife_test(sparkbot_presentation_adapter_test "unit;display;sparkbot;board;contract"
    sparkbot_presentation_adapter_test.cc)
target_link_libraries(sparkbot_presentation_adapter_test PRIVATE display_sparkbot)

add_voicelife_test(display_snapshot_queue_test "unit;display;sparkbot;queue;contract"
    display_snapshot_queue_test.cc)
target_link_libraries(display_snapshot_queue_test PRIVATE display_sparkbot)

add_voicelife_test(sparkbot_asset_manifest_test "unit;display;assets;contract"
    sparkbot_asset_manifest_test.cc)
target_link_libraries(sparkbot_asset_manifest_test PRIVATE display_esp display_sparkbot yyjson)
target_compile_definitions(sparkbot_asset_manifest_test PRIVATE VOICELIFE_SOURCE_DIR="${ROOT_DIR}")

add_voicelife_test(platform_assembly_test "unit;platform;assembly;contract"
    platform_assembly_test.cc
    "${ROOT_DIR}/main/platform_assemblies.cc")
target_include_directories(platform_assembly_test PRIVATE
    "${ROOT_DIR}/main"
    "${ROOT_DIR}/components/voicelife_runtime/include")
target_link_libraries(platform_assembly_test PRIVATE display_esp display_sparkbot voice board_esp audio_esp)

add_voicelife_test(sparkbot_lvgl_renderer_test "unit;display;sparkbot;contract"
    sparkbot_lvgl_renderer_test.cc)
target_link_libraries(sparkbot_lvgl_renderer_test PRIVATE display_sparkbot)

add_voicelife_test(sparkbot_emoji_assets_test "unit;display;sparkbot;assets;contract"
    sparkbot_emoji_assets_test.cc)
target_link_libraries(sparkbot_emoji_assets_test PRIVATE display_sparkbot)

add_voicelife_test(pcm_frame_assembler_contract_test "unit;voice;audio;esp;contract"
    pcm_frame_assembler_contract_test.cc)
target_link_libraries(pcm_frame_assembler_contract_test PRIVATE audio_esp Threads::Threads)

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

add_voicelife_test(im_action_channel_test "unit;im" im_action_channel_test.cc)
target_link_libraries(im_action_channel_test PRIVATE im contracts)
target_compile_definitions(im_action_channel_test PRIVATE VOICELIFE_SOURCE_DIR="${ROOT_DIR}")

add_voicelife_test(im_action_channel_window_test "unit;im" im_action_channel_window_test.cc)
target_link_libraries(im_action_channel_window_test PRIVATE im contracts)
target_compile_definitions(im_action_channel_window_test PRIVATE VOICELIFE_SOURCE_DIR="${ROOT_DIR}")

add_voicelife_test(im_sse_test "unit;im" im_sse_test.cc)
target_link_libraries(im_sse_test PRIVATE im contracts)
target_compile_definitions(im_sse_test PRIVATE VOICELIFE_SOURCE_DIR="${ROOT_DIR}")

add_voicelife_test(im_response_reader_test "unit;im" im_response_reader_test.cc)
target_link_libraries(im_response_reader_test PRIVATE im contracts)

add_voicelife_test(im_pairing_client_test "unit;im;pairing" im_pairing_client_test.cc)
target_link_libraries(im_pairing_client_test PRIVATE im contracts)
target_compile_definitions(im_pairing_client_test PRIVATE VOICELIFE_SOURCE_DIR="${ROOT_DIR}")

add_voicelife_test(binding_use_case_test "unit;im;binding" binding_use_case_test.cc)
target_link_libraries(binding_use_case_test PRIVATE im contracts Threads::Threads)

add_voicelife_test(im_binding_mcp_tools_test "unit;mcp;im;runtime" im_binding_mcp_tools_test.cc
    "${ROOT_DIR}/components/voicelife_runtime/src/im_binding_mcp_tools.cc")
target_include_directories(im_binding_mcp_tools_test PRIVATE "${ROOT_DIR}/components/voicelife_runtime/src")
target_link_libraries(im_binding_mcp_tools_test PRIVATE mcp im)

add_voicelife_test(binding_presentation_test "unit;im;runtime;binding" binding_presentation_test.cc
    "${ROOT_DIR}/components/voicelife_runtime/src/im_binding_presentation.cc")
target_include_directories(binding_presentation_test PRIVATE "${ROOT_DIR}/components/voicelife_runtime/src")
target_link_libraries(binding_presentation_test PRIVATE im)

add_voicelife_test(binding_polling_lease_test "unit;im;runtime;binding" binding_polling_lease_test.cc)
target_include_directories(binding_polling_lease_test PRIVATE "${ROOT_DIR}/components/voicelife_runtime/src")

add_voicelife_test(mcp_worker_policy_test "unit;mcp;im;runtime" mcp_worker_policy_test.cc)
target_include_directories(mcp_worker_policy_test PRIVATE "${ROOT_DIR}/components/voicelife_runtime/src")
target_link_libraries(mcp_worker_policy_test PRIVATE im)

add_voicelife_test(im_runtime_test "unit;im;runtime" im_runtime_test.cc)
target_link_libraries(im_runtime_test PRIVATE im contracts)

add_voicelife_test(schedule_reminder_im_adapter_test "unit;im;schedule;reminder;runtime"
    schedule_reminder_im_adapter_test.cc
    "${ROOT_DIR}/components/voicelife_runtime/src/schedule_reminder_im_adapter.cc")
target_include_directories(schedule_reminder_im_adapter_test PRIVATE
    "${ROOT_DIR}/components/voicelife_runtime/src")
target_link_libraries(schedule_reminder_im_adapter_test PRIVATE im schedule timing storage_memory contracts)

add_voicelife_test(schedule_reminder_im_adapter_failure_test "unit;im;schedule;reminder;runtime"
    schedule_reminder_im_adapter_failure_test.cc
    "${ROOT_DIR}/components/voicelife_runtime/src/schedule_reminder_im_adapter.cc")
target_include_directories(schedule_reminder_im_adapter_failure_test PRIVATE
    "${ROOT_DIR}/components/voicelife_runtime/src")
target_link_libraries(schedule_reminder_im_adapter_failure_test PRIVATE im schedule timing storage_memory contracts)

add_voicelife_test(runtime_smoke_test "integration;runtime" runtime_smoke_test.cc)
target_link_libraries(runtime_smoke_test PRIVATE mcp voice)

add_voicelife_test(serial_voice_protocol_test "unit;runtime;voice" serial_voice_protocol_test.cc)
target_include_directories(serial_voice_protocol_test PRIVATE "${ROOT_DIR}/components/voicelife_runtime/src")

add_voicelife_test(usb_serial_frame_router_test "unit;runtime;voice;im" usb_serial_frame_router_test.cc
    "${ROOT_DIR}/components/voicelife_runtime/src/usb_serial_frame_router.cc")
target_include_directories(usb_serial_frame_router_test PRIVATE "${ROOT_DIR}/components/voicelife_runtime/src")
target_link_libraries(usb_serial_frame_router_test PRIVATE im contracts)

add_voicelife_test(wifi_provisioning_test "unit;runtime;wifi;provisioning"
    wifi_provisioning_test.cc
    "${ROOT_DIR}/components/voicelife_runtime/src/wifi_provisioning.cc")
target_include_directories(wifi_provisioning_test PRIVATE
    "${ROOT_DIR}/components/voicelife_runtime/src")

add_voicelife_test(sqlite_schedule_repository_test "integration;storage;sqlite;schedule"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/test/sqlite_schedule_repository_test.cc")
target_link_libraries(sqlite_schedule_repository_test PRIVATE storage_sqlite schedule)

add_voicelife_test(sqlite_database_test "unit;storage;sqlite;database"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/test/sqlite_database_test.cc")
target_link_libraries(sqlite_database_test PRIVATE storage_sqlite)

add_voicelife_test(sqlite_schema_test "unit;storage;sqlite;schema"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/test/sqlite_schema_test.cc")
target_link_libraries(sqlite_schema_test PRIVATE storage_sqlite)

add_voicelife_test(voicelife_schema_test "integration;storage;sqlite;schema"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/test/voicelife_schema_test.cc")
target_include_directories(voicelife_schema_test PRIVATE
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src")
target_link_libraries(voicelife_schema_test PRIVATE storage_sqlite)

add_voicelife_test(sqlite_schedule_repository_unit_test "unit;storage;sqlite;schedule;repository"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/test/sqlite_schedule_repository_unit_test.cc")
target_include_directories(sqlite_schedule_repository_unit_test PRIVATE
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src")
target_link_libraries(sqlite_schedule_repository_unit_test PRIVATE storage_sqlite schedule)

add_voicelife_test(sqlite_schedule_reminder_task_repository_test "unit;storage;sqlite;reminder;repository"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/test/sqlite_schedule_reminder_task_repository_test.cc")
target_link_libraries(sqlite_schedule_reminder_task_repository_test PRIVATE storage_sqlite schedule)

add_voicelife_test(sqlite_schedule_reminder_task_failure_test "unit;storage;sqlite;reminder;repository"
    sqlite_schedule_reminder_task_failure_test.cc)
target_include_directories(sqlite_schedule_reminder_task_failure_test PRIVATE
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src")
target_link_libraries(sqlite_schedule_reminder_task_failure_test PRIVATE storage_sqlite schedule)

add_voicelife_test(sqlite_schedule_reminder_mapper_branch_test "unit;storage;sqlite;reminder;mapping"
    sqlite_schedule_reminder_mapper_branch_test.cc)
target_include_directories(sqlite_schedule_reminder_mapper_branch_test PRIVATE
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src")
target_link_libraries(sqlite_schedule_reminder_mapper_branch_test PRIVATE storage_sqlite schedule)

add_voicelife_test(sqlite_schedule_rule_repository_test "integration;storage;sqlite;schedule"
    "${ROOT_DIR}/components/voicelife_storage_sqlite/test/sqlite_schedule_rule_repository_test.cc")
target_include_directories(sqlite_schedule_rule_repository_test PRIVATE
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src")
target_link_libraries(sqlite_schedule_rule_repository_test PRIVATE storage_sqlite schedule)

cmake_minimum_required(VERSION 3.20)
project(voicelife_host_tests LANGUAGES C CXX)

find_package(Threads REQUIRED)

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
    "${ROOT_DIR}/components/voicelife_contracts/src/im/reminder_action_result.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/reminder_action_command.cc"
    "${ROOT_DIR}/components/voicelife_contracts/src/im/notification_submission.cc")
target_link_libraries(contracts PRIVATE yyjson)
add_voicelife_library(im voicelife_im
    "${ROOT_DIR}/components/voicelife_im/src/im_reporting_channel.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_action_channel.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_sse.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_runtime.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_config_store.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_retry_policy.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_provisioning.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_wire.cc"
    "${ROOT_DIR}/components/voicelife_im/src/im_endpoint.cc"
    "${ROOT_DIR}/components/voicelife_im/src/transport/im_response_reader.cc")
target_include_directories(im PUBLIC "${ROOT_DIR}/components/voicelife_im/src/transport")
target_link_libraries(im PUBLIC contracts)
add_voicelife_library(schedule voicelife_schedule
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_create_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/mock/schedule_mock_data.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_operation_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/mock/schedule_operation_mock_data.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_operation_query_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_query_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/service/schedule_service.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/rules/schedule_time_rules.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_undo_helpers.cc"
    "${ROOT_DIR}/components/voicelife_schedule/src/helpers/schedule_update_helpers.cc")
target_include_directories(schedule PRIVATE "${ROOT_DIR}/components/voicelife_schedule/src")
target_link_libraries(schedule PUBLIC contracts Threads::Threads)
add_voicelife_library(timing voicelife_timing
    "${ROOT_DIR}/components/voicelife_timing/src/occurrence_planner.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_policy.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_service.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_service_calendar.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_service_delete_reminder_rule.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_service_reminder_triggers.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_service_reminder_rules.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_service_dismiss_reminder_trigger.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_service_snooze_reminder_trigger.cc"
    "${ROOT_DIR}/components/voicelife_timing/src/timing_task_state.cc")
target_link_libraries(timing PUBLIC contracts)
add_voicelife_library(mcp voicelife_mcp
    "${ROOT_DIR}/components/voicelife_mcp/src/mcp_server.cc"
    "${ROOT_DIR}/components/voicelife_mcp/src/mcp_json_writer.cc")
target_link_libraries(mcp PUBLIC contracts)
target_link_libraries(mcp PRIVATE yyjson)
add_voicelife_library(voice voicelife_voice
    "${ROOT_DIR}/components/voicelife_voice/src/audio_frame_queue.cc"
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
target_link_libraries(linx PUBLIC voice cjson)
add_voicelife_library(linx_esp voicelife_linx_esp
    "${ROOT_DIR}/components/voicelife_linx_esp/src/websocket_fragment_assembler.cc")
target_link_libraries(linx_esp PUBLIC contracts linx)
add_voicelife_library(storage_sqlite voicelife_storage_sqlite
    "${ROOT_DIR}/components/voicelife_storage_sqlite/src/storage_protocol.cc")
target_link_libraries(storage_sqlite PUBLIC contracts)
add_voicelife_library(audio_esp voicelife_audio_esp
    "${ROOT_DIR}/components/voicelife_audio_esp/src/audio_board_profile.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp32s3_audio_probe.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/pcm_frame_assembler.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp32s3_pcm_audio_port.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp32s3_pcm_i2s_runtime.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp_multinet_wake_detector.cc"
    "${ROOT_DIR}/components/voicelife_audio_esp/src/esp_wakenet_detector.cc")
target_link_libraries(audio_esp PUBLIC contracts voice)
add_voicelife_library(board_esp voicelife_board_esp
    "${ROOT_DIR}/components/voicelife_board_esp/src/sparkbot_profile.cc"
    "${ROOT_DIR}/components/voicelife_board_esp/src/gpio46_power_arbiter.cc"
    "${ROOT_DIR}/components/voicelife_board_esp/src/esp32s3_board_probe.cc"
    "${ROOT_DIR}/components/voicelife_board_esp/src/sparkbot_wake_model_assets.cc")
target_link_libraries(board_esp PUBLIC contracts)
add_voicelife_library(display_esp voicelife_display_esp
    "${ROOT_DIR}/components/voicelife_display_esp/src/ssd1306_presentation_adapter.cc")
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

add_voicelife_test(schedule_operation_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_operation_test.cc")
target_include_directories(schedule_operation_test PRIVATE
    "${ROOT_DIR}/components/voicelife_schedule/src")
target_link_libraries(schedule_operation_test PRIVATE schedule)

add_voicelife_test(schedule_recent_operation_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_recent_operation_test.cc")
target_include_directories(schedule_recent_operation_test PRIVATE
    "${ROOT_DIR}/components/voicelife_schedule/src")
target_link_libraries(schedule_recent_operation_test PRIVATE schedule)

add_voicelife_test(schedule_undo_operation_test "unit;schedule"
    "${ROOT_DIR}/components/voicelife_schedule/test/schedule_undo_operation_test.cc")
target_include_directories(schedule_undo_operation_test PRIVATE
    "${ROOT_DIR}/components/voicelife_schedule/src")
target_link_libraries(schedule_undo_operation_test PRIVATE schedule)

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

add_voicelife_test(timing_task_calendar_test "unit;timing" timing_task_calendar_test.cc)
target_link_libraries(timing_task_calendar_test PRIVATE timing)

add_voicelife_test(timing_occurrence_planner_test "unit;timing" timing_occurrence_planner_test.cc)
target_link_libraries(timing_occurrence_planner_test PRIVATE timing)

add_voicelife_test(timing_task_service_reminder_triggers_test "unit;timing" timing_task_service_reminder_triggers_test.cc)
target_link_libraries(timing_task_service_reminder_triggers_test PRIVATE timing)

add_voicelife_test(timing_task_service_snooze_test "unit;timing" timing_task_service_snooze_test.cc)
target_link_libraries(timing_task_service_snooze_test PRIVATE timing)

add_voicelife_test(timing_task_service_dismiss_test "unit;timing" timing_task_service_dismiss_test.cc)
target_link_libraries(timing_task_service_dismiss_test PRIVATE timing)

add_voicelife_test(timing_task_store_test "unit;timing" timing_task_store_test.cc)
target_link_libraries(timing_task_store_test PRIVATE timing)

add_voicelife_test(timing_task_service_contract_test "unit;timing" timing_task_service_contract_test.cc)
target_link_libraries(timing_task_service_contract_test PRIVATE timing)

add_voicelife_test(mcp_server_test "unit;mcp"
    "${ROOT_DIR}/components/voicelife_mcp/test/mcp_server_test.cc")
target_include_directories(mcp_server_test PRIVATE "${ROOT_DIR}/third_party/yyjson")
target_link_libraries(mcp_server_test PRIVATE mcp)

add_voicelife_test(schedule_mcp_tools_test "unit;mcp;schedule;runtime" schedule_mcp_tools_test.cc
    "${ROOT_DIR}/components/voicelife_runtime/src/schedule_mcp_tools.cc")
target_include_directories(schedule_mcp_tools_test PRIVATE "${ROOT_DIR}/components/voicelife_runtime/src")
target_link_libraries(schedule_mcp_tools_test PRIVATE mcp schedule)

add_voicelife_test(linx_mcp_bridge_test "unit;mcp;linx;runtime" linx_mcp_bridge_test.cc
    "${ROOT_DIR}/components/voicelife_runtime/src/linx_mcp_bridge.cc"
    "${ROOT_DIR}/components/voicelife_runtime/src/schedule_mcp_tools.cc")
target_include_directories(linx_mcp_bridge_test PRIVATE "${ROOT_DIR}/components/voicelife_runtime/src")
target_link_libraries(linx_mcp_bridge_test PRIVATE contracts mcp schedule)

add_voicelife_test(linx_ota_contract_test "unit;linx;contract" linx_ota_contract_test.cc)
target_link_libraries(linx_ota_contract_test PRIVATE contracts linx)

add_voicelife_test(voice_session_coordinator_test "unit;voice" voice_session_coordinator_test.cc)
target_link_libraries(voice_session_coordinator_test PRIVATE voice)

add_voicelife_test(voice_session_contract_test "unit;voice;contract" voice_session_contract_test.cc)
target_link_libraries(voice_session_contract_test PRIVATE voice)

add_voicelife_test(audio_frame_queue_contract_test "unit;voice;audio;contract"
    audio_frame_queue_contract_test.cc)
target_link_libraries(audio_frame_queue_contract_test PRIVATE voice)

add_voicelife_test(wake_gate_audio_input_test "unit;voice;audio;contract"
    wake_gate_audio_input_test.cc)
target_link_libraries(wake_gate_audio_input_test PRIVATE voice)

add_voicelife_test(voice_interaction_controller_test "unit;voice;interaction;contract"
    voice_interaction_controller_test.cc)
target_link_libraries(voice_interaction_controller_test PRIVATE voice)

add_voicelife_test(linx_provider_contract_test "unit;voice;linx;contract" linx_provider_contract_test.cc)
target_link_libraries(linx_provider_contract_test PRIVATE linx)

add_voicelife_test(linx_esp_transport_contract_test "unit;voice;linx;esp;contract"
    linx_esp_transport_contract_test.cc)
target_link_libraries(linx_esp_transport_contract_test PRIVATE linx_esp)

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
target_link_libraries(pcm_frame_assembler_contract_test PRIVATE audio_esp)

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

add_voicelife_test(im_runtime_test "unit;im;runtime" im_runtime_test.cc)
target_link_libraries(im_runtime_test PRIVATE im contracts)

add_voicelife_test(runtime_smoke_test "integration;runtime" runtime_smoke_test.cc)
target_link_libraries(runtime_smoke_test PRIVATE mcp voice)

add_voicelife_test(storage_protocol_contract_test "unit;storage;contract"
    storage_protocol_contract_test.cc)
target_link_libraries(storage_protocol_contract_test PRIVATE storage_sqlite)

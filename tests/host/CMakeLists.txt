cmake_minimum_required(VERSION 3.20)
project(voicelife_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Werror -Wpedantic)
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
    "${ROOT_DIR}/components/voicelife_platform/src/sequential_id_generator.cc")
target_link_libraries(platform PUBLIC contracts application)

enable_testing()
add_executable(runtime_smoke_test runtime_smoke_test.cc)
target_link_libraries(runtime_smoke_test PRIVATE mcp voice im platform)
add_test(NAME runtime_smoke_test COMMAND runtime_smoke_test)

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED VOICELIFE_ROOT)
    message(FATAL_ERROR "VOICELIFE_ROOT is required")
endif()

set(known_components
    voicelife_application
    voicelife_contracts
    voicelife_im
    voicelife_mcp
    voicelife_runtime
    voicelife_runtime_esp
    voicelife_schedule
    voicelife_storage_fatfs
    voicelife_storage_memory
    voicelife_storage_sqlite
    voicelife_timing
    voicelife_timing_esp
    voicelife_voice
    voicelife_linx
    voicelife_linx_esp
    voicelife_audio_esp
    voicelife_board_esp
    voicelife_display_esp
    voicelife_display_sparkbot
)

function(idf_component_register)
    cmake_parse_arguments(
        component
        ""
        ""
        "SRCS;SRC_DIRS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;REQUIRES;PRIV_REQUIRES"
        ${ARGN}
    )
    set(captured_public "${component_REQUIRES}" PARENT_SCOPE)
    set(captured_private "${component_PRIV_REQUIRES}" PARENT_SCOPE)
endfunction()

function(load_dependencies component_name)
    unset(captured_public)
    unset(captured_private)
    include("${VOICELIFE_ROOT}/components/${component_name}/CMakeLists.txt")
    set("actual_${component_name}_public" "${captured_public}" PARENT_SCOPE)
    set("actual_${component_name}_private" "${captured_private}" PARENT_SCOPE)
endfunction()

function(assert_dependencies component_name visibility)
    set(expected ${ARGN})
    string(TOLOWER "${visibility}" visibility_key)
    set(actual_variable "actual_${component_name}_${visibility_key}")
    set(actual "${${actual_variable}}")
    list(SORT expected)
    list(SORT actual)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "${component_name} ${visibility} dependencies mismatch: expected=[${expected}] actual=[${actual}]"
        )
    endif()
endfunction()

file(GLOB component_paths LIST_DIRECTORIES true "${VOICELIFE_ROOT}/components/*")
set(discovered_components)
foreach(component_path IN LISTS component_paths)
    if(IS_DIRECTORY "${component_path}")
        get_filename_component(component_name "${component_path}" NAME)
        if(NOT component_name MATCHES "^voicelife_[a-z0-9_]+$")
            message(FATAL_ERROR "Invalid component directory name: ${component_name}")
        endif()
        list(APPEND discovered_components "${component_name}")
    endif()
endforeach()
list(SORT discovered_components)
list(SORT known_components)
if(NOT "${discovered_components}" STREQUAL "${known_components}")
    message(FATAL_ERROR
        "Component inventory changed; update architecture rules: known=[${known_components}] actual=[${discovered_components}]"
    )
endif()

# 架构门禁按 SQLite 功能开启后的完整依赖图校验可选实现。
set(CONFIG_VOICELIFE_STORAGE_SQLITE ON)
set(CONFIG_VOICELIFE_STORAGE_FATFS ON)
set(CONFIG_VOICELIFE_STORAGE_FATFS_RUNTIME ON)

foreach(component_name IN LISTS known_components)
    string(REGEX REPLACE "^voicelife_" "" capability "${component_name}")
    if(NOT IS_DIRECTORY "${VOICELIFE_ROOT}/components/${component_name}/include/voicelife/${capability}")
        message(FATAL_ERROR "Public include path does not match component namespace: ${component_name}")
    endif()
    load_dependencies("${component_name}")
endforeach()

assert_dependencies(voicelife_contracts PUBLIC)
assert_dependencies(voicelife_contracts PRIVATE yyjson)
assert_dependencies(voicelife_application PUBLIC)
assert_dependencies(voicelife_application PRIVATE)
assert_dependencies(voicelife_im PUBLIC voicelife_contracts)
assert_dependencies(voicelife_im PRIVATE esp_http_client mbedtls)
assert_dependencies(voicelife_schedule PUBLIC voicelife_contracts)
assert_dependencies(voicelife_schedule PRIVATE voicelife_timing)
assert_dependencies(voicelife_storage_sqlite PUBLIC voicelife_contracts voicelife_schedule)
assert_dependencies(voicelife_storage_sqlite PRIVATE sqlite3)
assert_dependencies(voicelife_storage_fatfs PUBLIC voicelife_contracts)
assert_dependencies(voicelife_storage_fatfs PRIVATE esp_partition fatfs)
assert_dependencies(voicelife_storage_memory PUBLIC voicelife_contracts voicelife_schedule)
assert_dependencies(voicelife_storage_memory PRIVATE)
assert_dependencies(voicelife_timing PUBLIC voicelife_contracts)
assert_dependencies(voicelife_timing PRIVATE)
assert_dependencies(voicelife_timing_esp PUBLIC voicelife_contracts voicelife_timing)
assert_dependencies(voicelife_timing_esp PRIVATE esp_timer freertos)
assert_dependencies(voicelife_mcp PUBLIC voicelife_contracts voicelife_im)
assert_dependencies(voicelife_mcp PRIVATE voicelife_schedule yyjson)
assert_dependencies(voicelife_voice PUBLIC voicelife_contracts)
assert_dependencies(voicelife_voice PRIVATE)
assert_dependencies(voicelife_linx PUBLIC voicelife_contracts voicelife_voice)
assert_dependencies(voicelife_linx PRIVATE pthread)
assert_dependencies(voicelife_linx_esp PUBLIC voicelife_contracts voicelife_linx)
assert_dependencies(voicelife_linx_esp PRIVATE esp_websocket_client esp-tls esp_event esp_timer freertos heap)
assert_dependencies(voicelife_audio_esp PUBLIC voicelife_contracts voicelife_voice)
assert_dependencies(voicelife_display_esp PUBLIC voicelife_contracts voicelife_voice)
assert_dependencies(voicelife_display_esp PRIVATE driver esp_lcd esp_timer)
assert_dependencies(voicelife_display_sparkbot PUBLIC voicelife_contracts voicelife_voice)
assert_dependencies(voicelife_display_sparkbot PRIVATE esp_driver_spi esp_lcd esp_partition esp_psram freertos spi_flash)
assert_dependencies(voicelife_audio_esp PRIVATE esp_audio_codec esp_driver_i2c esp_driver_i2s esp_timer espressif__esp-sr)
assert_dependencies(voicelife_board_esp PUBLIC voicelife_contracts)
assert_dependencies(voicelife_board_esp PRIVATE esp_hw_support esp_partition esp_psram esp_system spi_flash)
assert_dependencies(voicelife_runtime PUBLIC voicelife_contracts)
assert_dependencies(voicelife_runtime_esp PUBLIC voicelife_application)
assert_dependencies(voicelife_runtime_esp PRIVATE freertos voicelife_mcp)
assert_dependencies(voicelife_runtime PRIVATE esp-tls esp_app_format esp_driver_gpio esp_driver_usb_serial_jtag led_strip esp_event esp_http_client esp_http_server esp_netif lwip esp_partition esp_psram esp_timer esp_wifi nvs_flash nvs_sec_provider spi_flash voicelife_application voicelife_im voicelife_linx voicelife_linx_esp voicelife_mcp voicelife_runtime_esp voicelife_voice voicelife_audio_esp voicelife_display_esp voicelife_schedule voicelife_storage_fatfs voicelife_storage_memory voicelife_storage_sqlite voicelife_timing_esp)

message(STATUS "PASS component names, include paths, and dependency graph")

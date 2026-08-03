cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED VOICELIFE_ROOT)
    message(FATAL_ERROR "VOICELIFE_ROOT is required")
endif()

set(known_components
    voicelife_contracts
    voicelife_mcp
    voicelife_runtime
    voicelife_schedule
    voicelife_timing
    voicelife_voice
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

foreach(component_name IN LISTS known_components)
    string(REGEX REPLACE "^voicelife_" "" capability "${component_name}")
    if(NOT IS_DIRECTORY "${VOICELIFE_ROOT}/components/${component_name}/include/voicelife/${capability}")
        message(FATAL_ERROR "Public include path does not match component namespace: ${component_name}")
    endif()
    load_dependencies("${component_name}")
endforeach()

assert_dependencies(voicelife_contracts PUBLIC)
assert_dependencies(voicelife_contracts PRIVATE)
assert_dependencies(voicelife_schedule PUBLIC voicelife_contracts)
assert_dependencies(voicelife_schedule PRIVATE)
assert_dependencies(voicelife_timing PUBLIC voicelife_contracts)
assert_dependencies(voicelife_timing PRIVATE)
assert_dependencies(voicelife_mcp PUBLIC voicelife_contracts)
assert_dependencies(voicelife_mcp PRIVATE)
assert_dependencies(voicelife_voice PUBLIC voicelife_contracts)
assert_dependencies(voicelife_voice PRIVATE)
assert_dependencies(voicelife_runtime PUBLIC voicelife_contracts)
assert_dependencies(voicelife_runtime PRIVATE voicelife_mcp voicelife_voice)

message(STATUS "PASS component names, include paths, and dependency graph")

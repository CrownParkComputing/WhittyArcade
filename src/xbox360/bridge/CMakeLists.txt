cmake_minimum_required(VERSION 3.18)
project(whitty_robotron_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(WHITTY_HOST_INCLUDE_DIR "" CACHE PATH
    "WhittyArcade public include directory")
set(WHITTY_REXGLUE_SDK_DIR "" CACHE PATH
    "ReXGlue SDK source directory")
set(WHITTY_REXGLUE_RUNTIME_DIR "" CACHE PATH
    "Directory containing the built ReXGlue release libraries")
set(WHITTY_ROBOTRON_GENERATED_DIR "" CACHE PATH
    "Directory containing Robotron's ReXGlue-generated sources.cmake")
set(WHITTY_OUTPUT_DIR "${CMAKE_BINARY_DIR}" CACHE PATH
    "Directory in which to stage the runtime module and its dependencies")

foreach(required_path IN ITEMS
        WHITTY_HOST_INCLUDE_DIR
        WHITTY_REXGLUE_SDK_DIR
        WHITTY_REXGLUE_RUNTIME_DIR
        WHITTY_ROBOTRON_GENERATED_DIR)
    if(NOT IS_DIRECTORY "${${required_path}}")
        message(FATAL_ERROR "${required_path} is not a directory: ${${required_path}}")
    endif()
endforeach()

if(NOT EXISTS "${WHITTY_ROBOTRON_GENERATED_DIR}/sources.cmake")
    message(FATAL_ERROR
        "Robotron generated sources are missing. Run ReXGlue codegen first: "
        "${WHITTY_ROBOTRON_GENERATED_DIR}/sources.cmake")
endif()
if(NOT EXISTS "${WHITTY_REXGLUE_RUNTIME_DIR}/librexruntime.so" OR
   NOT EXISTS "${WHITTY_REXGLUE_RUNTIME_DIR}/librexgpu-xenos.so" OR
   NOT EXISTS "${WHITTY_REXGLUE_RUNTIME_DIR}/libTracyClient.so" OR
   NOT EXISTS "${WHITTY_REXGLUE_RUNTIME_DIR}/libfmt.a")
    message(FATAL_ERROR
        "The selected ReXGlue runtime directory has not been built in Release mode")
endif()

include("${WHITTY_ROBOTRON_GENERATED_DIR}/sources.cmake")

# ReXGlue probes for RenderDoc whenever librenderdoc is installed. Loading the
# system capture layer inside a dlmopen namespace is unsafe and unnecessary for
# an embedded arcade runtime, so satisfy the optional probe with an empty DSO.
add_library(whitty_renderdoc_stub SHARED renderdoc_stub.cpp)
set_target_properties(whitty_renderdoc_stub PROPERTIES
    OUTPUT_NAME renderdoc
    LIBRARY_OUTPUT_DIRECTORY "${WHITTY_OUTPUT_DIR}")

add_library(whitty_robotron_runtime SHARED
    robotron_runtime.cpp
    ${GENERATED_SOURCES})

target_include_directories(whitty_robotron_runtime PRIVATE
    "${WHITTY_HOST_INCLUDE_DIR}"
    "${WHITTY_REXGLUE_SDK_DIR}/thirdparty/fmt/include"
    "${WHITTY_REXGLUE_SDK_DIR}/thirdparty/spdlog/include"
    "${WHITTY_REXGLUE_SDK_DIR}/thirdparty/tomlplusplus/include"
    "${WHITTY_REXGLUE_SDK_DIR}/include"
    "${WHITTY_REXGLUE_SDK_DIR}/thirdparty/simde"
    "${WHITTY_ROBOTRON_GENERATED_DIR}")
target_compile_definitions(whitty_robotron_runtime PRIVATE
    REX_PLATFORM_LINUX=1
    REX_HAS_VULKAN=1)
target_compile_options(whitty_robotron_runtime PRIVATE
    -O2
    -mcmodel=large
    -msse4.1
    -fno-strict-aliasing
    -ffp-model=strict
    -fno-char8_t)
target_link_options(whitty_robotron_runtime PRIVATE
    -Wl,--no-relax)
target_link_libraries(whitty_robotron_runtime PRIVATE
    "${WHITTY_REXGLUE_RUNTIME_DIR}/librexruntime.so"
    "${WHITTY_REXGLUE_RUNTIME_DIR}/libfmt.a")
add_dependencies(whitty_robotron_runtime whitty_renderdoc_stub)

set_target_properties(whitty_robotron_runtime PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${WHITTY_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

configure_file(stage_runtime.cmake.in stage_runtime.cmake @ONLY)
add_custom_command(TARGET whitty_robotron_runtime POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/stage_runtime.cmake"
    COMMENT "Staging the isolated ReXGlue runtime beside WhittyArcade")

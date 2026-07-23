cmake_minimum_required(VERSION 3.18)
project(WhittyArcade LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WHITTY_ENABLE_SYSTEM246
    "Build the Play!-backed Namco System 246/RRV runtime" ON)
set(WHITTY_PLAY_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/play"
    CACHE PATH "Pinned Play! source tree used by the System 246 runtime")

option(WHITTY_ENABLE_XBOX360
    "Build the isolated ReXGlue Robotron: 2084 runtime module" OFF)
set(WHITTY_REXGLUE_SDK_DIR "$ENV{REXSDK_DIR}" CACHE PATH
    "ReXGlue SDK source directory used for the Xbox 360 runtime")
set(WHITTY_REXGLUE_RUNTIME_DIR "" CACHE PATH
    "Directory containing the ReXGlue Release shared libraries")
set(WHITTY_ROBOTRON_GENERATED_DIR "" CACHE PATH
    "Directory containing Robotron ReXGlue-generated sources.cmake")
set(WHITTY_REXGLUE_CXX_COMPILER "" CACHE FILEPATH
    "Clang 20+ compiler used for the generated Xbox 360 code")

if(WHITTY_ENABLE_XBOX360)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "The isolated WhittyArcade/ReXGlue bridge currently supports Linux")
    endif()
    if(NOT WHITTY_REXGLUE_SDK_DIR)
        message(FATAL_ERROR
            "WHITTY_ENABLE_XBOX360 requires WHITTY_REXGLUE_SDK_DIR")
    endif()
    if(NOT WHITTY_REXGLUE_RUNTIME_DIR)
        set(WHITTY_REXGLUE_RUNTIME_DIR
            "${WHITTY_REXGLUE_SDK_DIR}/out/linux-amd64/Release"
            CACHE PATH "Directory containing ReXGlue Release libraries" FORCE)
    endif()
    if(NOT EXISTS "${WHITTY_ROBOTRON_GENERATED_DIR}/sources.cmake")
        message(FATAL_ERROR
            "WHITTY_ENABLE_XBOX360 requires WHITTY_ROBOTRON_GENERATED_DIR "
            "to contain Robotron's generated sources.cmake")
    endif()
    if(NOT WHITTY_REXGLUE_CXX_COMPILER)
        find_program(WHITTY_REXGLUE_CXX_COMPILER
            NAMES clang++-22 clang++
            REQUIRED)
    endif()
endif()

if(NOT WIN32)
    set(WHITTY_MATH_LIB m)
endif()

# Find required libraries
find_package(OpenGL REQUIRED)
find_package(SDL3 REQUIRED)
find_package(OpenAL REQUIRED)
find_package(GLEW REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET minizip)
pkg_check_modules(SDL3_TTF REQUIRED IMPORTED_TARGET sdl3-ttf)
pkg_check_modules(MPG123 REQUIRED IMPORTED_TARGET libmpg123)

if(WHITTY_ENABLE_SYSTEM246)
    if(NOT EXISTS "${WHITTY_PLAY_SOURCE_DIR}/Source/CMakeLists.txt")
        message(FATAL_ERROR
            "WHITTY_ENABLE_SYSTEM246=ON requires the pinned Play! submodule. "
            "Run: git submodule update --init --recursive")
    endif()

    # Play!'s platform header only sets these variables when Play! itself is
    # the top-level project. Set the same narrow platform contract here before
    # adding its core, headless Vulkan GS and OpenAL output targets.
    if(WIN32)
        set(TARGET_PLATFORM_WIN32 TRUE)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(TARGET_PLATFORM_WIN32_X64 TRUE)
        else()
            set(TARGET_PLATFORM_WIN32_X86 TRUE)
        endif()
    elseif(APPLE)
        set(TARGET_PLATFORM_MACOS TRUE)
    else()
        set(TARGET_PLATFORM_UNIX TRUE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64.*|AARCH64.*)")
            set(TARGET_PLATFORM_UNIX_AARCH64 TRUE)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
            set(TARGET_PLATFORM_UNIX_ARM TRUE)
        endif()
    endif()
    list(PREPEND CMAKE_MODULE_PATH
        "${WHITTY_PLAY_SOURCE_DIR}/deps/Dependencies/cmake-modules")
    set(ENABLE_AMAZON_S3 OFF CACHE BOOL
        "Disable Play! network-backed disc loading" FORCE)
    # Avoid libchdr's bundled zlib CMake renaming a tracked source header.
    set(WITH_SYSTEM_ZLIB ON CACHE BOOL
        "Use WhittyArcade's system zlib for Play!/libchdr" FORCE)

    add_subdirectory("${WHITTY_PLAY_SOURCE_DIR}/Source"
        "${CMAKE_BINARY_DIR}/play-core" EXCLUDE_FROM_ALL)
    # Recent system zlib headers no longer define the legacy STDC macros used
    # by zstd's compatibility wrapper. The wrapper is C99 and safe to mark so.
    if(TARGET libzstd_zlibwrapper_static AND NOT MSVC)
        target_compile_definitions(libzstd_zlibwrapper_static PRIVATE
            STDC=1 STDC99=1 Z_HAVE_STDARG_H=1)
    endif()
    add_subdirectory("${WHITTY_PLAY_SOURCE_DIR}/Source/gs/GSH_Vulkan"
        "${CMAKE_BINARY_DIR}/play-gsh-vulkan" EXCLUDE_FROM_ALL)
    add_subdirectory("${WHITTY_PLAY_SOURCE_DIR}/Source/sound/SH_OpenAL"
        "${CMAKE_BINARY_DIR}/play-sh-openal" EXCLUDE_FROM_ALL)

    add_library(system246_rrv_music STATIC
        src/system246/rrv_music.cpp)
    target_include_directories(system246_rrv_music PUBLIC
        ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(system246_rrv_music SYSTEM PRIVATE
        ${WHITTY_PLAY_SOURCE_DIR}/deps/libchdr/include)
    target_link_libraries(system246_rrv_music PUBLIC
        chdr-static Threads::Threads)
    target_compile_options(system246_rrv_music PRIVATE
        -Wall -Wextra -Wpedantic)

    add_library(system246_video_geometry STATIC
        src/system246/video_deinterlace.cpp)
    target_include_directories(system246_video_geometry PUBLIC
        ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(system246_video_geometry PRIVATE
        -Wall -Wextra -Wpedantic)
endif()

# FindGLEW can be backed by either CMake's module or a distribution-provided
# config package. Prefer its imported target because openSUSE's config does
# not populate the legacy GLEW_LIBRARIES variable.
if(TARGET GLEW::GLEW)
    set(WHITTY_GLEW_TARGET GLEW::GLEW)
elseif(TARGET GLEW::glew)
    set(WHITTY_GLEW_TARGET GLEW::glew)
else()
    set(WHITTY_GLEW_TARGET ${GLEW_LIBRARIES})
endif()

# Standalone MC68020 core. Musashi's 020 effective-address implementation
# shares compile-time model predicates with the 010 and EC020 paths, so those
# lower variants must remain enabled even though this machine selects a 68020.
add_library(musashi STATIC
    third_party/musashi/m68kcpu.c
    third_party/musashi/m68kdasm.c
    third_party/musashi/m68kops.c
    third_party/musashi/softfloat/softfloat.c
)
target_include_directories(musashi PUBLIC third_party/musashi)
target_compile_definitions(musashi PRIVATE
    M68K_EMULATE_010=1
    M68K_EMULATE_EC020=1
    M68K_EMULATE_020=1
    M68K_EMULATE_030=0
    M68K_EMULATE_040=0
)
target_compile_options(musashi PRIVATE -w)

# Moira (MIT, Dirk W. Hoffmann): instance-based Motorola 68k core. Unlike
# Musashi, every CPU is an independent object, so dual-68000 boards (main +
# sound CPU) coexist in one process. Requires C++20; kept isolated to this
# target so the rest of the tree stays on C++17.
add_library(moira STATIC
    third_party/moira/Moira/Moira.cpp
    third_party/moira/Moira/MoiraDebugger.cpp
)
target_include_directories(moira PUBLIC third_party/moira/Moira)
# Moira's generated instruction tables make GCC's -O3 pass consume several
# gigabytes while compiling.  -O1 keeps the interpreter optimized without
# making release builds unreliable on normal desktop machines.
target_compile_options(moira PRIVATE -w -O1)
set_target_properties(moira PROPERTIES CXX_STANDARD 20)

# Project-facing multi-instance 68000 wrapper around Moira.
add_library(m68000_cpu STATIC src/m68000_cpu.cpp)
target_include_directories(m68000_cpu PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(m68000_cpu PUBLIC moira)
set_target_properties(m68000_cpu PROPERTIES CXX_STANDARD 20)

# ymfm (BSD-3-Clause, Aaron Giles / MAME 3rdparty). Used by Shinobi (YM2151
# OPM) and any future board that needs YM-series FM synthesis. Vendor copy
# was taken from MAME 3rdparty/ymfm unchanged, except LICENSE moved alongside.
add_library(ymfm STATIC
    third_party/ymfm/src/ymfm_misc.cpp
    third_party/ymfm/src/ymfm_adpcm.cpp
    third_party/ymfm/src/ymfm_opm.cpp
    third_party/ymfm/src/ymfm_opn.cpp
    third_party/ymfm/src/ymfm_opl.cpp
    third_party/ymfm/src/ymfm_opq.cpp
    third_party/ymfm/src/ymfm_opz.cpp
    third_party/ymfm/src/ymfm_ssg.cpp
    third_party/ymfm/src/ymfm_pcm.cpp
)
target_include_directories(ymfm SYSTEM PUBLIC third_party/ymfm/src)
target_compile_options(ymfm PRIVATE -w)

# Sega 8-bit PCM chip (BSD-3-Clause; MAME segapcm.cpp/.h, downstream of
# segapcm.cpp authors Hiromitsu Shioya / Olivier Galibert). Used by Shinobi's
# PCM voice playback. Standalone — no external bus dependency.
add_library(segapcm STATIC third_party/segapcm/segapcm.cpp)
target_include_directories(segapcm SYSTEM PUBLIC third_party/segapcm)
target_compile_options(segapcm PRIVATE -w)

# Standalone Mitsubishi M37702 core used by Namco's C74 sound MCU. This is a
# framework-free adaptation of MAME's BSD-3-Clause m37710 implementation.
add_library(m37710 STATIC
    third_party/m37710/m37710.cpp
    third_party/m37710/m37710o0.cpp
    third_party/m37710/m37710o1.cpp
    third_party/m37710/m37710o2.cpp
    third_party/m37710/m37710o3.cpp
)
target_include_directories(m37710 PUBLIC third_party/m37710)
target_compile_options(m37710 PRIVATE -w)

# Standalone NEC V60 core for Sega Model 1. This is a framework-free adapter
# around MAME's BSD-3-Clause instruction implementation; no MAME runtime is
# linked or required.
add_library(v60 STATIC
    third_party/v60/v60.cpp
    third_party/v60/v60d.cpp
)
target_include_directories(v60 PUBLIC third_party/v60)
target_compile_options(v60 PRIVATE -w)

# Standalone Intel i960KB core used by Sega Model 2. This adapts MAME's
# BSD-3-Clause implementation to byte-wide host memory callbacks.
add_library(i960 STATIC
    third_party/i960/i960.cpp
    third_party/i960/i960dis.cpp
)
target_include_directories(i960 PUBLIC third_party/i960)
target_compile_options(i960 PRIVATE -O2 -w)

# Command-level HLE for the Virtua Racing family TGP. Adapted from MAME's
# BSD-3-Clause Model 1 implementation and isolated from its device framework.
add_library(model1_tgp STATIC
    third_party/model1/model1_tgp_hle.cpp
    third_party/model1/model1_tgp_lle.cpp
)
target_include_directories(model1_tgp PUBLIC
    third_party/model1
    third_party/mb86233
)
target_compile_options(model1_tgp PRIVATE -w)

# Fujitsu MB86233 geometry processor used by Virtua Racing/Formula. This is a
# framework-free adapter around MAME's BSD-3-Clause CPU implementation.
add_library(mb86233 STATIC
    third_party/mb86233/mb86233.cpp
    third_party/mb86233/mb86233_native.cpp
)
target_include_directories(mb86233 PUBLIC third_party/mb86233)
target_compile_options(mb86233 PRIVATE -O2 -w)

add_library(system24_video STATIC src/system24_tile_video.cpp)
target_include_directories(system24_video PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_options(system24_video PRIVATE -O2 -Wall -Wextra -Wpedantic)

add_library(model1_video STATIC third_party/model1/model1_video.cpp)
target_include_directories(model1_video PUBLIC
    third_party/model1
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model1_video PUBLIC system24_video glm::glm)
target_compile_options(model1_video PRIVATE -w)

add_library(chips_z80 STATIC third_party/chips/z80_impl.cpp)
target_include_directories(chips_z80 SYSTEM PUBLIC third_party/chips)
target_compile_options(chips_z80 PRIVATE -w)

# Portable Namco System 1 core, adapted from the user's Galaga '88 host port.
add_library(namcos1_core STATIC
    third_party/namcos1/namcos1_machine.c
    third_party/namcos1/namcos1_render.c
    third_party/namcos1/mc6809.c
    third_party/namcos1/m6801.c)
target_include_directories(namcos1_core PUBLIC third_party/namcos1)
target_compile_options(namcos1_core PRIVATE -w)

# Instance-based MC6809 interpreter imported from the MIT-licensed GnG
# Amiga prototype and adapted to use a per-machine I/O context.
add_library(m6809f STATIC
    third_party/m6809f/m6809f.c
    third_party/m6809f/m6809f_ops_alu16.c
    third_party/m6809f/m6809f_ops_alu8.c
    third_party/m6809f/m6809f_ops_branch.c
    third_party/m6809f/m6809f_ops_load.c
    third_party/m6809f/m6809f_ops_misc.c
    third_party/m6809f/m6809f_ops_page2.c
    third_party/m6809f/m6809f_ops_page3.c
    third_party/m6809f/m6809f_ops_rmw.c
    third_party/m6809f/m6809f_ops_stack.c)
target_include_directories(m6809f PUBLIC third_party/m6809f)
target_compile_options(m6809f PRIVATE -O2 -w)

add_library(model1_io STATIC src/sega/model1/model1_io_board.cpp)
target_include_directories(model1_io PUBLIC
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(model1_io SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_compile_options(model1_io PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(model1_io PUBLIC chips_z80)

# Model 1 sound devices. YM3438 synthesis uses the BSD-3-Clause ymfm library;
# the standalone MultiPCM core is adapted from MAME's BSD-3-Clause source.
add_library(model1_multipcm STATIC
    third_party/model1_audio/model1_multipcm.c)
target_include_directories(model1_multipcm PUBLIC third_party/model1_audio)
target_compile_options(model1_multipcm PRIVATE -w)

add_library(model1_dsb STATIC src/sega/model1/model1_dsb.cpp)
target_include_directories(model1_dsb PUBLIC
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(model1_dsb SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(model1_dsb PUBLIC chips_z80 PkgConfig::MPG123)
target_compile_options(model1_dsb PRIVATE -Wall -Wextra -Wpedantic)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${SDL3_INCLUDE_DIRS})
include_directories(${GLEW_INCLUDE_DIRS})

add_library(arcade_audio_output STATIC src/arcade_audio_output.cpp)
target_include_directories(arcade_audio_output PUBLIC
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(arcade_audio_output PRIVATE
    -Wall -Wextra -Wpedantic)

# Source files
set(SOURCES
    src/main.cpp
    src/arcade_session.cpp
    src/namco/system22/system22_session.cpp
    src/sega/model1/model1_session.cpp
    src/sega/model2/model2_session.cpp
    src/namco/galaxian/galaxian_session.cpp
    src/sega/system16b/system16b_session.cpp
    src/capcom/gng/gng_session.cpp
    src/namco/namco_session.cpp
    src/arcade_renderer.cpp
    src/arcade_input.cpp
    src/network_video_link.cpp
    src/multiplayer_lobby.cpp
    src/input_mapping.cpp
    src/input_mapper.cpp
    src/arcade_frontend.cpp
    src/launcher_menu.cpp
    src/arcade_settings.cpp
    src/arcade_catalog.cpp
    src/arcade_game_probe.cpp
    src/rom_library.cpp
    src/persistent_data.cpp
    src/high_scores.cpp
    src/arcade_presenter.cpp
    src/arcade_video_worker.cpp
    src/namco/system22/system22_audio.cpp
    src/musashi_memory.cpp
    src/namco/system22/system22_cpu.cpp
    src/namco/system22/system22_dsp.cpp
    src/namco/system22/system22_mcu.cpp
    src/namco/system22/system22_sprites.cpp
    src/namco/system22/system22_tms320c25.cpp
    src/namco/system22/system22_rom.cpp
    src/system246_rom.cpp
    src/system246_controls.cpp
    src/xbox360_rom.cpp
    src/xbox360_runtime_loader.cpp
    src/xbox360_audio.cpp
    src/xbox360_session.cpp
    src/sega/model1/model1_rom.cpp
    src/sega/model1/model1_machine.cpp
    src/sega/model1/model1_audio.cpp
    src/sega/model2/model2_rom.cpp
    src/sega/model2/model2_scsp_dsp.cpp
    src/sega/model2/model2_audio.cpp
    src/sega/model2/model2_bus.cpp
    src/sega/model2/model2_machine.cpp
    src/sega/model2/model2_geometry.cpp
    src/sega/model2/model2_video.cpp
    src/namco/galaxian/galaxian_rom.cpp
    src/namco/galaxian/galaxian_audio.cpp
    src/namco/galaxian/galaxian_audio_mooncrst.cpp
    src/namco/galaxian/galaxian_audio_phoenix.cpp
    src/namco/galaxian/galaxian_machine.cpp
    src/namco/galaxian/galaxian_machine_mooncrst.cpp
    src/namco/galaxian/galaxian_machine_phoenix.cpp
    src/sega/system16b/system16b_rom.cpp
    src/sega/system16b/system16b_audio.cpp
    src/sega/system16b/system16b_machine.cpp
    src/sega/system16b/system16b_machine_shinobi.cpp
    src/capcom/gng/gng_rom.cpp
    src/capcom/gng/gng_machine.cpp
    src/capcom/gng/gng_audio.cpp
    src/namco/namco_rom.cpp
    src/namco/galaga/galaga_machine.cpp
    src/namco/galaga/galaga_audio.cpp
    src/namco/system1/system1_machine.cpp
    src/namco/system1/system1_audio.cpp
    src/namco/system1/system1_ym2151.cpp
)

if(WHITTY_ENABLE_SYSTEM246)
    list(APPEND SOURCES
        src/system246_session.cpp
        src/system246/play_audio.cpp
        src/system246/play_core.cpp
        src/system246/play_input.cpp
        src/system246/play_media_cache.cpp
        src/system246/play_video.cpp
    )
else()
    list(APPEND SOURCES src/system246_session_stub.cpp)
endif()

if(WIN32)
    enable_language(RC)
    list(APPEND SOURCES
        src/platform_file_dialog_windows.cpp
        resources/windows/WhittyArcade.rc
    )
else()
    list(APPEND SOURCES src/platform_file_dialog_unix.cpp)
endif()

# Build executable
if(WIN32)
    add_executable(WhittyArcade WIN32 ${SOURCES})
else()
    add_executable(WhittyArcade ${SOURCES})
endif()
target_include_directories(WhittyArcade PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/moira/Moira)
if(WHITTY_ENABLE_SYSTEM246)
    target_include_directories(WhittyArcade SYSTEM PRIVATE
        ${WHITTY_PLAY_SOURCE_DIR}/Source
        ${WHITTY_PLAY_SOURCE_DIR}/deps/CodeGen/include
        ${WHITTY_PLAY_SOURCE_DIR}/deps/Framework/include
        ${WHITTY_PLAY_SOURCE_DIR}/deps/libchdr/include)
    target_compile_definitions(WhittyArcade PRIVATE
        WHITTY_ENABLE_SYSTEM246=1)
endif()

# Link libraries
target_link_libraries(WhittyArcade
    PRIVATE
    ${OPENGL_LIBRARIES}
    SDL3::SDL3
    ${OPENAL_LIBRARY}
    ${WHITTY_GLEW_TARGET}
    musashi
    m37710
    v60
    i960
    model1_tgp
    mb86233
    model1_video
    model1_io
    model1_dsb
    model1_multipcm
    arcade_audio_output
    m68000_cpu
    ymfm
    segapcm
    m6809f
    namcos1_core
    chips_z80
    Threads::Threads
    PkgConfig::MINIZIP
    PkgConfig::SDL3_TTF
    ZLIB::ZLIB
    Vulkan::Vulkan
    ${WHITTY_MATH_LIB}
    ${CMAKE_DL_LIBS}
)
if(WHITTY_ENABLE_SYSTEM246)
    # Static-library order matters: the Vulkan backend consumes GS symbols
    # implemented by PlayCore, so PlayCore must follow the backend targets.
    target_link_libraries(WhittyArcade PRIVATE
        gsh_vulkan sh_openal PlayCore system246_rrv_music
        system246_video_geometry chdr-static)
endif()
if(WIN32)
    target_link_libraries(WhittyArcade PRIVATE ole32 shell32 uuid ws2_32)
endif()

# Compiler flags
target_compile_options(WhittyArcade PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-O2;-Wall;-Wextra;-Wpedantic>"
)

if(WHITTY_ENABLE_XBOX360)
    include(ExternalProject)
    ExternalProject_Add(whitty_robotron_runtime_project
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/xbox360/bridge"
        BINARY_DIR "${CMAKE_BINARY_DIR}/xbox360-bridge"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_CXX_COMPILER=${WHITTY_REXGLUE_CXX_COMPILER}
            -DWHITTY_HOST_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/include
            -DWHITTY_REXGLUE_SDK_DIR=${WHITTY_REXGLUE_SDK_DIR}
            -DWHITTY_REXGLUE_RUNTIME_DIR=${WHITTY_REXGLUE_RUNTIME_DIR}
            -DWHITTY_ROBOTRON_GENERATED_DIR=${WHITTY_ROBOTRON_GENERATED_DIR}
            -DWHITTY_OUTPUT_DIR=${CMAKE_BINARY_DIR}
        BUILD_BYPRODUCTS
            "${CMAKE_BINARY_DIR}/libwhitty_robotron_runtime.so"
            "${CMAKE_BINARY_DIR}/librexruntime.so"
            "${CMAKE_BINARY_DIR}/librexgpu-xenos.so"
            "${CMAKE_BINARY_DIR}/libTracyClient.so"
            "${CMAKE_BINARY_DIR}/librenderdoc.so"
        UPDATE_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
        USES_TERMINAL_BUILD TRUE)
    add_dependencies(WhittyArcade whitty_robotron_runtime_project)
endif()

enable_testing()
add_test(NAME video_session_lifecycle_test
    COMMAND WhittyArcade --video-lifecycle-test)
set_tests_properties(video_session_lifecycle_test PROPERTIES
    ENVIRONMENT "DISPLAY=;WAYLAND_DISPLAY=;WHITTYARCADE_HEADLESS=1")

add_test(NAME arcade_session_factory_test
    COMMAND WhittyArcade --session-factory-test)

add_executable(arcade_audio_output_test tests/arcade_audio_output_test.cpp)
target_link_libraries(arcade_audio_output_test PRIVATE arcade_audio_output)
target_compile_options(arcade_audio_output_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME arcade_audio_output_test COMMAND arcade_audio_output_test)

add_executable(arcade_settings_test
    tests/arcade_settings_test.cpp
    src/arcade_settings.cpp
)
target_include_directories(arcade_settings_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(arcade_settings_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME arcade_settings_test COMMAND arcade_settings_test)

add_executable(arcade_lifecycle_test tests/arcade_lifecycle_test.cpp)
target_include_directories(arcade_lifecycle_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(arcade_lifecycle_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME arcade_lifecycle_test COMMAND arcade_lifecycle_test)

add_executable(input_mapping_test
    tests/input_mapping_test.cpp
    src/input_mapping.cpp
)
target_include_directories(input_mapping_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(input_mapping_test PRIVATE SDL3::SDL3)
target_compile_options(input_mapping_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
target_compile_definitions(input_mapping_test PRIVATE SDL_MAIN_HANDLED)
add_test(NAME input_mapping_test COMMAND input_mapping_test)

add_executable(arcade_catalog_test
    tests/arcade_catalog_test.cpp
    src/arcade_catalog.cpp
    src/arcade_game_probe.cpp
    src/namco/system22/system22_rom.cpp
    src/system246_rom.cpp
    src/xbox360_rom.cpp
    src/sega/model1/model1_rom.cpp
    src/sega/model2/model2_rom.cpp
    src/namco/galaxian/galaxian_rom.cpp
    src/sega/system16b/system16b_rom.cpp
    src/capcom/gng/gng_rom.cpp
    src/namco/namco_rom.cpp
)
target_include_directories(arcade_catalog_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(arcade_catalog_test PRIVATE
    PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(arcade_catalog_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME arcade_catalog_test COMMAND arcade_catalog_test)

add_executable(rom_library_test
    tests/rom_library_test.cpp
    src/arcade_catalog.cpp
    src/arcade_game_probe.cpp
    src/rom_library.cpp
    src/namco/system22/system22_rom.cpp
    src/system246_rom.cpp
    src/xbox360_rom.cpp
    src/sega/model1/model1_rom.cpp
    src/sega/model2/model2_rom.cpp
    src/namco/galaxian/galaxian_rom.cpp
    src/sega/system16b/system16b_rom.cpp
    src/capcom/gng/gng_rom.cpp
    src/namco/namco_rom.cpp
)
target_include_directories(rom_library_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(rom_library_test PRIVATE PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(rom_library_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME rom_library_test COMMAND rom_library_test)

add_executable(namco_rom_test
    tests/namco_rom_test.cpp
    src/namco/namco_rom.cpp)
target_include_directories(namco_rom_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(namco_rom_test PRIVATE PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(namco_rom_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
set(NAMCO_GALAGA_TEST_ROM "" CACHE FILEPATH
    "Optional galaga.zip used by the Namco ROM integration test")
set(NAMCO_PACMANIA_TEST_ROM "" CACHE FILEPATH
    "Optional pacmania.zip used by the Namco ROM integration test")
if(NAMCO_GALAGA_TEST_ROM AND NAMCO_PACMANIA_TEST_ROM)
    add_test(NAME namco_rom_test COMMAND namco_rom_test
        "${NAMCO_GALAGA_TEST_ROM}" "${NAMCO_PACMANIA_TEST_ROM}")
else()
    add_test(NAME namco_rom_test COMMAND namco_rom_test)
endif()

add_executable(gng_machine_test
    tests/gng_machine_test.cpp
    src/capcom/gng/gng_rom.cpp
    src/capcom/gng/gng_machine.cpp
    src/high_scores.cpp)
target_include_directories(gng_machine_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(gng_machine_test PRIVATE
    m6809f PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(gng_machine_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME gng_machine_test COMMAND gng_machine_test)
set(GNG_TEST_ROM "" CACHE FILEPATH
    "Optional gng.zip used by the Ghosts'n Goblins boot/gameplay test")
if(GNG_TEST_ROM)
    add_test(NAME gng_rom_integration_test
        COMMAND gng_machine_test "${GNG_TEST_ROM}")
    set_tests_properties(gng_rom_integration_test PROPERTIES
        LABELS "gng;rom;integration")
endif()

add_executable(system246_rom_test
    tests/system246_rom_test.cpp
    src/system246_rom.cpp
)
target_include_directories(system246_rom_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(system246_rom_test PRIVATE
    PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(system246_rom_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
set(SYSTEM246_TEST_ROM "" CACHE FILEPATH
    "Optional rrvac.zip used by the System 246 ROM/media integration test")
set(SYSTEM246_TEST_CHD "" CACHE FILEPATH
    "Optional rrv1-a.chd used by the System 246 ROM/media integration test")
if(SYSTEM246_TEST_ROM AND SYSTEM246_TEST_CHD)
    add_test(NAME system246_rom_test
        COMMAND system246_rom_test ${SYSTEM246_TEST_ROM} ${SYSTEM246_TEST_CHD})
else()
add_test(NAME system246_rom_test COMMAND system246_rom_test)
endif()

add_executable(system246_controls_test
    tests/system246_controls_test.cpp
    src/system246_controls.cpp
)
target_include_directories(system246_controls_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(system246_controls_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME system246_controls_test COMMAND system246_controls_test)

set_tests_properties(system246_rom_test system246_controls_test PROPERTIES
    LABELS system246)

add_executable(xbox360_rom_test
    tests/xbox360_rom_test.cpp
    src/xbox360_rom.cpp
)
target_include_directories(xbox360_rom_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(xbox360_rom_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
set(XBOX360_TEST_GAME "" CACHE PATH
    "Optional extracted Robotron: 2084 directory used by the XEX test")
if(XBOX360_TEST_GAME)
    add_test(NAME xbox360_rom_test
        COMMAND xbox360_rom_test "${XBOX360_TEST_GAME}")
else()
    add_test(NAME xbox360_rom_test COMMAND xbox360_rom_test)
endif()
set_tests_properties(xbox360_rom_test PROPERTIES LABELS xbox360)

if(WHITTY_ENABLE_XBOX360)
    add_executable(xbox360_runtime_smoke_test
        tests/xbox360_runtime_smoke_test.cpp
        src/xbox360_runtime_loader.cpp)
    target_include_directories(xbox360_runtime_smoke_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(xbox360_runtime_smoke_test PRIVATE
        Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(xbox360_runtime_smoke_test PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    add_dependencies(xbox360_runtime_smoke_test
        whitty_robotron_runtime_project)
    if(XBOX360_TEST_GAME)
        add_test(NAME xbox360_runtime_smoke_test
            COMMAND xbox360_runtime_smoke_test
                "${XBOX360_TEST_GAME}"
                "${CMAKE_BINARY_DIR}/xbox360-test-user"
                "${CMAKE_BINARY_DIR}/xbox360-test-cache")
        set_tests_properties(xbox360_runtime_smoke_test PROPERTIES
            LABELS "xbox360;runtime"
            TIMEOUT 45)
    endif()
endif()

if(WHITTY_ENABLE_SYSTEM246)
    add_executable(system246_video_test tests/system246_video_test.cpp)
    target_include_directories(system246_video_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/play/Source)
    target_link_libraries(system246_video_test PRIVATE
        system246_video_geometry)
    target_compile_options(system246_video_test PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME system246_video_test COMMAND system246_video_test)
    set_tests_properties(system246_video_test PROPERTIES
        LABELS "system246;video")

    add_executable(system246_music_test tests/system246_music_test.cpp)
    target_include_directories(system246_music_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SDL3_INCLUDE_DIRS})
    target_link_libraries(system246_music_test PRIVATE
        system246_rrv_music SDL3::SDL3)
    target_compile_options(system246_music_test PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    if(SYSTEM246_TEST_CHD)
        add_test(NAME system246_music_test
            COMMAND system246_music_test --verify "${SYSTEM246_TEST_CHD}")
    else()
        add_test(NAME system246_music_test COMMAND system246_music_test)
    endif()
    set_tests_properties(system246_music_test PROPERTIES
        LABELS "system246;audio;media"
        TIMEOUT 30)

    add_executable(system246_cd_test tests/system246_cd_test.cpp)
    target_include_directories(system246_cd_test PRIVATE
        ${WHITTY_PLAY_SOURCE_DIR}/Source)
    target_compile_options(system246_cd_test PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    set(SYSTEM246_TEST_ISO "" CACHE FILEPATH
        "Optional generated RRV1_A ISO used by the CD/audio integration test")
    if(SYSTEM246_TEST_ISO)
        add_test(NAME system246_cd_test
            COMMAND system246_cd_test "${SYSTEM246_TEST_ISO}")
    else()
        add_test(NAME system246_cd_test COMMAND system246_cd_test)
    endif()
    set_tests_properties(system246_cd_test PROPERTIES
        LABELS "system246;media")

    add_executable(system246_spu2_test tests/system246_spu2_test.cpp)
    target_include_directories(system246_spu2_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(system246_spu2_test SYSTEM PRIVATE
        ${WHITTY_PLAY_SOURCE_DIR}/Source
        ${WHITTY_PLAY_SOURCE_DIR}/deps/Framework/include)
    target_link_libraries(system246_spu2_test PRIVATE
        PlayCore arcade_audio_output)
    target_compile_options(system246_spu2_test PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME system246_spu2_test COMMAND system246_spu2_test)
    set_tests_properties(system246_spu2_test PROPERTIES
        LABELS "system246;audio")

    # Builds and runs only the quick, headless System 246 checks. This avoids
    # starting the emulator or requiring the RRV ROM/CHD for normal iteration.
    add_custom_target(check_system246
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L system246
        DEPENDS
            system246_rom_test
            system246_controls_test
            system246_cd_test
            system246_music_test
            system246_spu2_test
            system246_video_test
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL)
endif()

add_executable(persistent_data_test
    tests/persistent_data_test.cpp
    src/arcade_catalog.cpp
    src/persistent_data.cpp
)
target_include_directories(persistent_data_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(persistent_data_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME persistent_data_test COMMAND persistent_data_test)

add_executable(high_scores_test
    tests/high_scores_test.cpp
    src/high_scores.cpp
)
target_include_directories(high_scores_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(high_scores_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME high_scores_test COMMAND high_scores_test)

add_executable(system22_cpu_test
    tests/system22_cpu_test.cpp
    src/musashi_memory.cpp
    src/namco/system22/system22_cpu.cpp
)
target_include_directories(system22_cpu_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(system22_cpu_test PRIVATE
    musashi Threads::Threads ${WHITTY_MATH_LIB})
target_compile_options(system22_cpu_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME system22_cpu_test COMMAND system22_cpu_test)

add_executable(system22_sprite_test
    tests/system22_sprite_test.cpp
    src/namco/system22/system22_sprites.cpp)
target_include_directories(system22_sprite_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(system22_sprite_test PRIVATE
    -Wall -Wextra -Wpedantic)
add_test(NAME system22_sprite_test COMMAND system22_sprite_test)

add_executable(system22_rom_test
    tests/system22_rom_test.cpp
    src/namco/system22/system22_rom.cpp
)
target_include_directories(system22_rom_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(system22_rom_test PRIVATE
    PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(system22_rom_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
set(SYSTEM22_TEST_ROMS "" CACHE STRING
    "Semicolon-separated working System 22 archives for ROM integration tests")
foreach(SYSTEM22_TEST_ROM IN LISTS SYSTEM22_TEST_ROMS)
    get_filename_component(SYSTEM22_TEST_NAME "${SYSTEM22_TEST_ROM}" NAME_WE)
    string(MAKE_C_IDENTIFIER "${SYSTEM22_TEST_NAME}" SYSTEM22_TEST_ID)
    add_test(NAME system22_${SYSTEM22_TEST_ID}_rom_test
        COMMAND system22_rom_test "${SYSTEM22_TEST_ROM}")
    set_tests_properties(system22_${SYSTEM22_TEST_ID}_rom_test PROPERTIES
        RUN_SERIAL TRUE)
endforeach()

add_executable(i960_test tests/i960_test.cpp)
target_include_directories(i960_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/i960)
target_link_libraries(i960_test PRIVATE i960 ${WHITTY_MATH_LIB})
target_compile_options(i960_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME i960_test COMMAND i960_test)

add_executable(system22_audio_test
    tests/system22_audio_test.cpp
    src/namco/system22/system22_audio.cpp
)
target_include_directories(system22_audio_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(system22_audio_test PRIVATE ${OPENAL_LIBRARY} Threads::Threads)
target_link_libraries(system22_audio_test PRIVATE arcade_audio_output)
target_compile_options(system22_audio_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME system22_audio_test COMMAND system22_audio_test)

add_executable(model2_audio_test
    tests/model2_audio_test.cpp
    src/sega/model2/model2_scsp_dsp.cpp
    src/sega/model2/model2_audio.cpp
    src/musashi_memory.cpp
)
target_include_directories(model2_audio_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/musashi)
target_link_libraries(model2_audio_test PRIVATE
    musashi arcade_audio_output ${OPENAL_LIBRARY} Threads::Threads
    ${WHITTY_MATH_LIB})
target_compile_options(model2_audio_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model2_audio_test COMMAND model2_audio_test)

add_executable(system22_tms320c25_test
    tests/system22_tms320c25_test.cpp
    src/namco/system22/system22_tms320c25.cpp
)
target_include_directories(system22_tms320c25_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(system22_tms320c25_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME system22_tms320c25_test COMMAND system22_tms320c25_test)

add_executable(system22_m37710_test tests/system22_m37710_test.cpp)
target_link_libraries(system22_m37710_test PRIVATE m37710)
target_compile_options(system22_m37710_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME system22_m37710_test COMMAND system22_m37710_test)

add_executable(model1_rom_test
    tests/model1_rom_test.cpp
    src/sega/model1/model1_rom.cpp
)
target_include_directories(model1_rom_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model1_rom_test PRIVATE PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(model1_rom_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(model2_rom_test
    tests/model2_rom_test.cpp
    src/sega/model2/model2_rom.cpp
)
target_include_directories(model2_rom_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model2_rom_test PRIVATE PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(model2_rom_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(model2_vcop_rom_test
    tests/model2_vcop_rom_test.cpp
    src/sega/model2/model2_rom.cpp
)
target_include_directories(model2_vcop_rom_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model2_vcop_rom_test PRIVATE
    PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(model2_vcop_rom_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(model1io2_board_test
    tests/model1io2_board_test.cpp
    src/sega/model1/model1_io_board.cpp
    src/sega/model2/model2_rom.cpp
)
target_include_directories(model1io2_board_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(model1io2_board_test PRIVATE
    chips_z80 PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(model1io2_board_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(model2_boot_test
    tests/model2_boot_test.cpp
    src/sega/model2/model2_rom.cpp
    src/sega/model2/model2_bus.cpp
    src/sega/model2/model2_machine.cpp
    src/sega/model2/model2_geometry.cpp
    src/sega/model2/model2_video.cpp
)
target_include_directories(model2_boot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/i960
)
target_link_libraries(model2_boot_test PRIVATE
    i960 mb86233 system24_video model1_io PkgConfig::MINIZIP ZLIB::ZLIB
    ${WHITTY_MATH_LIB})
if(WIN32)
    target_link_libraries(model2_boot_test PRIVATE ws2_32)
endif()
target_compile_options(model2_boot_test PRIVATE
    -O2 -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(model2_geometry_test
    tests/model2_geometry_test.cpp
    src/sega/model2/model2_geometry.cpp
)
target_include_directories(model2_geometry_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(model2_geometry_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model2_geometry_test COMMAND model2_geometry_test)

add_executable(model2_video_test
    tests/model2_video_test.cpp
    src/sega/model2/model2_geometry.cpp
    src/sega/model2/model2_video.cpp
)
target_include_directories(model2_video_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model2_video_test PRIVATE
    system24_video ${WHITTY_MATH_LIB})
target_compile_options(model2_video_test PRIVATE
    -O2 -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model2_video_test COMMAND model2_video_test)

add_executable(model2_io_test
    tests/model2_io_test.cpp
    src/sega/model2/model2_bus.cpp
)
target_include_directories(model2_io_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model2_io_test PRIVATE model1_io)
if(WIN32)
    target_link_libraries(model2_io_test PRIVATE ws2_32)
endif()
target_compile_options(model2_io_test PRIVATE
    -O2 -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model2_io_test COMMAND model2_io_test)

add_executable(model1_boot_test
    tests/model1_boot_test.cpp
    src/sega/model1/model1_rom.cpp
    src/sega/model1/model1_machine.cpp
)
target_include_directories(model1_boot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/v60
)
target_link_libraries(model1_boot_test PRIVATE
    v60 model1_tgp mb86233 model1_video model1_io
    PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(model1_boot_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(model1_io_board_test tests/model1_io_board_test.cpp)
target_include_directories(model1_io_board_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model1_io_board_test PRIVATE model1_io)
target_compile_options(model1_io_board_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME model1_io_board_test COMMAND model1_io_board_test)

add_executable(model1_cabinet_test tests/model1_cabinet_test.cpp)
target_include_directories(model1_cabinet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(model1_cabinet_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model1_cabinet_test COMMAND model1_cabinet_test)

add_executable(model1_dsb_test tests/model1_dsb_test.cpp)
target_include_directories(model1_dsb_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model1_dsb_test PRIVATE model1_dsb)
target_compile_options(model1_dsb_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME model1_dsb_test COMMAND model1_dsb_test)

add_executable(galaxian_phoenix_machine_test
    tests/galaxian_phoenix_machine_test.cpp
    src/high_scores.cpp
    src/namco/galaxian/galaxian_machine.cpp
    src/namco/galaxian/galaxian_machine_phoenix.cpp
)
target_include_directories(galaxian_phoenix_machine_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(galaxian_phoenix_machine_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(galaxian_phoenix_machine_test PRIVATE
    chips_z80 PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(galaxian_phoenix_machine_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
set(GALAXIAN_PHOENIX_TEST_ROM "" CACHE FILEPATH
    "Optional Phoenix archive used by the Galaxian board integration test")
if(GALAXIAN_PHOENIX_TEST_ROM)
    add_test(NAME galaxian_phoenix_machine_test
        COMMAND galaxian_phoenix_machine_test "${GALAXIAN_PHOENIX_TEST_ROM}")
else()
    add_test(NAME galaxian_phoenix_machine_test
        COMMAND galaxian_phoenix_machine_test)
endif()

add_executable(galaxian_mooncrst_machine_test
    tests/galaxian_mooncrst_machine_test.cpp
    src/namco/galaxian/galaxian_audio_mooncrst.cpp
    src/high_scores.cpp
    src/namco/galaxian/galaxian_machine.cpp
    src/namco/galaxian/galaxian_machine_mooncrst.cpp
)
target_include_directories(galaxian_mooncrst_machine_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(galaxian_mooncrst_machine_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(galaxian_mooncrst_machine_test PRIVATE
    chips_z80 PkgConfig::MINIZIP ZLIB::ZLIB
)
target_compile_options(galaxian_mooncrst_machine_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
set(GALAXIAN_MOONCRST_TEST_ROM "" CACHE FILEPATH
    "Optional Moon Cresta archive used by the Galaxian board integration test")
if(GALAXIAN_MOONCRST_TEST_ROM)
    add_test(NAME galaxian_mooncrst_machine_test
        COMMAND galaxian_mooncrst_machine_test
                "${GALAXIAN_MOONCRST_TEST_ROM}")
else()
    add_test(NAME galaxian_mooncrst_machine_test
        COMMAND galaxian_mooncrst_machine_test)
endif()

add_executable(galaxian_base_machine_test
    tests/galaxian_base_machine_test.cpp
    src/namco/galaxian/galaxian_audio_mooncrst.cpp
    src/high_scores.cpp
    src/namco/galaxian/galaxian_machine.cpp
    src/namco/galaxian/galaxian_machine_mooncrst.cpp
)
target_include_directories(galaxian_base_machine_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(galaxian_base_machine_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(galaxian_base_machine_test PRIVATE
    chips_z80 PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(galaxian_base_machine_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
set(GALAXIAN_BASE_TEST_ROM "" CACHE FILEPATH
    "Optional Galaxian archive used by the base board integration test")
if(GALAXIAN_BASE_TEST_ROM)
    add_test(NAME galaxian_base_machine_test
        COMMAND galaxian_base_machine_test "${GALAXIAN_BASE_TEST_ROM}")
else()
    add_test(NAME galaxian_base_machine_test
        COMMAND galaxian_base_machine_test)
endif()

add_executable(galaxian_uniwars_machine_test
    tests/galaxian_uniwars_machine_test.cpp
    src/namco/galaxian/galaxian_audio_mooncrst.cpp
    src/high_scores.cpp
    src/namco/galaxian/galaxian_machine.cpp
    src/namco/galaxian/galaxian_machine_mooncrst.cpp
)
target_include_directories(galaxian_uniwars_machine_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(galaxian_uniwars_machine_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(galaxian_uniwars_machine_test PRIVATE
    chips_z80 PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(galaxian_uniwars_machine_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
set(GALAXIAN_UNIWARS_TEST_ROM "" CACHE FILEPATH
    "Optional UniWar S archive used by the Galaxian board integration test")
if(GALAXIAN_UNIWARS_TEST_ROM)
    add_test(NAME galaxian_uniwars_machine_test
        COMMAND galaxian_uniwars_machine_test "${GALAXIAN_UNIWARS_TEST_ROM}")
else()
    add_test(NAME galaxian_uniwars_machine_test
        COMMAND galaxian_uniwars_machine_test)
endif()

add_executable(galaxian_audio_test
    tests/galaxian_audio_test.cpp
    src/namco/galaxian/galaxian_audio.cpp
    src/namco/galaxian/galaxian_audio_phoenix.cpp
    src/namco/galaxian/galaxian_audio_mooncrst.cpp
)
target_include_directories(galaxian_audio_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(galaxian_audio_test PRIVATE
    arcade_audio_output ${OPENAL_LIBRARY} Threads::Threads)
target_compile_options(galaxian_audio_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME galaxian_audio_test COMMAND galaxian_audio_test)

add_executable(galaxian_rom_loader_test
    tests/galaxian_rom_loader_test.cpp
    src/namco/galaxian/galaxian_rom.cpp
)
target_include_directories(galaxian_rom_loader_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(galaxian_rom_loader_test PRIVATE
    PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(galaxian_rom_loader_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
if(GALAXIAN_PHOENIX_TEST_ROM AND GALAXIAN_BASE_TEST_ROM AND
   GALAXIAN_MOONCRST_TEST_ROM AND GALAXIAN_UNIWARS_TEST_ROM)
    add_test(NAME galaxian_rom_loader_test
        COMMAND galaxian_rom_loader_test
            ${GALAXIAN_PHOENIX_TEST_ROM} ${GALAXIAN_BASE_TEST_ROM}
            ${GALAXIAN_MOONCRST_TEST_ROM} ${GALAXIAN_UNIWARS_TEST_ROM})
else()
    add_test(NAME galaxian_rom_loader_test
        COMMAND galaxian_rom_loader_test)
endif()

set(MODEL1_TEST_ROM "" CACHE FILEPATH
    "Optional Virtua Formula archive used by native Model 1 integration tests")
if(MODEL1_TEST_ROM)
    add_test(NAME model1_rom_test COMMAND model1_rom_test ${MODEL1_TEST_ROM})
    add_test(NAME model1_boot_test COMMAND model1_boot_test ${MODEL1_TEST_ROM})
    add_test(NAME model1_frame_test
        COMMAND model1_boot_test ${MODEL1_TEST_ROM} 901)
    set_tests_properties(model1_frame_test PROPERTIES
        ENVIRONMENT "MODEL1_FRAMES=1")
endif()

set(MODEL2_TEST_ROM "" CACHE FILEPATH
    "Optional Sega Rally Championship archive used by Model 2 ROM tests")
if(MODEL2_TEST_ROM)
    add_test(NAME model2_rom_test
        COMMAND model2_rom_test ${MODEL2_TEST_ROM})
    add_test(NAME model2_boot_test
        COMMAND model2_boot_test ${MODEL2_TEST_ROM} 1800)
    add_test(NAME model2_race_regression_test
        COMMAND model2_boot_test ${MODEL2_TEST_ROM} 2220)
    set_tests_properties(model2_race_regression_test PROPERTIES
        ENVIRONMENT "MODEL2_AUTO_RACE=1;MODEL2_HELD_GAS_FRAME=1600")
endif()

set(MODEL2_VCOP_TEST_ROM "" CACHE FILEPATH
    "Optional Virtua Cop archive used by the Model 2 ROM loader test")
if(MODEL2_VCOP_TEST_ROM)
    add_test(NAME model2_vcop_rom_test
        COMMAND model2_vcop_rom_test ${MODEL2_VCOP_TEST_ROM})
    add_test(NAME model1io2_board_test
        COMMAND model1io2_board_test ${MODEL2_VCOP_TEST_ROM})
    add_test(NAME model2_vcop_boot_test
        COMMAND model2_boot_test ${MODEL2_VCOP_TEST_ROM} 1600)
endif()

set(MODEL2_VCOP2_TEST_ROM "" CACHE FILEPATH
    "Optional Virtua Cop 2 archive used by the Model 2 boot render test")
if(MODEL2_VCOP2_TEST_ROM)
    add_test(NAME model2_vcop2_boot_test
        COMMAND model2_boot_test ${MODEL2_VCOP2_TEST_ROM} 1600)
endif()

set(MODEL1_VF_TEST_ROM "" CACHE FILEPATH
    "Optional Virtua Fighter archive used by native Model 1 ROM tests")
if(MODEL1_VF_TEST_ROM)
    add_test(NAME model1_vf_rom_test
        COMMAND model1_rom_test ${MODEL1_VF_TEST_ROM})
endif()

set(MODEL1_SWA_TEST_ROM "" CACHE FILEPATH
    "Optional Star Wars Arcade archive used by native Model 1 ROM tests")
if(MODEL1_SWA_TEST_ROM)
    add_test(NAME model1_swa_rom_test
        COMMAND model1_rom_test ${MODEL1_SWA_TEST_ROM})
endif()

set(MODEL1_WINGWAR_TEST_ROM "" CACHE FILEPATH
    "Optional Wing War archive used by native Model 1 ROM tests")
if(MODEL1_WINGWAR_TEST_ROM)
    add_test(NAME model1_wingwar_rom_test
        COMMAND model1_rom_test ${MODEL1_WINGWAR_TEST_ROM})
endif()

# Copy ROMs and data files
if(EXISTS "${CMAKE_SOURCE_DIR}/roms")
    add_custom_target(copy_roms COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/roms $<TARGET_FILE_DIR:WhittyArcade>/roms)
endif()

# Shinobi (Sega System 16-B) host boot test. Loads a Shinobi US ROM
# image, runs N virtual frames, and dumps the rendered RGBA frame to
# /tmp/system16b_frame.rgba. Used to verify the board's renderer and
# memory map end-to-end without needing display capture.
add_executable(system16b_boot_test
    tests/system16b_boot_test.cpp
    src/high_scores.cpp
    src/sega/system16b/system16b_rom.cpp
    src/sega/system16b/system16b_machine_shinobi.cpp
)
target_include_directories(system16b_boot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(system16b_boot_test PRIVATE m68000_cpu PkgConfig::MINIZIP)
add_test(NAME system16b_input_test COMMAND system16b_boot_test --input-test)

add_executable(system16b_audio_test
    tests/system16b_audio_test.cpp
    src/sega/system16b/system16b_audio.cpp
)
target_include_directories(system16b_audio_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(system16b_audio_test PRIVATE
    arcade_audio_output ymfm segapcm OpenAL::OpenAL Threads::Threads)
add_test(NAME system16b_audio_test COMMAND system16b_audio_test)

add_executable(system16b_audio_integration_test
    tests/system16b_audio_integration_test.cpp
    src/high_scores.cpp
    src/sega/system16b/system16b_rom.cpp
    src/sega/system16b/system16b_audio.cpp
    src/sega/system16b/system16b_machine.cpp
    src/sega/system16b/system16b_machine_shinobi.cpp
)
target_include_directories(system16b_audio_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(system16b_audio_integration_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(system16b_audio_integration_test PRIVATE
    arcade_audio_output m68000_cpu chips_z80 ymfm segapcm OpenAL::OpenAL
    Threads::Threads PkgConfig::MINIZIP)

set(SHINOBI_TEST_ROM "" CACHE FILEPATH
    "Optional Shinobi archive used by the native sound integration test")
if(SHINOBI_TEST_ROM)
    add_test(NAME system16b_audio_integration_test
        COMMAND system16b_audio_integration_test ${SHINOBI_TEST_ROM} 900)
endif()

# Dual-instance 68000 core test: two m68000_cpu objects must not share any
# state (the Musashi limitation this core replaces).
add_executable(m68000_cpu_test tests/m68000_cpu_test.cpp)
target_include_directories(m68000_cpu_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(m68000_cpu_test PRIVATE m68000_cpu)
target_compile_options(m68000_cpu_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME m68000_cpu_test COMMAND m68000_cpu_test)

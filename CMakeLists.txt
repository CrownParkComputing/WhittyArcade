cmake_minimum_required(VERSION 3.18)
project(MANX LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimised build. Individual test executables pass -O2
# themselves, but the emulated-CPU libraries (chips_z80, i960, mb86233, the
# 68000 and 6809 cores) take their optimisation from the build type alone.
# With no build type they compile unoptimised, which costs little for games
# whose cores step in instruction-sized chunks but is ruinous for one that
# clocks a whole CPU per T-cycle: Virtua Cop drives the model1io2 board's Z80
# that way and drops from ~163 fps to ~20 fps.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()


if(NOT WIN32)
    set(MANX_MATH_LIB m)
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
pkg_check_modules(CURL IMPORTED_TARGET libcurl)

# FindGLEW can be backed by either CMake's module or a distribution-provided
# config package. Prefer its imported target because openSUSE's config does
# not populate the legacy GLEW_LIBRARIES variable.
if(TARGET GLEW::GLEW)
    set(MANX_GLEW_TARGET GLEW::GLEW)
elseif(TARGET GLEW::glew)
    set(MANX_GLEW_TARGET GLEW::glew)
else()
    set(MANX_GLEW_TARGET ${GLEW_LIBRARIES})
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

# Finding other machines on the LAN. Used by the launcher lobby and by both
# native cabinet-link transports, so it is a library rather than a source
# repeated in each target.
add_library(lan_peers STATIC src/lan_peers.cpp)
target_include_directories(lan_peers PUBLIC include)

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

# Intel MCS-51 microcontroller core (MAME BSD-3-Clause, standalone
# adaptation) - the i8751 protection MCU on several System 16B boards.
add_library(mcs51 STATIC third_party/mcs51/mcs51.cpp)
target_include_directories(mcs51 SYSTEM PUBLIC third_party/mcs51)
target_compile_options(mcs51 PRIVATE -w)

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

# Wall-clock-paced FMV playback (include/manx_fmv.h). A framework module,
# not a Burnout one: it decodes any container ffmpeg handles into BGRA or
# RGBA frames, paced by the stream's own per-frame timestamps because
# container frame-rate metadata cannot be trusted (Xbox XMV claims 1000 fps).
# The target only exists when the ffmpeg dev libraries are present, so a
# consumer gates its call sites on its own HAVE define and skips FMVs
# cleanly on a build without them.
pkg_check_modules(MANX_FFMPEG IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libswresample)
if(MANX_FFMPEG_FOUND)
    add_library(manx_fmv STATIC src/fmv/manx_fmv.c)
    target_include_directories(manx_fmv PUBLIC ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(manx_fmv PUBLIC PkgConfig::MANX_FFMPEG)
endif()

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

add_library(arcade_feedback STATIC src/arcade_feedback.cpp)
target_include_directories(arcade_feedback PUBLIC
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(arcade_feedback PRIVATE
    -Wall -Wextra -Wpedantic)

add_library(arcade_audio_output STATIC src/arcade_audio_output.cpp)
target_include_directories(arcade_audio_output PUBLIC
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(arcade_audio_output PUBLIC arcade_feedback)
target_compile_options(arcade_audio_output PRIVATE
    -Wall -Wextra -Wpedantic)

# Source files
set(SOURCES
    src/main.cpp
    src/play_stats.cpp
    src/manx_http.cpp
    src/manx_stun.cpp
    src/online_link.cpp
    src/banner_library.cpp
    src/system16_data.cpp
    src/bezel_artwork.cpp
    src/bezel_library.cpp
    src/stb_image_impl.cpp
    src/arcade_session.cpp
    src/namco/system22/system22_session.cpp
    src/sega/model1/model1_session.cpp
    src/sega/model2/model2_session.cpp
    src/namco/galaxian/galaxian_session.cpp
    src/sega/system16b/system16b_session.cpp
    src/taito/taitoz/taitoz_session.cpp
    src/taito/taitoz/taitoz_rom.cpp
    src/taito/taitoz/taitoz_machine.cpp
    src/taito/taitoz/taitoz_video.cpp
    src/taito/taitoz/taitoz_audio.cpp
    src/capcom/gng/gng_session.cpp
    src/game_plugin_host.cpp
    src/plugin_session.cpp
    src/plugin_audio.cpp
    src/namco/namco_session.cpp
    src/arcade_renderer.cpp
    src/arcade_input.cpp
    src/network_video_link.cpp
    src/multiplayer_lobby.cpp
    src/manx_log_tap.cpp
    src/input_mapping.cpp
    src/input_mapper.cpp
    src/media_library.cpp
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
    src/namco/system22/system22_c139_transport.cpp
    src/namco/system22/system22_dsp.cpp
    src/namco/system22/system22_mcu.cpp
    src/namco/system22/system22_sprites.cpp
    src/namco/system22/system22_tms320c25.cpp
    src/namco/system22/system22_rom.cpp
    src/system246_rom.cpp
    src/system246_controls.cpp
    src/sega/model1/model1_rom.cpp
    src/sega/model1/model1_machine.cpp
    src/sega/model1/model1_audio.cpp
    src/sega/model2/model2_rom.cpp
    src/sega/model2/model2_scsp_dsp.cpp
    src/sega/model2/model2_audio.cpp
    src/sega/model2/model2_bus.cpp
    src/sega/model2/model2_machine.cpp
    src/sega/model2/model2_geometry.cpp
    src/sega/model2/model2_draw_list.cpp
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
    src/midway/midway_rom.cpp
    src/midway/midway_chd.cpp
    src/midway/mips_cpu.cpp
    src/midway/wolfunit_session.cpp
    src/namco/galaga/galaga_machine.cpp
    src/namco/galaga/galaga_audio.cpp
    src/namco/system1/system1_machine.cpp
    src/namco/system1/pacmania_feedback.cpp
    src/namco/system1/system1_audio.cpp
    src/namco/system1/system1_ym2151.cpp
)

if(WIN32)
    enable_language(RC)
    list(APPEND SOURCES
        src/platform_file_dialog_windows.cpp
        resources/windows/MANX.rc
    )
else()
    list(APPEND SOURCES src/platform_file_dialog_unix.cpp)
endif()

# Vulkan shaders. Each GLSL stage is compiled to SPIR-V and emitted as a C
# header holding a uint32_t array, so the binary carries its own shaders and
# there is no runtime file lookup or GLSL compiler dependency. Vulkan::glslangValidator
# ships with the Vulkan SDK; glslc (shaderc) is accepted as an alternative.
# glslangValidator's --vn emits a named uint32_t array, which is what the
# sources #include. shaderc's glslc has no equivalent, so it is not offered as
# a fallback: a build that silently produced headerless SPIR-V would fail at
# the first #include instead of here.
if(TARGET Vulkan::glslangValidator)
    set(MANX_GLSL_COMPILER $<TARGET_FILE:Vulkan::glslangValidator>)
else()
    find_program(MANX_GLSLANG_EXECUTABLE glslangValidator)
    if(NOT MANX_GLSLANG_EXECUTABLE)
        message(FATAL_ERROR
            "glslangValidator not found. Install glslang to build the Vulkan "
            "shaders.")
    endif()
    set(MANX_GLSL_COMPILER ${MANX_GLSLANG_EXECUTABLE})
endif()

set(MANX_SHADER_HEADER_DIR ${CMAKE_BINARY_DIR}/generated/shaders)
file(MAKE_DIRECTORY ${MANX_SHADER_HEADER_DIR})
set(MANX_SHADER_HEADERS "")
foreach(shader
        arcade_post.vert arcade_post.frag
        arcade_overlay.vert arcade_overlay.frag
        arcade_solid.frag
        system22_polygon.vert system22_polygon.frag
        fullscreen_quad.vert system22_text.frag
        model2_polygon.vert model2_polygon.frag model2_overlay.frag)
    string(REPLACE "." "_" shader_symbol ${shader})
    set(shader_source ${CMAKE_SOURCE_DIR}/shaders/${shader})
    set(shader_header ${MANX_SHADER_HEADER_DIR}/${shader_symbol}.h)
    set(shader_command ${MANX_GLSL_COMPILER}
        --target-env vulkan1.0 --vn ${shader_symbol}_spirv
        -o ${shader_header} ${shader_source})
    add_custom_command(
        OUTPUT ${shader_header}
        COMMAND ${shader_command}
        DEPENDS ${shader_source}
        COMMENT "Compiling ${shader} to SPIR-V"
        VERBATIM)
    list(APPEND MANX_SHADER_HEADERS ${shader_header})
endforeach()
add_custom_target(manx_shaders DEPENDS ${MANX_SHADER_HEADERS})

# Build executable
if(WIN32)
    add_executable(MANX WIN32 ${SOURCES})
else()
    add_executable(MANX ${SOURCES})
endif()
target_link_libraries(MANX PRIVATE lan_peers)
set_target_properties(MANX PROPERTIES OUTPUT_NAME MANX)
add_dependencies(MANX manx_shaders)
target_include_directories(MANX PRIVATE ${MANX_SHADER_HEADER_DIR})
target_include_directories(MANX PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/json
    ${CMAKE_SOURCE_DIR}/third_party/moira/Moira)

# Link libraries
target_link_libraries(MANX
    PRIVATE
    ${OPENGL_LIBRARIES}
    SDL3::SDL3
    ${OPENAL_LIBRARY}
    ${MANX_GLEW_TARGET}
    mcs51
    musashi
    m37710
    v60
    i960
    model1_tgp
    mb86233
    model1_video
    model1_io
    # model1_audio calls into the DSB directly, so the binary needs it even
    # though no other library pulls it in.
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
    ${MANX_MATH_LIB}
    ${CMAKE_DL_LIBS}
)
# Cabinet bezels are fetched over HTTP when libcurl is available; without it
# the bezel library reports itself unconfigured and no bezel ever appears.
if(TARGET PkgConfig::CURL)
    target_link_libraries(MANX PRIVATE PkgConfig::CURL)
    target_compile_definitions(MANX PRIVATE MANX_HAVE_CURL)
endif()
# Cover-flow video playback (manx_fmv). Only present when ffmpeg dev
# libraries were found; without them the cover-flow view still works with
# still artwork alone.
if(TARGET manx_fmv)
    target_link_libraries(MANX PRIVATE manx_fmv)
    target_compile_definitions(MANX PRIVATE MANX_HAVE_FFMPEG)
endif()

if(WIN32)
    target_link_libraries(MANX PRIVATE ole32 shell32 uuid ws2_32)
endif()

# Compiler flags
target_compile_options(MANX PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-O2;-Wall;-Wextra;-Wpedantic>"
)

enable_testing()
add_test(NAME video_session_lifecycle_test
    COMMAND MANX --video-lifecycle-test)
set_tests_properties(video_session_lifecycle_test PROPERTIES
    ENVIRONMENT "DISPLAY=;WAYLAND_DISPLAY=;MANX_HEADLESS=1")

add_test(NAME arcade_session_factory_test
    COMMAND MANX --session-factory-test)
# Constructs and destroys every board in turn, which reaches the video layer.
# On a build machine there is no display to reach, so it runs headless like
# video_session_lifecycle_test above rather than failing for want of a screen.
set_tests_properties(arcade_session_factory_test PROPERTIES
    ENVIRONMENT "DISPLAY=;WAYLAND_DISPLAY=;MANX_HEADLESS=1")
add_test(NAME c139_link_test
    COMMAND MANX --c139-link-test)
set_tests_properties(c139_link_test PROPERTIES TIMEOUT 60)

add_executable(arcade_audio_output_test tests/arcade_audio_output_test.cpp)
target_link_libraries(arcade_audio_output_test PRIVATE arcade_audio_output)
target_compile_options(arcade_audio_output_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME arcade_audio_output_test COMMAND arcade_audio_output_test)

add_executable(audio_impact_detector_test tests/audio_impact_detector_test.cpp)
target_include_directories(audio_impact_detector_test PRIVATE include)
target_compile_options(audio_impact_detector_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME audio_impact_detector_test COMMAND audio_impact_detector_test)

add_executable(arcade_feedback_test tests/arcade_feedback_test.cpp)
target_link_libraries(arcade_feedback_test PRIVATE arcade_feedback)
target_compile_options(arcade_feedback_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME arcade_feedback_test COMMAND arcade_feedback_test)

add_executable(pacmania_feedback_test
    tests/pacmania_feedback_test.cpp
    src/namco/system1/pacmania_feedback.cpp)
target_include_directories(pacmania_feedback_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(pacmania_feedback_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME pacmania_feedback_test COMMAND pacmania_feedback_test)

add_executable(arcade_settings_test
    tests/arcade_settings_test.cpp
    src/arcade_settings.cpp
)
target_include_directories(arcade_settings_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(arcade_settings_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME arcade_settings_test COMMAND arcade_settings_test)

add_executable(media_library_test
    tests/media_library_test.cpp
    src/media_library.cpp
)
target_include_directories(media_library_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests)
target_compile_options(media_library_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME media_library_test COMMAND media_library_test)

# Renders the Vulkan presentation shaders into an offscreen image and checks
# the pixels. Needs a Vulkan device; it skips cleanly when there is none.
add_executable(vulkan_post_pipeline_test tests/vulkan_post_pipeline_test.cpp)
add_dependencies(vulkan_post_pipeline_test manx_shaders)
target_include_directories(vulkan_post_pipeline_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${MANX_SHADER_HEADER_DIR})
target_link_libraries(vulkan_post_pipeline_test PRIVATE Vulkan::Vulkan)
target_compile_options(vulkan_post_pipeline_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vulkan_post_pipeline_test COMMAND vulkan_post_pipeline_test)

# Drives the real presenter through a System 22 frame under the Vulkan
# validation layers. Opens a window, so it is not wired into ctest; run it
# directly with MANX_PRESENTER_HARNESS=1.
# Two cabinets, without the cabinets. Deliberately NOT an add_test: it signs
# in to the live Firebase project, so it needs the internet, an account and a
# slice of the daily quota - none of which ctest may ever depend on.
if(TARGET PkgConfig::CURL)
    add_executable(online_live_harness
        tests/online_live_harness.cpp
        src/online_link.cpp
        src/manx_http.cpp
        src/lan_peers.cpp
    )
    target_include_directories(online_live_harness PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/json)
    target_link_libraries(online_live_harness PRIVATE PkgConfig::CURL)
    target_compile_definitions(online_live_harness PRIVATE MANX_HAVE_CURL)
    # Sockets are a library on Windows rather than part of libc, and this
    # links the lobby's own networking. MANX itself has always said so; the
    # harnesses had not, so a build where the program linked perfectly well
    # still failed.
    if(WIN32)
        target_link_libraries(online_live_harness PRIVATE ws2_32)
    endif()
endif()

add_executable(vulkan_presenter_harness
    tests/vulkan_presenter_harness.cpp
    src/arcade_presenter.cpp
    src/arcade_settings.cpp
    src/namco/system22/system22_rom.cpp
    src/sega/model2/model2_rom.cpp
    src/sega/model2/model2_bus.cpp
    src/sega/model2/model2_machine.cpp
    src/sega/model2/model2_geometry.cpp
    src/sega/model2/model2_draw_list.cpp
    src/sega/model2/model2_video.cpp
)
target_link_libraries(vulkan_presenter_harness PRIVATE lan_peers)
if(WIN32)
    target_link_libraries(vulkan_presenter_harness PRIVATE ws2_32)
endif()

add_dependencies(vulkan_presenter_harness manx_shaders)
target_include_directories(vulkan_presenter_harness PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/i960
    ${MANX_SHADER_HEADER_DIR})
target_link_libraries(vulkan_presenter_harness PRIVATE SDL3::SDL3
    Vulkan::Vulkan i960 mb86233 system24_video model1_io
    PkgConfig::MINIZIP ZLIB::ZLIB ${MANX_MATH_LIB})
target_compile_options(vulkan_presenter_harness PRIVATE -Wall -Wextra)

add_executable(wall_workspace_test
    tests/wall_workspace_test.cpp
)
target_include_directories(wall_workspace_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(wall_workspace_test PRIVATE SDL3::SDL3)
target_compile_options(wall_workspace_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME wall_workspace_test COMMAND wall_workspace_test)

add_executable(netplay_lockstep_test
    tests/netplay_lockstep_test.cpp
)
target_compile_options(netplay_lockstep_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME netplay_lockstep_test COMMAND netplay_lockstep_test)

# Deliberately does not link libcurl: manx_stun opens no socket and resolves
# no name, which is what lets the whole codec be tested from a byte buffer.
add_executable(manx_stun_test
    tests/manx_stun_test.cpp
    src/manx_stun.cpp
)
target_include_directories(manx_stun_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(manx_stun_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME manx_stun_test COMMAND manx_stun_test)

add_executable(lobby_targets_test
    tests/lobby_targets_test.cpp
)
target_include_directories(lobby_targets_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(lobby_targets_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME lobby_targets_test COMMAND lobby_targets_test)

# Header-only under test, so this needs neither libcurl nor a network: canned
# Firestore JSON in, decisions out.
add_executable(online_wire_test
    tests/online_wire_test.cpp
)
target_include_directories(online_wire_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/json)
target_compile_options(online_wire_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME online_wire_test COMMAND online_wire_test)

add_executable(menu_stick_test
    tests/menu_stick_test.cpp
)
target_include_directories(menu_stick_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(menu_stick_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME menu_stick_test COMMAND menu_stick_test)

add_executable(wall_capacity_test
    tests/wall_capacity_test.cpp
)
target_include_directories(wall_capacity_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(wall_capacity_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME wall_capacity_test COMMAND wall_capacity_test)

add_executable(wall_layout_test
    tests/wall_layout_test.cpp
)
target_include_directories(wall_layout_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(wall_layout_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME wall_layout_test COMMAND wall_layout_test)

add_executable(bezel_artwork_test
    tests/bezel_artwork_test.cpp
    src/bezel_artwork.cpp
)
target_include_directories(bezel_artwork_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(bezel_artwork_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME bezel_artwork_test COMMAND bezel_artwork_test)

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

add_executable(session_window_test tests/session_window_test.cpp)
target_include_directories(session_window_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME session_window_test COMMAND session_window_test)

add_executable(bubblebobble_test
    tests/bubblebobble_test.cpp
    src/bubblebobble/bb_game.cpp)
target_include_directories(bubblebobble_test PRIVATE include)
add_test(NAME bubblebobble_test COMMAND bubblebobble_test)

add_executable(jetpac_test
    tests/jetpac_test.cpp
    src/jetpac/jp_game.cpp)
target_include_directories(jetpac_test PRIVATE include)
add_test(NAME jetpac_test COMMAND jetpac_test)

add_executable(plugin_audio_test
    tests/plugin_audio_test.cpp
    src/plugin_audio.cpp
    src/arcade_audio_output.cpp)
target_include_directories(plugin_audio_test PRIVATE include)
target_link_libraries(plugin_audio_test PRIVATE OpenAL::OpenAL arcade_feedback)
add_test(NAME plugin_audio_test COMMAND plugin_audio_test)

add_executable(game_plugin_test
    tests/game_plugin_test.cpp
    src/game_plugin_host.cpp
    src/arcade_catalog.cpp
    src/arcade_game_probe.cpp
    src/midway/midway_rom.cpp
    src/taito/taitoz/taitoz_rom.cpp
    src/namco/system22/system22_rom.cpp
    src/system246_rom.cpp
    src/sega/model1/model1_rom.cpp
    src/sega/model2/model2_rom.cpp
    src/namco/galaxian/galaxian_rom.cpp
    src/sega/system16b/system16b_rom.cpp
    src/capcom/gng/gng_rom.cpp
    src/namco/namco_rom.cpp
)
target_include_directories(game_plugin_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
# The test builds its own plugins, so it needs to tell the compiler it invokes
# where the ABI header lives.
target_compile_definitions(game_plugin_test PRIVATE
    INCLUDE_DIR="${CMAKE_SOURCE_DIR}/include")
target_link_libraries(game_plugin_test PRIVATE
    PkgConfig::MINIZIP ZLIB::ZLIB ${CMAKE_DL_LIBS})
target_compile_options(game_plugin_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME game_plugin_test COMMAND game_plugin_test)

add_executable(arcade_catalog_test
    tests/arcade_catalog_test.cpp
    src/arcade_catalog.cpp
    src/midway/midway_rom.cpp
    src/arcade_game_probe.cpp
    src/taito/taitoz/taitoz_rom.cpp
    src/namco/system22/system22_rom.cpp
    src/system246_rom.cpp
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
    src/midway/midway_rom.cpp
    src/arcade_settings.cpp
    src/arcade_game_probe.cpp
    src/taito/taitoz/taitoz_rom.cpp
    src/rom_library.cpp
    src/namco/system22/system22_rom.cpp
    src/system246_rom.cpp
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
set(NAMCO_GALAGA88_TEST_ROM "" CACHE FILEPATH
    "Optional galaga88.zip used by the Namco ROM integration test")
if(NAMCO_GALAGA_TEST_ROM AND NAMCO_PACMANIA_TEST_ROM)
    add_test(NAME namco_rom_test COMMAND namco_rom_test
        "${NAMCO_GALAGA_TEST_ROM}" "${NAMCO_PACMANIA_TEST_ROM}"
        "${NAMCO_GALAGA88_TEST_ROM}")
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
    musashi Threads::Threads ${MANX_MATH_LIB})
target_compile_options(system22_cpu_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME system22_cpu_test COMMAND system22_cpu_test)

add_executable(c139_transport_test
    tests/c139_transport_test.cpp
    src/musashi_memory.cpp
    src/namco/system22/system22_cpu.cpp
    src/namco/system22/system22_c139_transport.cpp
)
target_link_libraries(c139_transport_test PRIVATE lan_peers)

target_include_directories(c139_transport_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(c139_transport_test PRIVATE
    musashi Threads::Threads ${MANX_MATH_LIB})
# The cabinet link is a UDP transport, and Windows keeps sockets in a library
# of their own - the same line the two Model 2 tests already carry.
if(WIN32)
    target_link_libraries(c139_transport_test PRIVATE ws2_32)
endif()
target_compile_options(c139_transport_test PRIVATE
    -Wall -Wextra -Wpedantic)
add_test(NAME c139_transport_test COMMAND c139_transport_test)
# Loopback sockets must be unique per test run, and the loopback
# exchange loops several times — give it time.
set_tests_properties(c139_transport_test PROPERTIES TIMEOUT 60)

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
target_link_libraries(i960_test PRIVATE i960 ${MANX_MATH_LIB})
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
    ${MANX_MATH_LIB})
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
target_link_libraries(model2_boot_test PRIVATE lan_peers)

target_include_directories(model2_boot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/i960
)
target_link_libraries(model2_boot_test PRIVATE
    i960 mb86233 system24_video model1_io PkgConfig::MINIZIP ZLIB::ZLIB
    ${MANX_MATH_LIB})
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
    system24_video ${MANX_MATH_LIB})
target_compile_options(model2_video_test PRIVATE
    -O2 -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model2_video_test COMMAND model2_video_test)

add_executable(model2_io_test
    tests/model2_io_test.cpp
    src/sega/model2/model2_bus.cpp
)
target_link_libraries(model2_io_test PRIVATE lan_peers)

target_include_directories(model2_io_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/mb86233)
target_link_libraries(model2_io_test PRIVATE model1_io mb86233 ZLIB::ZLIB)
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
    chips_z80 arcade_feedback PkgConfig::MINIZIP ZLIB::ZLIB
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

add_executable(galaxian_determinism_test
    tests/galaxian_determinism_test.cpp
    src/high_scores.cpp
    src/namco/galaxian/galaxian_machine.cpp
    src/namco/galaxian/galaxian_machine_mooncrst.cpp
)
target_include_directories(galaxian_determinism_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(galaxian_determinism_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(galaxian_determinism_test PRIVATE
    chips_z80 PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(galaxian_determinism_test PRIVATE -Wall -Wextra)
# Registered so it actually runs. This is the test the whole netplay design
# rests on - lockstep is only possible because the board is deterministic - and
# it was built but never invoked by ctest, so nothing would have reported the
# day that stopped being true. It needs a ROM, and prints usage and exits 0
# without one, so registering it unconditionally would be a test that passes by
# doing nothing.
if(GALAXIAN_BASE_TEST_ROM)
    add_test(NAME galaxian_determinism_test
        COMMAND galaxian_determinism_test ${GALAXIAN_BASE_TEST_ROM})
    set_tests_properties(galaxian_determinism_test PROPERTIES LABELS netplay)
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
    chips_z80 arcade_feedback PkgConfig::MINIZIP ZLIB::ZLIB)
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
    chips_z80 arcade_feedback PkgConfig::MINIZIP ZLIB::ZLIB)
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
        ${CMAKE_SOURCE_DIR}/roms $<TARGET_FILE_DIR:MANX>/roms)
endif()

# Shinobi (Sega System 16-B) host boot test. Loads a Shinobi US ROM
# image, runs N virtual frames, and dumps the rendered RGBA frame to
# /tmp/system16b_frame.rgba. Used to verify the board's renderer and
# memory map end-to-end without needing display capture.
add_executable(mcs51_core_test tests/mcs51_core_test.cpp)
target_link_libraries(mcs51_core_test PRIVATE mcs51)
add_test(NAME mcs51_core_test COMMAND mcs51_core_test)

add_executable(system16b_boot_test
    tests/system16b_boot_test.cpp
    src/high_scores.cpp
    src/sega/system16b/system16b_rom.cpp
    src/sega/system16b/system16b_machine_shinobi.cpp
    src/sega/system16b/system16b_audio.cpp
)
target_include_directories(system16b_boot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(system16b_boot_test PRIVATE m68000_cpu chips_z80
    mcs51 arcade_audio_output ymfm segapcm OpenAL::OpenAL Threads::Threads
    PkgConfig::MINIZIP)
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
    arcade_audio_output m68000_cpu chips_z80 mcs51 ymfm segapcm
    OpenAL::OpenAL Threads::Threads PkgConfig::MINIZIP)

set(SHINOBI_TEST_ROM "" CACHE FILEPATH
    "Optional Shinobi archive used by the native sound integration test")
if(SHINOBI_TEST_ROM)
    add_test(NAME system16b_audio_integration_test
        COMMAND system16b_audio_integration_test ${SHINOBI_TEST_ROM} 900)
endif()

# Bay Route (set 1 US, unprotected) ROM-backed boot regression. The loader
# and the 171-5358 board config are exercised end-to-end against a real
# archive; like SHINOBI_TEST_ROM this never ships in-tree.
set(BAYROUTE_TEST_ROM "" CACHE FILEPATH
    "Optional Bay Route (set 1 US) archive for the ROM-backed boot test")
if(BAYROUTE_TEST_ROM)
    add_test(NAME system16b_bayroute_boot_test
        COMMAND system16b_boot_test ${BAYROUTE_TEST_ROM} 60)
endif()

# Taito Z System ROM loader test. Without arguments it checks only the
# metadata contract so CI can run it; with a legal archive (and optionally a
# directory of MAME region dumps) it validates every assembled region.
add_executable(taitoz_rom_test
    tests/taitoz_rom_test.cpp
    src/taito/taitoz/taitoz_rom.cpp
)
target_include_directories(taitoz_rom_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(taitoz_rom_test PRIVATE PkgConfig::MINIZIP)
set(TAITOZ_TEST_ROM "" CACHE FILEPATH
    "Optional Continental Circus archive for the Taito Z ROM test")
set(TAITOZ_MAME_REGIONS "" CACHE PATH
    "Optional directory of MAME region dumps to byte-compare the loader against")
if(TAITOZ_TEST_ROM)
    add_test(NAME taitoz_rom_test COMMAND taitoz_rom_test
        "${TAITOZ_TEST_ROM}" "${TAITOZ_MAME_REGIONS}")
else()
    add_test(NAME taitoz_rom_test COMMAND taitoz_rom_test)
endif()

# Taito Z boot/frame test: dual-68000 execution plus the full video
# composite against a real archive.
add_executable(taitoz_boot_test
    tests/taitoz_boot_test.cpp
    src/taito/taitoz/taitoz_rom.cpp
    src/taito/taitoz/taitoz_machine.cpp
    src/taito/taitoz/taitoz_video.cpp
    src/taito/taitoz/taitoz_audio.cpp
)
target_include_directories(taitoz_boot_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(taitoz_boot_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(taitoz_boot_test PRIVATE m68000_cpu chips_z80 ymfm
    arcade_audio_output OpenAL::OpenAL Threads::Threads PkgConfig::MINIZIP)
if(TAITOZ_TEST_ROM)
    add_test(NAME taitoz_boot_test
        COMMAND taitoz_boot_test "${TAITOZ_TEST_ROM}" 60)
endif()

# ROM board 171-5797 contract test: the mapper region layout, the
# 315-5248 multiplier, both 315-5250 compare/timers and the tile bank
# register E-SWAT introduced. Needs no ROMs, so it runs in CI.
add_executable(system16b_5797_test
    tests/system16b_5797_test.cpp
    src/high_scores.cpp
    src/sega/system16b/system16b_rom.cpp
    src/sega/system16b/system16b_machine_shinobi.cpp
)
target_include_directories(system16b_5797_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(system16b_5797_test PRIVATE mcs51 Threads::Threads
    PkgConfig::MINIZIP)
add_test(NAME system16b_5797_test COMMAND system16b_5797_test)

# E-SWAT (pre-decrypted `eswatd` clone) ROM-backed boot regression, which
# is the only way to exercise the 5797 mapper decode against real code.
set(ESWAT_TEST_ROM "" CACHE FILEPATH
    "Optional E-SWAT (eswatd) archive for the ROM-backed boot test")
if(ESWAT_TEST_ROM)
    add_test(NAME system16b_eswat_boot_test
        COMMAND system16b_boot_test ${ESWAT_TEST_ROM} 60)
endif()

# Wonder Boy III: Monster Lair (pre-decrypted `wb33d`) boot regression.
set(WB3_TEST_ROM "" CACHE FILEPATH
    "Optional Wonder Boy III (wb33d) archive for the ROM-backed boot test")
if(WB3_TEST_ROM)
    add_test(NAME system16b_wb3_boot_test
        COMMAND system16b_boot_test ${WB3_TEST_ROM} 60)
endif()

# Dual-instance 68000 core test: two m68000_cpu objects must not share any
# state (the Musashi limitation this core replaces).
add_executable(m68000_cpu_test tests/m68000_cpu_test.cpp)
target_include_directories(m68000_cpu_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(m68000_cpu_test PRIVATE m68000_cpu)
target_compile_options(m68000_cpu_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME m68000_cpu_test COMMAND m68000_cpu_test)

# ===========================================================================
# System 246/256 board bring-up: embedded PCSX2 core link test (FOUNDATION).
#
# Standalone smoke-test executable that proves MANX can link against
# the prebuilt PCSX2 arcade-fork static libraries and stand the PCSX2 CPU
# thread up and down headlessly (no game boot, no GS device, no UI/audio).
#
# It is compiled and linked directly with clang++ in C++20 -- matching how the
# PCSX2 libraries themselves were built -- so its ABI matches libpcsx2.a even
# though the main MANX tree builds with the default (GCC) compiler.
# The include dirs, compile definitions and flags are copied verbatim from how
# PCSX2 compiles its own core (pcsx2/VMManager.cpp) so the arcade-fork headers
# parse identically here.
#
# Kept entirely separate from the main MANX binary and NOT part of the
# default (ALL) build, so a PCSX2 link failure cannot break the rest of the
# tree. Build explicitly with:
#     cmake --build build --target system246_pcsx2_link_test
#
# PCSX2 has no ARM backend, so the target only exists on x86-64.
# ===========================================================================
set(MANX_PCSX2_SRC_ROOT "/home/jon/pcsx2x6" CACHE PATH
    "PCSX2 arcade-fork source tree used by the System 246 link test")
set(MANX_PCSX2_BUILD_ROOT "/home/jon/pcsx2x6/build" CACHE PATH
    "PCSX2 arcade-fork build tree containing the prebuilt static libraries")

# Set TRUE once the real PCSX2-backed System 246 session (the dlopen client that
# drives libsystem246_pcsx2.so) is compiled into MANX and the .so is
# built; a stub make_system246_session is added otherwise.
set(MANX_SYSTEM246_PCSX2_LINKED FALSE)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    find_program(MANX_PCSX2_CLANGXX NAMES clang++ clang++-22 clang++-20)
    if(NOT MANX_PCSX2_CLANGXX)
        message(STATUS
            "system246_pcsx2_link_test: clang++ not found; PCSX2 link test disabled")
    elseif(NOT EXISTS "${MANX_PCSX2_BUILD_ROOT}/pcsx2/libpcsx2.a")
        message(STATUS
            "system246_pcsx2_link_test: ${MANX_PCSX2_BUILD_ROOT}/pcsx2/libpcsx2.a "
            "not found; PCSX2 link test disabled")
    else()
        set(_p2s "${MANX_PCSX2_SRC_ROOT}")
        set(_p2b "${MANX_PCSX2_BUILD_ROOT}")

        set(MANX_PCSX2_SOURCES
            "${CMAKE_SOURCE_DIR}/src/system246/pcsx2_host.cpp"
            "${CMAKE_SOURCE_DIR}/src/system246/pcsx2_link_test.cpp")

        # Compile flags copied from PCSX2's own core build (minus the PCH, which
        # we reproduce by #include "pcsx2/PrecompiledHeader.h" in the sources).
        set(MANX_PCSX2_FLAGS
            -DFMT_USE_EXCEPTIONS=0 -DFMT_USE_RTTI=0
            -O3 -DNDEBUG -std=gnu++20 -fPIC -march=native -pipe
            -fvisibility=hidden -pthread -fno-exceptions -g1
            -Wno-unused-function -Wno-unused-parameter -Wno-missing-field-initializers
            -ffp-contract=fast -fno-strict-aliasing -Wno-parentheses
            -Wno-missing-braces -Wno-unknown-pragmas)

        # Compile definitions copied verbatim from the PCSX2 core build.
        set(MANX_PCSX2_DEFINES
            -DCPUINFO_SUPPORTED_PLATFORM=1 -DENABLE_OPENGL -DENABLE_VULKAN
            -DHAVE_DECL_BASENAME=1 -DHAVE_LIMITS_H=1 -DHAVE_STDLIB_H=1
            -DHAVE_STRING_H=1 -DPLUTOSVG_HAS_FREETYPE
            -DRYML_DEFAULT_CALLBACK_USES_EXCEPTIONS -DSOUNDTOUCH_FLOAT_SAMPLES
            -DST_NO_EXCEPTION_HANDLING -DWAYLAND_API -DX11_API
            -DXBYAK_NO_EXCEPTION -DZYCORE_STATIC_BUILD -DZYDIS_STATIC_BUILD
            -D_M_X86=1 -D__POSIX__)

        # Include dirs copied verbatim from the PCSX2 core build.
        set(MANX_PCSX2_INCLUDES
            -I${_p2s}/3rdparty/xbyak
            -I${_p2s}/pcsx2
            -I${_p2b}/pcsx2
            -I${_p2b}/common/include
            -I${_p2s}/3rdparty/glad/include
            -I${_p2s}/3rdparty/vulkan/include
            -I${_p2s}/3rdparty/zydis/dependencies/zycore/include
            -I${_p2s}/3rdparty/zydis/include
            -I${_p2s}/3rdparty/include
            -I${_p2s}
            -I${_p2s}/3rdparty/fast_float/include
            -I${_p2s}/3rdparty/imgui/include
            -I${_p2s}/3rdparty/libchdr/include
            -I${_p2s}/3rdparty/rcheevos/include
            -I${_p2s}/3rdparty/discord-rpc/include
            -I${_p2s}/3rdparty/rapidjson/include
            -I${_p2s}/3rdparty/simpleini/include
            -I${_p2s}/3rdparty/freesurround/include
            -I${_p2s}/3rdparty/soundtouch/soundtouch
            -I${_p2s}/3rdparty/lzma/include
            -I${_p2s}/3rdparty/demangler/include
            -I${_p2s}/3rdparty/ccc/src
            -I${_p2s}/3rdparty/fmt/include
            -I${_p2s}/3rdparty/libzip/lib
            -I${_p2b}/3rdparty/libzip
            -I${_p2s}/3rdparty/cpuinfo/include
            -I${_p2s}/3rdparty/cubeb/include
            -isystem /usr/include/freetype2
            -isystem /usr/include/plutosvg
            -isystem /usr/include/plutovg)

        # Prebuilt PCSX2 static libraries. Wrapped in --start-group/--end-group
        # to resolve their circular dependencies.
        set(MANX_PCSX2_STATIC_LIBS
            ${_p2b}/pcsx2/libpcsx2.a
            ${_p2b}/common/libcommon.a
            ${_p2b}/3rdparty/ccc/libccc.a
            ${_p2b}/3rdparty/cpuinfo/libcpuinfo.a
            ${_p2b}/3rdparty/cubeb/libcubeb.a
            ${_p2b}/3rdparty/demangler/libdemanglegnu.a
            ${_p2b}/3rdparty/discord-rpc/libdiscord-rpc.a
            ${_p2b}/3rdparty/fmt/libfmt.a
            ${_p2b}/3rdparty/freesurround/libfreesurround.a
            ${_p2b}/3rdparty/glad/libglad.a
            ${_p2b}/3rdparty/imgui/libimgui.a
            ${_p2b}/3rdparty/libchdr/liblibchdr.a
            ${_p2b}/3rdparty/libzip/libzip.a
            ${_p2b}/3rdparty/lzma/libpcsx2-lzma.a
            ${_p2b}/3rdparty/rcheevos/librcheevos.a
            ${_p2b}/3rdparty/simpleini/libsimpleini.a
            ${_p2b}/3rdparty/soundtouch/libpcsx2-soundtouch.a
            ${_p2b}/3rdparty/zydis/libzydis.a)

        # System libraries PCSX2 depends on (matches the fork's gsrunner link).
        set(MANX_PCSX2_SYSTEM_LIBS
            -ldl -lbacktrace -lcurl -ldbus-1 -lfontconfig -lfreetype -lGLX
            -ljpeg -llz4 -lOpenGL -lpcap -lpng -ludev -lwebp -lX11 -lXext -lXi
            -lxkbcommon -lXrandr -lXrender -lz -lzstd -lpthread -lSDL3
            -lryml -lplutosvg -lplutovg)

        # -------------------------------------------------------------------
        # Isolated PCSX2 module: libsystem246_pcsx2.so (dlopen'd at runtime).
        #
        # The PCSX2 arcade core bundles its own glad GL loader plus a raft of
        # static initialisers that collide with MANX's GLEW-based
        # OpenGL renderer when both live in one binary (this hung
        # polygon_renderer_gpu::initialize at startup for EVERY game). So NO
        # PCSX2 symbol is linked into MANX. Instead the code is split:
        #
        #   * system246_pcsx2_session.cpp is a plain GCC/C++17 dlopen client
        #     compiled straight into MANX. It includes no PCSX2 header
        #     -- only <dlfcn.h> and the flat C ABI in pcsx2_module.h.
        #   * pcsx2_module.cpp + pcsx2_host.cpp (the only files that touch
        #     PCSX2) are compiled and linked with clang++ (matching the ABI
        #     libpcsx2.a was built with) into libsystem246_pcsx2.so, next to
        #     the MANX binary. MANX dlopen()s it at run time and
        #     drives Ridge Racer V purely through the four wa_pcsx2_* exports.
        #
        # Because MANX only sees the C ABI, it reverts cleanly to GCC
        # and carries zero PCSX2 / glad symbols.
        target_sources(MANX PRIVATE
            "${CMAKE_SOURCE_DIR}/src/system246/system246_pcsx2_session.cpp"
            "${CMAKE_SOURCE_DIR}/src/system246/system246_loading_screen.cpp")

        set(MANX_PCSX2_MODULE_SOURCES
            "${CMAKE_SOURCE_DIR}/src/system246/pcsx2_module.cpp"
            "${CMAKE_SOURCE_DIR}/src/system246/pcsx2_host.cpp")

        set(MANX_PCSX2_MODULE_SO
            "${CMAKE_BINARY_DIR}/libsystem246_pcsx2.so")

        # Single-shot clang++ compile+link into the shared object (same recipe
        # as the link-test/boot harnesses, plus -shared). Every PCSX2 static
        # lib is position-independent (POSITION_INDEPENDENT_CODE=ON in the
        # arcade build), so they link cleanly into a .so. Our own module sources
        # additionally need the MANX headers.
        add_custom_command(
            OUTPUT "${MANX_PCSX2_MODULE_SO}"
            COMMAND "${MANX_PCSX2_CLANGXX}"
                -shared -fPIC
                ${MANX_PCSX2_FLAGS}
                ${MANX_PCSX2_DEFINES}
                ${MANX_PCSX2_INCLUDES}
                -I${CMAKE_SOURCE_DIR}/include
                -I${CMAKE_SOURCE_DIR}/src
                -I${CMAKE_SOURCE_DIR}/src/system246
                ${MANX_PCSX2_MODULE_SOURCES}
                -o "${MANX_PCSX2_MODULE_SO}"
                -Wl,--start-group ${MANX_PCSX2_STATIC_LIBS} -Wl,--end-group
                ${MANX_PCSX2_SYSTEM_LIBS}
            DEPENDS ${MANX_PCSX2_MODULE_SOURCES}
                "${CMAKE_SOURCE_DIR}/include/audio_impact_detector.h"
                "${CMAKE_SOURCE_DIR}/src/system246/pcsx2_module.h"
                "${CMAKE_SOURCE_DIR}/src/system246/pcsx2_host.h"
                "${CMAKE_SOURCE_DIR}/src/system246/pcsx2_input.h"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Linking libsystem246_pcsx2.so (isolated PCSX2 module) with clang++"
            VERBATIM)

        # Part of the default (ALL) build; MANX dlopen()s it, so also
        # order it before the binary is considered done.
        add_custom_target(system246_pcsx2_module ALL
            DEPENDS "${MANX_PCSX2_MODULE_SO}")
        add_dependencies(MANX system246_pcsx2_module)

        set(MANX_SYSTEM246_PCSX2_LINKED TRUE)

        set(MANX_PCSX2_LINK_TEST_BIN
            "${CMAKE_BINARY_DIR}/system246_pcsx2_link_test")

        add_custom_command(
            OUTPUT "${MANX_PCSX2_LINK_TEST_BIN}"
            COMMAND "${MANX_PCSX2_CLANGXX}"
                ${MANX_PCSX2_FLAGS}
                ${MANX_PCSX2_DEFINES}
                ${MANX_PCSX2_INCLUDES}
                ${MANX_PCSX2_SOURCES}
                -o "${MANX_PCSX2_LINK_TEST_BIN}"
                -Wl,--start-group ${MANX_PCSX2_STATIC_LIBS} -Wl,--end-group
                ${MANX_PCSX2_SYSTEM_LIBS}
            DEPENDS ${MANX_PCSX2_SOURCES}
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Linking system246_pcsx2_link_test against prebuilt PCSX2 static libraries"
            VERBATIM)


        # -------------------------------------------------------------------
        # RRV boot harness: same clang++/ABI recipe as the link test above,
        # plus SDL3, that boots Ridge Racer V Arcade Battle in-process into a
        # real Vulkan window with cubeb audio. Compiles pcsx2_boot_rrv.cpp +
        # pcsx2_host.cpp and reuses the same flags/defines/includes/static-lib
        # group; SDL3 libs are already in MANX_PCSX2_SYSTEM_LIBS (-lSDL3).
        #
        # Like the link test it is x86-64 only and kept OUT of the default
        # (ALL) build. Build explicitly with:
        #     cmake --build build --target system246_pcsx2_boot_rrv
        # -------------------------------------------------------------------
        set(MANX_PCSX2_BOOT_RRV_SOURCES
            "${CMAKE_SOURCE_DIR}/src/system246/pcsx2_host.cpp"
            "${CMAKE_SOURCE_DIR}/src/system246/pcsx2_boot_rrv.cpp")

        # SDL3 compile flags (headers). The link libs come from -lSDL3 already
        # present in MANX_PCSX2_SYSTEM_LIBS.
        execute_process(
            COMMAND pkg-config --cflags sdl3
            OUTPUT_VARIABLE MANX_SDL3_CFLAGS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE MANX_SDL3_PKGCONFIG_RESULT)
        if(NOT MANX_SDL3_PKGCONFIG_RESULT EQUAL 0)
            set(MANX_SDL3_CFLAGS "")
        endif()
        separate_arguments(MANX_SDL3_CFLAGS_LIST NATIVE_COMMAND "${MANX_SDL3_CFLAGS}")

        set(MANX_PCSX2_BOOT_RRV_BIN
            "${CMAKE_BINARY_DIR}/system246_pcsx2_boot_rrv")

        add_custom_command(
            OUTPUT "${MANX_PCSX2_BOOT_RRV_BIN}"
            COMMAND "${MANX_PCSX2_CLANGXX}"
                ${MANX_PCSX2_FLAGS}
                ${MANX_PCSX2_DEFINES}
                ${MANX_PCSX2_INCLUDES}
                ${MANX_SDL3_CFLAGS_LIST}
                ${MANX_PCSX2_BOOT_RRV_SOURCES}
                -o "${MANX_PCSX2_BOOT_RRV_BIN}"
                -Wl,--start-group ${MANX_PCSX2_STATIC_LIBS} -Wl,--end-group
                ${MANX_PCSX2_SYSTEM_LIBS}
            DEPENDS ${MANX_PCSX2_BOOT_RRV_SOURCES}
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Linking system246_pcsx2_boot_rrv (RRV in-process boot) against prebuilt PCSX2 static libraries + SDL3"
            VERBATIM)

    endif()
endif()

# On any target where the embedded PCSX2 core was not linked (non-x86-64, no
# clang, or the prebuilt libpcsx2 is absent), provide a stub
# make_system246_session so the board stays wired into the catalog/factory and
# MANX still links.
if(NOT MANX_SYSTEM246_PCSX2_LINKED)
    target_sources(MANX PRIVATE
        ${CMAKE_SOURCE_DIR}/src/system246/system246_pcsx2_session_stub.cpp)
    # The factory test asks every board to build a session. Without the core
    # this one cannot, and the test has to know the difference between a board
    # this build left out and a board that is broken.
    target_compile_definitions(MANX PRIVATE
        MANX_SYSTEM246_STUBBED=1)
endif()

add_executable(title_frame_dump
    tests/title_frame_dump.cpp
    src/namco/system1/system1_ym2151.cpp
    src/namco/galaxian/galaxian_machine.cpp
    src/namco/galaxian/galaxian_machine_mooncrst.cpp
    src/namco/galaxian/galaxian_machine_phoenix.cpp
    src/namco/galaga/galaga_machine.cpp
    src/namco/system1/system1_machine.cpp
    src/namco/system1/pacmania_feedback.cpp
    src/namco/namco_rom.cpp
    src/high_scores.cpp)
target_include_directories(title_frame_dump PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(title_frame_dump SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/chips
    ${CMAKE_SOURCE_DIR}/third_party/namcos1
    ${CMAKE_SOURCE_DIR}/third_party/ymfm/src)
target_link_libraries(title_frame_dump PRIVATE
    chips_z80 m6809f namcos1_core ymfm PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(title_frame_dump PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(namco_system1_start_test
    tests/namco_system1_start_test.cpp
    src/namco/system1/system1_ym2151.cpp
    src/namco/system1/system1_machine.cpp
    src/namco/system1/pacmania_feedback.cpp
    src/namco/namco_rom.cpp)
target_include_directories(namco_system1_start_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(namco_system1_start_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/namcos1
    ${CMAKE_SOURCE_DIR}/third_party/ymfm/src)
target_link_libraries(namco_system1_start_test PRIVATE
    m6809f namcos1_core ymfm Threads::Threads PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(namco_system1_start_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)

# Asserts must survive in every test, including Release.
#
# The default Release flags carry -DNDEBUG, which turns every assert() in a test
# into nothing at all. A test built that way still compiles, still runs, still
# prints "all checks passed" and still reports success to ctest - while checking
# absolutely nothing. Twenty-six targets were in that state when this was found,
# among them netplay_lockstep_test and galaxian_determinism_test: the two tests
# the entire multiplayer design rests on.
#
# Individual targets were passing -UNDEBUG by hand, which works right up until
# someone adds a test and does not know to. Applying it to every registered test
# here makes that impossible to get wrong, and is deliberately placed at the end
# so it covers targets added anywhere above.
# Every *_test TARGET, not every registered test: some are built but never
# registered with add_test (galaxian_determinism_test and system16b_boot_test
# are, today), and others register only when an optional ROM is present. Those
# are exactly the ones nobody is watching, so they are the last ones that should
# quietly lose their asserts.
get_property(_manx_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _manx_targets)
    if(_target MATCHES "_test$")
        # Custom and utility targets carry the same suffix but cannot take
        # compile options, so ask what this actually is before setting any.
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "EXECUTABLE" OR _type STREQUAL "STATIC_LIBRARY" OR
           _type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "OBJECT_LIBRARY")
            target_compile_options(${_target} PRIVATE -UNDEBUG)
        endif()
    endif()
endforeach()

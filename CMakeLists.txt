cmake_minimum_required(VERSION 3.16)
project(WhittyArcade LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required libraries
find_package(OpenGL REQUIRED)
find_package(SDL2 REQUIRED)
find_package(OpenAL REQUIRED)
find_package(GLEW REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET minizip)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
pkg_check_modules(MPG123 REQUIRED IMPORTED_TARGET libmpg123)

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

add_library(model1_io STATIC src/model1_io_board.cpp)
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

add_library(model1_dsb STATIC src/model1_dsb.cpp)
target_include_directories(model1_dsb PUBLIC
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(model1_dsb SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(model1_dsb PUBLIC chips_z80 PkgConfig::MPG123)
target_compile_options(model1_dsb PRIVATE -Wall -Wextra -Wpedantic)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${SDL2_INCLUDE_DIRS})
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
    src/system22_session.cpp
    src/model1_session.cpp
    src/model2_session.cpp
    src/galaxian_session.cpp
    src/shinobi_session.cpp
    src/arcade_renderer.cpp
    src/arcade_input.cpp
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
    src/system22_audio.cpp
    src/musashi_memory.cpp
    src/system22_cpu.cpp
    src/system22_dsp.cpp
    src/system22_mcu.cpp
    src/system22_sprites.cpp
    src/system22_tms320c25.cpp
    src/system22_rom.cpp
    src/model1_rom.cpp
    src/model1_machine.cpp
    src/model1_audio.cpp
    src/model2_rom.cpp
    src/model2_scsp_dsp.cpp
    src/model2_audio.cpp
    src/model2_bus.cpp
    src/model2_machine.cpp
    src/model2_geometry.cpp
    src/model2_video.cpp
    src/galaxian_rom.cpp
    src/galaxian_audio.cpp
    src/galaxian_audio_mooncrst.cpp
    src/galaxian_audio_phoenix.cpp
    src/galaxian_machine.cpp
    src/galaxian_machine_mooncrst.cpp
    src/galaxian_machine_phoenix.cpp
    src/shinobi_rom.cpp
    src/shinobi_audio.cpp
    src/shinobi_machine.cpp
    src/shinobi_machine_shinobi.cpp
)


# Build executable
add_executable(WhittyArcade ${SOURCES})
target_include_directories(WhittyArcade PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/moira/Moira)

# Link libraries
target_link_libraries(WhittyArcade
    ${OPENGL_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${OPENAL_LIBRARY}
    ${GLEW_LIBRARIES}
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
    Threads::Threads
    PkgConfig::MINIZIP
    PkgConfig::SDL2_TTF
    ZLIB::ZLIB
    Vulkan::Vulkan
    m
)

# Compiler flags
target_compile_options(WhittyArcade PRIVATE
    -O2
    -Wall -Wextra -Wpedantic
)

enable_testing()
add_test(NAME video_session_lifecycle_test
    COMMAND WhittyArcade --video-lifecycle-test)
set_tests_properties(video_session_lifecycle_test PROPERTIES
    ENVIRONMENT "DISPLAY=;WAYLAND_DISPLAY=")

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
target_link_libraries(input_mapping_test PRIVATE ${SDL2_LIBRARIES})
target_compile_options(input_mapping_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME input_mapping_test COMMAND input_mapping_test)

add_executable(arcade_catalog_test
    tests/arcade_catalog_test.cpp
    src/arcade_catalog.cpp
    src/arcade_game_probe.cpp
    src/system22_rom.cpp
    src/model1_rom.cpp
    src/model2_rom.cpp
    src/galaxian_rom.cpp
    src/shinobi_rom.cpp
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
    src/system22_rom.cpp
    src/model1_rom.cpp
    src/model2_rom.cpp
    src/galaxian_rom.cpp
    src/shinobi_rom.cpp
)
target_include_directories(rom_library_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(rom_library_test PRIVATE PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(rom_library_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME rom_library_test COMMAND rom_library_test)

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
    src/system22_cpu.cpp
)
target_include_directories(system22_cpu_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(system22_cpu_test PRIVATE musashi Threads::Threads m)
target_compile_options(system22_cpu_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME system22_cpu_test COMMAND system22_cpu_test)

add_executable(system22_sprite_test
    tests/system22_sprite_test.cpp
    src/system22_sprites.cpp)
target_include_directories(system22_sprite_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(system22_sprite_test PRIVATE
    -Wall -Wextra -Wpedantic)
add_test(NAME system22_sprite_test COMMAND system22_sprite_test)

add_executable(system22_rom_test
    tests/system22_rom_test.cpp
    src/system22_rom.cpp
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
target_link_libraries(i960_test PRIVATE i960 m)
target_compile_options(i960_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME i960_test COMMAND i960_test)

add_executable(system22_audio_test
    tests/system22_audio_test.cpp
    src/system22_audio.cpp
)
target_include_directories(system22_audio_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(system22_audio_test PRIVATE ${OPENAL_LIBRARY} Threads::Threads)
target_link_libraries(system22_audio_test PRIVATE arcade_audio_output)
target_compile_options(system22_audio_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME system22_audio_test COMMAND system22_audio_test)

add_executable(model2_audio_test
    tests/model2_audio_test.cpp
    src/model2_scsp_dsp.cpp
    src/model2_audio.cpp
    src/musashi_memory.cpp
)
target_include_directories(model2_audio_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/musashi)
target_link_libraries(model2_audio_test PRIVATE
    musashi arcade_audio_output ${OPENAL_LIBRARY} Threads::Threads m)
target_compile_options(model2_audio_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model2_audio_test COMMAND model2_audio_test)

add_executable(system22_tms320c25_test
    tests/system22_tms320c25_test.cpp
    src/system22_tms320c25.cpp
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
    src/model1_rom.cpp
)
target_include_directories(model1_rom_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model1_rom_test PRIVATE PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(model1_rom_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(model2_rom_test
    tests/model2_rom_test.cpp
    src/model2_rom.cpp
)
target_include_directories(model2_rom_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model2_rom_test PRIVATE PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(model2_rom_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(model2_boot_test
    tests/model2_boot_test.cpp
    src/model2_rom.cpp
    src/model2_bus.cpp
    src/model2_machine.cpp
    src/model2_geometry.cpp
    src/model2_video.cpp
)
target_include_directories(model2_boot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/i960
)
target_link_libraries(model2_boot_test PRIVATE
    i960 mb86233 system24_video PkgConfig::MINIZIP ZLIB::ZLIB m)
target_compile_options(model2_boot_test PRIVATE
    -O2 -Wall -Wextra -Wpedantic -UNDEBUG)

add_executable(model2_geometry_test
    tests/model2_geometry_test.cpp
    src/model2_geometry.cpp
)
target_include_directories(model2_geometry_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(model2_geometry_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model2_geometry_test COMMAND model2_geometry_test)

add_executable(model2_video_test
    tests/model2_video_test.cpp
    src/model2_geometry.cpp
    src/model2_video.cpp
)
target_include_directories(model2_video_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(model2_video_test PRIVATE system24_video m)
target_compile_options(model2_video_test PRIVATE
    -O2 -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model2_video_test COMMAND model2_video_test)

add_executable(model2_io_test
    tests/model2_io_test.cpp
    src/model2_bus.cpp
)
target_include_directories(model2_io_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(model2_io_test PRIVATE
    -O2 -Wall -Wextra -Wpedantic -UNDEBUG)
add_test(NAME model2_io_test COMMAND model2_io_test)

add_executable(model1_boot_test
    tests/model1_boot_test.cpp
    src/model1_rom.cpp
    src/model1_machine.cpp
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
    src/galaxian_machine.cpp
    src/galaxian_machine_phoenix.cpp
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
    src/galaxian_audio_mooncrst.cpp
    src/high_scores.cpp
    src/galaxian_machine.cpp
    src/galaxian_machine_mooncrst.cpp
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

add_executable(galaxian_audio_test
    tests/galaxian_audio_test.cpp
    src/galaxian_audio.cpp
    src/galaxian_audio_phoenix.cpp
    src/galaxian_audio_mooncrst.cpp
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
    src/galaxian_rom.cpp
)
target_include_directories(galaxian_rom_loader_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(galaxian_rom_loader_test PRIVATE
    PkgConfig::MINIZIP ZLIB::ZLIB)
target_compile_options(galaxian_rom_loader_test PRIVATE
    -Wall -Wextra -Wpedantic -UNDEBUG)
if(GALAXIAN_PHOENIX_TEST_ROM AND GALAXIAN_MOONCRST_TEST_ROM)
    add_test(NAME galaxian_rom_loader_test
        COMMAND galaxian_rom_loader_test
            ${GALAXIAN_PHOENIX_TEST_ROM} ${GALAXIAN_MOONCRST_TEST_ROM})
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
# /tmp/shinobi_frame.rgba. Used to verify the board's renderer and
# memory map end-to-end without needing display capture.
add_executable(shinobi_boot_test
    tests/shinobi_boot_test.cpp
    src/high_scores.cpp
    src/shinobi_rom.cpp
    src/shinobi_machine_shinobi.cpp
)
target_include_directories(shinobi_boot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(shinobi_boot_test PRIVATE m68000_cpu PkgConfig::MINIZIP)
add_test(NAME shinobi_input_test COMMAND shinobi_boot_test --input-test)

add_executable(shinobi_audio_test
    tests/shinobi_audio_test.cpp
    src/shinobi_audio.cpp
)
target_include_directories(shinobi_audio_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(shinobi_audio_test PRIVATE
    arcade_audio_output ymfm segapcm OpenAL::OpenAL Threads::Threads)
add_test(NAME shinobi_audio_test COMMAND shinobi_audio_test)

add_executable(shinobi_audio_integration_test
    tests/shinobi_audio_integration_test.cpp
    src/high_scores.cpp
    src/shinobi_rom.cpp
    src/shinobi_audio.cpp
    src/shinobi_machine.cpp
    src/shinobi_machine_shinobi.cpp
)
target_include_directories(shinobi_audio_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(shinobi_audio_integration_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/chips)
target_link_libraries(shinobi_audio_integration_test PRIVATE
    arcade_audio_output m68000_cpu chips_z80 ymfm segapcm OpenAL::OpenAL
    Threads::Threads PkgConfig::MINIZIP)

set(SHINOBI_TEST_ROM "" CACHE FILEPATH
    "Optional Shinobi archive used by the native sound integration test")
if(SHINOBI_TEST_ROM)
    add_test(NAME shinobi_audio_integration_test
        COMMAND shinobi_audio_integration_test ${SHINOBI_TEST_ROM} 900)
endif()

# Dual-instance 68000 core test: two m68000_cpu objects must not share any
# state (the Musashi limitation this core replaces).
add_executable(m68000_cpu_test tests/m68000_cpu_test.cpp)
target_include_directories(m68000_cpu_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(m68000_cpu_test PRIVATE m68000_cpu)
target_compile_options(m68000_cpu_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME m68000_cpu_test COMMAND m68000_cpu_test)

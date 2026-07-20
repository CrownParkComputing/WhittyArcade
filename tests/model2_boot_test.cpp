#include "model2_machine.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <vector>

namespace {

bool write_capture(const model2_machine& machine,
                   const std::filesystem::path& path) {
    std::FILE* output = std::fopen(path.c_str(), "wb");
    if (!output) return false;

    std::fprintf(output, "P6\n%d %d\n255\n",
                 model2_machine::screen_width,
                 model2_machine::screen_height);
    const uint8_t* pixels = reinterpret_cast<const uint8_t*>(
        machine.frame_buffer());
    for (std::size_t offset = 0;
         offset < static_cast<std::size_t>(
                      model2_machine::screen_width *
                      model2_machine::screen_height * 4);
         offset += 4)
        std::fwrite(pixels + offset, 1, 3, output);
    std::fclose(output);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s srallyc.zip [frames]\n", argv[0]);
        return 2;
    }

    const int frames = argc >= 3 ? std::atoi(argv[2]) : 1;
    const bool geometry_only = std::getenv("MODEL2_GEOMETRY_ONLY") != nullptr;
    const char* held_gas_text = std::getenv("MODEL2_HELD_GAS_FRAME");
    const int held_gas_frame = held_gas_text ?
        std::atoi(held_gas_text) : 1600;
    const char* capture_dir_text = std::getenv("MODEL2_CAPTURE_DIR");
    const char* capture_start_text = std::getenv("MODEL2_CAPTURE_START");
    const char* capture_interval_text =
        std::getenv("MODEL2_CAPTURE_INTERVAL");
    const int capture_start = capture_start_text ?
        std::atoi(capture_start_text) : 1;
    const int capture_interval = std::max(1, capture_interval_text ?
        std::atoi(capture_interval_text) : 1);
    if (capture_dir_text)
        std::filesystem::create_directories(capture_dir_text);
    // Boot from factory defaults, never the user's persistent NVRAM. A test
    // may opt into an isolated prepared image for restart/persistence traces.
    if (const char* config = std::getenv("MODEL2_TEST_CONFIG_HOME"))
        setenv("XDG_CONFIG_HOME", config, 1);
    else
        setenv("XDG_CONFIG_HOME", "/nonexistent/whitty-boot-test", 1);
    model2_machine machine;
    if (!machine.initialize(argv[1])) return 1;
    std::vector<uint8_t> midi;
    machine.set_sound_uart_callback(
        [&midi](uint8_t data) { midi.push_back(data); });

    const uint32_t reset_pc = machine.program_counter();
    assert(reset_pc != 0);
    assert(reset_pc != 0xffffffffU);

    for (int frame = 0; frame < frames && !machine.cpu_faulted(); ++frame) {
        if (std::getenv("MODEL2_OPEN_GAME_ASSIGNMENTS")) {
            input_state input;
            input.test = true;
            input.service = std::getenv("MODEL2_CONFIRM_SELECTION") &&
                frame >= 680 && frame < 684;
            const char* pulse_text = std::getenv("MODEL2_SERVICE_PULSES");
            const int pulse_count = pulse_text ? std::atoi(pulse_text) : 6;
            for (int selection = 0; selection < pulse_count; ++selection) {
                const int press = 560 + selection * 18;
                input.test |= frame >= press && frame < press + 4;
            }
            machine.set_inputs(input);
        } else if (std::getenv("MODEL2_HOLD_TEST")) {
            input_state input;
            input.test = true;
            machine.set_inputs(input);
        } else if (std::getenv("MODEL2_AUTO_RACE")) {
            input_state input;
            input.coin1 = frame == 180 || frame == 181 ||
                          frame == 210 || frame == 211;
            input.start = frame >= 240 && frame < 244;
            // Pulse the accelerator through car, course and transmission
            // screens. Their confirmation logic is edge-sensitive, just
            // like the physical pedal switch near its pressed threshold.
            if (frame >= held_gas_frame ||
                (frame >= 300 && ((frame - 300) % 120) < 12))
                input.gas = 0x610;
            machine.set_inputs(input);
        } else if (std::getenv("MODEL2_AUTO_INPUT")) {
            input_state input;
            input.coin1 = frame == 180 || frame == 181;
            input.start = frame >= 240 && frame < 244;
            machine.set_inputs(input);
        }
        const int frame_number = frame + 1;
        const bool capture_this_frame = capture_dir_text &&
            frame_number >= capture_start &&
            ((frame_number - capture_start) % capture_interval) == 0;
        machine.run_frame(!geometry_only &&
                          (frame_number == frames || capture_this_frame));
        if (const char* trace_start_text =
                std::getenv("MODEL2_TRACE_RIVALS")) {
            const int trace_start = std::atoi(trace_start_text);
            if (frame_number >= trace_start) {
                const auto trace_u32 = [&machine](uint32_t address) {
                    uint32_t value = 0;
                    for (unsigned byte = 0; byte < 4; ++byte)
                        value |= static_cast<uint32_t>(
                            machine.debug_read8(address + byte)) <<
                            (byte * 8);
                    return value;
                };
                std::printf(
                    "RIVAL frame=%d scene=%08x "
                    "one=%08x/%u/%08x/%08x "
                    "two=%08x/%u/%08x/%08x "
                    "pos1=%08x/%08x/%08x/%08x "
                    "pos2=%08x/%08x/%08x/%08x\n",
                    frame_number, trace_u32(0x002020a0),
                    trace_u32(0x00500110), trace_u32(0x005001e8),
                    trace_u32(0x00213be8), trace_u32(0x002139fc),
                    trace_u32(0x00500210), trace_u32(0x005002e8),
                    trace_u32(0x00213c40), trace_u32(0x00213a0c),
                    trace_u32(0x00500114), trace_u32(0x00500118),
                    trace_u32(0x00500190), trace_u32(0x00500194),
                    trace_u32(0x00500214), trace_u32(0x00500218),
                    trace_u32(0x00500290), trace_u32(0x00500294));
            }
        }
        if (capture_this_frame) {
            char filename[32];
            std::snprintf(filename, sizeof(filename), "frame-%04d.ppm",
                          frame_number);
            assert(write_capture(machine,
                std::filesystem::path(capture_dir_text) / filename));
            const model2_geometry_summary& captured =
                machine.geometry_summary();
            std::printf("Captured frame %d: polygons=%u direct=%u\n",
                        frame_number, captured.decoded_polygons,
                        captured.direct_polygons);
        }
        if (std::getenv("MODEL2_TRACE_INPUT_RAM") && frame >= 176 &&
            frame <= 248) {
            const auto read32 = [&machine](uint32_t address) {
                uint32_t value = 0;
                for (unsigned byte = 0; byte < 4; ++byte)
                    value |= static_cast<uint32_t>(
                        machine.debug_read8(address + byte)) << (byte * 8);
                return value;
            };
            std::printf("frame %d input=%08x prev=%08x edge=%08x release=%08x\n",
                        frame, read32(0x0020205c), read32(0x00202060),
                        read32(0x00202064), read32(0x00202068));
        }
    }
    if (const char* range = std::getenv("MODEL2_DISASSEMBLE_RANGE")) {
        unsigned start = 0;
        unsigned end = 0;
        if (std::sscanf(range, "%x:%x", &start, &end) == 2) {
            for (uint32_t address = start; address < end; address += 4)
                std::printf("%08x %s\n", address,
                            machine.instruction_at(address).c_str());
        }
    }
    if (std::getenv("MODEL2_SCAN_INPUT_REFS")) {
        constexpr std::array<uint32_t, 4> targets{
            0x0020205c, 0x00202060, 0x00202064, 0x00202068};
        for (uint32_t address = 0x00500000; address < 0x00600000;
             ++address) {
            uint32_t value = 0;
            for (unsigned byte = 0; byte < 4; ++byte)
                value |= static_cast<uint32_t>(
                    machine.debug_read8(address + byte)) << (byte * 8);
            for (uint32_t target : targets) {
                if (value == target)
                    std::printf("input ref %08x -> %08x\n", address,
                                target);
            }
        }
    }
    if (std::getenv("MODEL2_TRACE_INPUT_RAM")) {
        for (uint32_t address = 0x0020205c; address < 0x0020206c;
             address += 4) {
            uint32_t value = 0;
            for (unsigned byte = 0; byte < 4; ++byte)
                value |= static_cast<uint32_t>(
                    machine.debug_read8(address + byte)) << (byte * 8);
            std::printf("input ram %08x = %08x\n", address, value);
        }
    }

    if (const char* start_text = std::getenv("MODEL2_DISASM_START")) {
        const uint32_t start = static_cast<uint32_t>(
            std::strtoul(start_text, nullptr, 0));
        const char* end_text = std::getenv("MODEL2_DISASM_END");
        const uint32_t end = end_text ? static_cast<uint32_t>(
            std::strtoul(end_text, nullptr, 0)) : start + 4;
        for (uint32_t address = start; address < end; address += 4)
            std::printf("%08x %s\n", address,
                        machine.instruction_at(address).c_str());
    }

    if (argc >= 4) {
        std::array<uint32_t, 128> history{};
        std::size_t position = 0;
        for (int step = 0; step < 1000000; ++step) {
            history[position++ % history.size()] = machine.program_counter();
            machine.run_cycles(1);
            if (machine.program_counter() == 0x005a3d58) break;
        }
        std::puts("Trace before firmware stop:");
        const std::size_t start = position > history.size() ?
            position - history.size() : 0;
        for (std::size_t index = start; index < position; ++index) {
            const uint32_t address = history[index % history.size()];
            std::printf("  %08x %s\n", address,
                        machine.instruction_at(address).c_str());
        }
    }

    std::printf("Model 2 boot: reset=%08x pc=%08x cycles=%llu "
                "unmapped=%llu/%llu last=%08x/%08x%s%s\n",
                reset_pc, machine.program_counter(),
                static_cast<unsigned long long>(machine.executed_cycles()),
                static_cast<unsigned long long>(machine.unmapped_reads()),
                static_cast<unsigned long long>(machine.unmapped_writes()),
                machine.last_unmapped_read(), machine.last_unmapped_write(),
                machine.cpu_faulted() ? " fault=\"" : "",
                machine.cpu_faulted() ? machine.cpu_error().c_str() : "");
    if (machine.cpu_faulted()) std::puts("\"");
    std::printf("Sound UART: bytes=%zu", midi.size());
    for (uint8_t byte : midi) std::printf(" %02x", byte);
    std::putchar('\n');
    const auto debug_u32 = [&machine](uint32_t address) {
        uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            value |= static_cast<uint32_t>(machine.debug_read8(address + byte))
                     << (byte * 8);
        return value;
    };
    const uint32_t sound_queue_count = debug_u32(0x0020b180);
    const uint32_t sound_queue_write = debug_u32(0x0020b184);
    const uint32_t sound_queue_busy = debug_u32(0x0020b188);
    std::printf("Sound queue: count=%u write=%u busy=%u uart=%02x irq=%08x/%08x\n",
                sound_queue_count, sound_queue_write, sound_queue_busy,
                machine.debug_read8(0x01c80002),
                debug_u32(0x00e80000), debug_u32(0x00e80004));
    std::printf("TGP: running=%s pc=%04x upload=%u fifo=%zu/%zu\n",
                machine.tgp_running() ? "yes" : "no",
                machine.tgp_program_counter(), machine.tgp_uploaded_words(),
                machine.tgp_input_words(), machine.tgp_output_words());
    std::printf("Geometry: read=%05x write=%05x\n",
                machine.geometry_read_address(),
                machine.geometry_write_address());
    uint32_t geometry_address = machine.geometry_read_address() & 0x1ffff;
    for (unsigned row = 0; row < 4; ++row) {
        std::printf("  %05x:", geometry_address);
        for (unsigned column = 0; column < 4; ++column) {
            std::printf(" %08x",
                        machine.geometry_buffer_word(geometry_address));
            geometry_address = (geometry_address + 4) & 0x1ffff;
        }
        std::putchar('\n');
    }
    const model2_geometry_summary& geometry = machine.geometry_summary();
    std::printf("  decoded=%u words=%u direct=%u polygons=%u "
                "object-words=%u zero=%u "
                "modes=%u/%u/%u/%u sources=ram/rom/fast:%u/%u/%u end=%s "
                "truncated=%s commands:",
                geometry.commands, geometry.words_consumed,
                geometry.direct_polygons, geometry.decoded_polygons,
                geometry.object_words,
                geometry.zero_length_objects,
                geometry.object_mode_counts[0], geometry.object_mode_counts[1],
                geometry.object_mode_counts[2], geometry.object_mode_counts[3],
                geometry.object_source_counts[0],
                geometry.object_source_counts[1],
                geometry.object_source_counts[2],
                geometry.ended ? "yes" : "no",
                geometry.truncated ? "yes" : "no");
    for (unsigned command = 0; command < geometry.command_counts.size();
         ++command) {
        if (geometry.command_counts[command])
            std::printf(" %02x=%u", command,
                        geometry.command_counts[command]);
    }
    std::putchar('\n');
    std::printf("  renderers=%u/%u/%u/%u links=%u/%u/%u/%u "
                "zsort=%u/%u/%u/%u checker=%u backface=%u culled=%u\n",
                geometry.renderer_counts[0], geometry.renderer_counts[1],
                geometry.renderer_counts[2], geometry.renderer_counts[3],
                geometry.link_type_counts[0], geometry.link_type_counts[1],
                geometry.link_type_counts[2], geometry.link_type_counts[3],
                geometry.z_sort_mode_counts[0], geometry.z_sort_mode_counts[1],
                geometry.z_sort_mode_counts[2], geometry.z_sort_mode_counts[3],
                geometry.checker_polygons, geometry.backface_polygons,
                geometry.culled_polygons);
    if (std::getenv("MODEL2_DUMP_OBJECTS")) {
        struct object_trace {
            uint32_t address{};
            uint16_t window{};
            uint32_t polygons{};
            uint32_t drawable{};
            uint16_t minimum_sort{0xffff};
            uint16_t maximum_sort{};
            float minimum_z{std::numeric_limits<float>::infinity()};
            float maximum_z{-std::numeric_limits<float>::infinity()};
            float minimum_x{std::numeric_limits<float>::infinity()};
            float maximum_x{-std::numeric_limits<float>::infinity()};
            float minimum_y{std::numeric_limits<float>::infinity()};
            float maximum_y{-std::numeric_limits<float>::infinity()};
        };
        std::map<uint16_t, object_trace> objects;
        for (const model2_geometry_polygon& polygon :
             machine.geometry_polygons()) {
            object_trace& object = objects[polygon.object_instance];
            object.address = polygon.object_address;
            object.window = polygon.window;
            ++object.polygons;
            object.minimum_sort = std::min(object.minimum_sort,
                                           polygon.z_value);
            object.maximum_sort = std::max(object.maximum_sort,
                                           polygon.z_value);
            if (polygon.vertex_count >= 3 && polygon.renderer != 1 &&
                ((polygon.attributes >> 8) & 3) != 0 &&
                (((polygon.attributes >> 17) & 1) != 0 ||
                 !polygon.backface))
                ++object.drawable;
            for (unsigned vertex = 0; vertex < polygon.vertex_count;
                 ++vertex) {
                const model2_geometry_vertex& point =
                    polygon.vertices[vertex];
                if (!std::isfinite(point.z) || point.z <= 0.0001f)
                    continue;
                object.minimum_z = std::min(object.minimum_z, point.z);
                object.maximum_z = std::max(object.maximum_z, point.z);
                const float x = polygon.center[0] + point.x / point.z;
                const float y = 384.0f - polygon.center[1] -
                                point.y / point.z;
                object.minimum_x = std::min(object.minimum_x, x);
                object.maximum_x = std::max(object.maximum_x, x);
                object.minimum_y = std::min(object.minimum_y, y);
                object.maximum_y = std::max(object.maximum_y, y);
            }
        }
        for (const auto& [instance, object] : objects) {
            std::printf("  object=%03u address=%08x window=%u "
                        "polys=%u/%u sort=%04x..%04x z=%.2f..%.2f "
                        "xy=%.1f..%.1f/%.1f..%.1f\n",
                        instance, object.address, object.window,
                        object.drawable, object.polygons,
                        object.minimum_sort, object.maximum_sort,
                        object.minimum_z, object.maximum_z,
                        object.minimum_x, object.maximum_x,
                        object.minimum_y, object.maximum_y);
        }
    }
    if (const char* reference_path =
            std::getenv("MODEL2_REFERENCE_BUFFER")) {
        std::vector<uint8_t> reference_buffer(0x20000);
        std::FILE* reference = std::fopen(reference_path, "rb");
        assert(reference != nullptr);
        assert(std::fread(reference_buffer.data(), 1,
                          reference_buffer.size(), reference) ==
               reference_buffer.size());
        std::fclose(reference);
        const char* start_text = std::getenv("MODEL2_REFERENCE_START");
        const uint32_t reference_start = start_text ?
            static_cast<uint32_t>(std::strtoul(start_text, nullptr, 0)) : 0;
        model2_geometry reference_geometry;
        reference_geometry.reset();
        reference_geometry.parse(reference_buffer, reference_start,
                                 machine.roms().polygon_data,
                                 machine.roms().texture_data);
        const model2_geometry_summary& reference_summary =
            reference_geometry.summary();
        std::printf("Reference geometry: start=%05x commands=%u "
                    "polygons=%u ended=%s truncated=%s\n",
                    reference_start, reference_summary.commands,
                    reference_summary.decoded_polygons,
                    reference_summary.ended ? "yes" : "no",
                    reference_summary.truncated ? "yes" : "no");
        uint16_t previous_instance = std::numeric_limits<uint16_t>::max();
        for (const model2_geometry_polygon& polygon :
             reference_geometry.polygons()) {
            if (polygon.object_instance == previous_instance) continue;
            previous_instance = polygon.object_instance;
            std::printf("  reference-object=%03u address=%08x window=%u\n",
                        polygon.object_instance, polygon.object_address,
                        polygon.window);
        }
    }
    std::printf("Video: non-black=%zu hash=%016llx\n",
                machine.non_black_pixels(),
                static_cast<unsigned long long>(machine.video_frame_hash()));
    std::printf("Current instruction: %s\n", machine.current_instruction().c_str());
    const uint32_t pc = machine.program_counter();
    for (int displacement = -24; displacement <= 24; displacement += 4) {
        const uint32_t address = pc + displacement;
        std::printf("%08x%c %s\n", address,
                    displacement == 0 ? '>' : ' ',
                    machine.instruction_at(address).c_str());
    }
    for (unsigned base = 0; base < 32; base += 8) {
        std::printf("r%02u-r%02u:", base, base + 7);
        for (unsigned index = base; index < base + 8; ++index)
            std::printf(" %08x", machine.cpu_register(index));
        std::putchar('\n');
    }
    if (const char* capture = std::getenv("MODEL2_CAPTURE")) {
        assert(write_capture(machine, capture));
    }
    if (const char* buffer_path = std::getenv("MODEL2_BUFFER_DUMP")) {
        if (std::FILE* output = std::fopen(buffer_path, "wb")) {
            for (uint32_t address = 0; address < 0x20000; address += 4) {
                const uint32_t word = machine.geometry_buffer_word(address);
                std::fwrite(&word, 1, sizeof(word), output);
            }
            std::fclose(output);
        }
    }
    if (const char* work_path = std::getenv("MODEL2_WORK_DUMP")) {
        if (std::FILE* output = std::fopen(work_path, "wb")) {
            for (uint32_t address = 0x00500000; address < 0x00600000;
                 ++address) {
                const uint8_t byte = machine.debug_read8(address);
                std::fwrite(&byte, 1, 1, output);
            }
            std::fclose(output);
        }
    }
    if (const char* local_path = std::getenv("MODEL2_LOCAL_DUMP")) {
        if (std::FILE* output = std::fopen(local_path, "wb")) {
            for (uint32_t address = 0x00200000; address < 0x00240000;
                 ++address) {
                const uint8_t byte = machine.debug_read8(address);
                std::fwrite(&byte, 1, 1, output);
            }
            std::fclose(output);
        }
    }
    if (const char* tgp_path = std::getenv("MODEL2_TGP_DUMP")) {
        if (std::FILE* output = std::fopen(tgp_path, "wb")) {
            for (uint16_t address = 0; address < 0x400; ++address) {
                const uint32_t word = machine.tgp_data_word(address);
                std::fwrite(&word, 1, sizeof(word), output);
            }
            std::fclose(output);
        }
    }

    assert(machine.executed_cycles() != 0);
    assert(!machine.cpu_faulted());
    if (frames >= 100) {
        assert(machine.program_counter() != 0x005a3d70);
        assert(machine.tgp_running());
        assert(machine.tgp_uploaded_words() == 1665);
        assert(machine.tgp_program_counter() != 0);
        if (!geometry_only) assert(machine.non_black_pixels() != 0);
    }
    if (frames >= 1500) {
        assert(geometry.ended);
        assert(!geometry.truncated);
        assert(geometry.decoded_polygons > 1000);
        // Timer 0 services the main-to-SCSP command ring. This catches lost
        // low edges on shared i960 IRQ2, which leave the UART idle while the
        // 128-byte game queue fills and reports SOUND BUFFER FULL.
        assert(sound_queue_count == 0);
        assert(sound_queue_busy == 0);
        assert(midi.size() >= 31);
        assert(midi.front() == 0xff);
    }
    if (std::getenv("MODEL2_AUTO_RACE") && frames == 2220) {
        // The second rival crosses a road-data page whose offsets overlap
        // the TGP arithmetic ports at this exact checkpoint. It must remain
        // an active object; later diagnostic runs legitimately reach the end
        // of the race and retire it.
        // 0xffff in the upper flag word is the game's deletion sentinel.
        const uint32_t rival_flags = debug_u32(0x00500210);
        const uint32_t rival_state = debug_u32(0x005002e8);
        assert((rival_flags & 0xffff0000U) != 0xffff0000U);
        assert(rival_state == 1);
    }
    return 0;
}

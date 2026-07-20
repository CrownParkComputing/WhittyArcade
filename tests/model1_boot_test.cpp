#include "model1_machine.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <unordered_set>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr,
                     "usage: model1_boot_test <model1.zip> [cycles]\n");
        return 2;
    }
    const int requested_cycles = argc == 3 ? std::atoi(argv[2]) : 200000;

    model1_machine machine;
    std::string error;
    if (!machine.initialize(argv[1], &error)) {
        std::fputs(error.c_str(), stderr);
        return 1;
    }

    try {
        int executed = 0;
        if (std::getenv("MODEL1_FRAMES")) {
            for (int frame = 0; frame < requested_cycles; ++frame)
                executed += machine.run_frame();
            executed += machine.execute(100000);
        } else {
            executed = machine.execute(requested_cycles);
        }
        machine.wait_for_video();
        std::printf("V60 boot: cycles=%d PC=%08x ROM-O bank=%u "
                    "TGP words=%zu mode=%s geom-PC=%04x\n",
                    executed, machine.program_counter(), machine.rom_o_bank(),
                    static_cast<std::size_t>(machine.tgp_command_count()),
                    machine.tgp_uses_lle() ? "LLE" : "HLE",
                    machine.tgp_geometry_pc());
        std::printf("I/O board: active=%d clocks=%zu PC=%04x\n",
                    machine.io_board_active(),
                    static_cast<std::size_t>(machine.io_board_clocks()),
                    machine.io_board_pc());
        std::size_t instruction_size = 0;
        std::printf("Current instruction: %s\n",
                    machine.disassemble(machine.program_counter(),
                                        &instruction_size).c_str());
        const std::vector<uint8_t>& pixels = machine.frame_pixels();
        std::unordered_set<uint32_t> colors;
        for (std::size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
            colors.insert((static_cast<uint32_t>(pixels[offset]) << 16) |
                          (static_cast<uint32_t>(pixels[offset + 1]) << 8) |
                          pixels[offset + 2]);
        }
        std::printf("Rendered pixels=%zu unique RGB colors=%zu\n",
                    pixels.size(), colors.size());
        std::printf("Renderer: quads=%zu unknown-list-type=%08x\n",
                    machine.rendered_quad_count(),
                    machine.unknown_display_list_type());
        std::printf("Display: ctl=%04x/%04x list=%zu/%zu palette=%zu\n",
                    machine.list_control(0), machine.list_control(1),
                    machine.display_list_nonzero_bytes(0),
                    machine.display_list_nonzero_bytes(1),
                    machine.palette_nonzero_bytes());
        std::printf("List0:");
        for (std::size_t word = 0; word < 16; ++word)
            std::printf(" %04x", machine.display_list_word(0, word));
        std::printf("\nList1:");
        for (std::size_t word = 0; word < 16; ++word)
            std::printf(" %04x", machine.display_list_word(1, word));
        std::printf("\n");
        if (const char* capture = std::getenv("MODEL1_CAPTURE")) {
            if (std::FILE* output = std::fopen(capture, "wb")) {
                std::fprintf(output, "P6\n%d %d\n255\n",
                             model1_machine::native_width(),
                             model1_machine::native_height());
                for (std::size_t offset = 0; offset + 3 < pixels.size();
                     offset += 4)
                    std::fwrite(pixels.data() + offset, 1, 3, output);
                std::fclose(output);
            }
        }
        if (std::getenv("MODEL1_DISASSEMBLE_BOOT")) {
            for (uint32_t address = 0xfe1540; address < 0xfe15d0;) {
                const std::string instruction =
                    machine.disassemble(address, &instruction_size);
                std::printf("%06x: %s\n", address, instruction.c_str());
                address += instruction_size ?
                    static_cast<uint32_t>(instruction_size) : 1;
            }
            for (uint32_t address = 0xfe02bc; address < 0xfe0340;) {
                const std::string instruction =
                    machine.disassemble(address, &instruction_size);
                std::printf("%06x: %s\n", address, instruction.c_str());
                address += instruction_size ?
                    static_cast<uint32_t>(instruction_size) : 1;
            }
        }
        if (const char* range = std::getenv("MODEL1_DISASSEMBLE_RANGE")) {
            char* separator = nullptr;
            const uint32_t start = static_cast<uint32_t>(
                std::strtoul(range, &separator, 16));
            const uint32_t end = separator && *separator == ':' ?
                static_cast<uint32_t>(std::strtoul(separator + 1, nullptr, 16)) :
                start;
            for (uint32_t address = start; address < end;) {
                const std::string instruction =
                    machine.disassemble(address, &instruction_size);
                std::printf("%06x: %s\n", address, instruction.c_str());
                address += instruction_size ?
                    static_cast<uint32_t>(instruction_size) : 1;
            }
        }
        if (std::getenv("MODEL1_FRAMES") && requested_cycles >= 400) {
            const std::size_t list0 = machine.display_list_nonzero_bytes(0);
            const std::size_t list1 = machine.display_list_nonzero_bytes(1);
            const std::size_t list_delta = list0 > list1 ?
                list0 - list1 : list1 - list0;
            if (list0 < 1000 || list1 < 1000 || list_delta > 2048 ||
                machine.rendered_quad_count() < 100 || colors.size() < 32 ||
                machine.unknown_display_list_type() != 0) {
                std::fprintf(stderr,
                    "Model 1 frame regression: lists=%zu/%zu delta=%zu "
                    "quads=%zu colors=%zu unknown=%08x\n",
                    list0, list1, list_delta,
                    machine.rendered_quad_count(), colors.size(),
                    machine.unknown_display_list_type());
                return 1;
            }
            // The legacy geometry program previously deadlocked after roughly
            // 700 frames when its RF3 external-memory base was decoded as a
            // shared-RAM pointer. Keep the integration test beyond that point
            // and require continued FIFO progress.
            if (machine.tgp_uses_lle() && requested_cycles >= 800 &&
                machine.tgp_command_count() < 250000) {
                std::fprintf(stderr,
                    "Model 1 TGP stopped making progress: words=%zu pc=%04x\n",
                    static_cast<std::size_t>(machine.tgp_command_count()),
                    machine.tgp_geometry_pc());
                return 1;
            }
        }
        if (executed < requested_cycles ||
            (machine.program_counter() & 0x00ffffff) == 0x00fffff0)
            return 1;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "V60 stopped: %s (PC=%08x)\n", exception.what(),
                     machine.program_counter());
        return 1;
    }
    return 0;
}

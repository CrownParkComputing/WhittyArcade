#include "high_scores.h"
#include "test_platform.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
    std::vector<std::uint8_t> phoenix(27, 0x20);
    phoenix[0] = 0x00;
    phoenix[1] = 0x08;
    phoenix[2] = 0x80;
    phoenix[9] = 0x00;
    phoenix[10] = 0x05;
    phoenix[11] = 0x40;
    phoenix[18] = 0x00;
    phoenix[19] = 0x03;
    phoenix[20] = 0x00;
    phoenix[6] = 0x28;
    phoenix[7] = 0x28;
    phoenix[8] = 0x20;
    high_score_table table;
    std::string error;
    assert(decode_high_score_table("phoenix", phoenix, table, &error));
    assert(table.entries.size() == 1 && table.entries[0].score == 880);
    assert(table.extra_scores.size() == 2);
    assert(table.extra_scores[0].second == 540);
    assert(table.extra_scores[1].second == 300);

    std::vector<std::uint8_t> mooncrst(84, 0x24);
    for (std::size_t rank = 0; rank < 5; ++rank) {
        mooncrst[rank * 3 + 0] = 0x00;
        mooncrst[rank * 3 + 1] = 0x50;
        mooncrst[rank * 3 + 2] = 0x00;
        const std::size_t name = rank < 4 ? 18 + rank * 14 : 74;
        // Moon Cresta's character code is ASCII minus 55.
        constexpr std::array<std::uint8_t, 8> maker_name{
            0x3e, 0x3c, 0x3a, 0x38, 0x36, 0x34, 0x32, 0x30};
        std::copy(maker_name.begin(), maker_name.end(),
                  mooncrst.begin() + static_cast<std::ptrdiff_t>(name));
    }
    assert(decode_high_score_table("mooncrst", mooncrst, table, &error));
    assert(table.entries.size() == 5);
    assert(table.entries[0].score == 5000);
    assert(table.entries[0].name == "Nichibutsu");

    std::vector<std::uint8_t> shinobi(0x146, 0);
    const std::array<std::uint8_t, 8> first_shinobi{
        0x00, 0x10, 0x00, 0x00, 0x05, 'A', 'A', 'A'};
    std::copy(first_shinobi.begin(), first_shinobi.end(), shinobi.begin());
    std::copy(first_shinobi.begin(), first_shinobi.end(),
              shinobi.begin() + 0xa2);
    std::copy(first_shinobi.begin(), first_shinobi.begin() + 4,
              shinobi.begin() + 0x142);
    assert(decode_high_score_table("shinobi4", shinobi, table, &error));
    assert(table.entries.size() == 20);
    assert(table.entries[0].score == 100000);
    assert(table.entries[0].name == "AAA");

    std::vector<std::uint8_t> gng(0x5e, 0);
    for (std::size_t rank = 0; rank < 10; ++rank) {
        // Reverse record storage to prove that the rank-pointer table is used.
        gng[rank * 2] = 0x15;
        gng[rank * 2 + 1] =
            static_cast<std::uint8_t>(44 + (9 - rank) * 7);
        const std::size_t record = 20 + rank * 7;
        gng[record + 0] = 0x00;
        gng[record + 1] = static_cast<std::uint8_t>(rank);
        gng[record + 2] = 0x00;
        gng[record + 3] = 0x00;
        gng[record + 4] = 'A' + static_cast<std::uint8_t>(rank);
        gng[record + 5] = 0x1d;
        gng[record + 6] = 'Z';
    }
    gng[0x5a] = 0x00;
    gng[0x5b] = 0x09;
    assert(decode_high_score_table("gng", gng, table, &error));
    assert(table.entries.size() == 10);
    assert(table.entries[0].score == 90000);
    assert(table.entries[0].name == "J.Z");
    assert(table.entries[9].score == 0);
    assert(table.extra_scores[0].second == 90000);

    std::vector<std::uint8_t> invalid = phoenix;
    invalid[1] = 0xfa;
    assert(!decode_high_score_table("phoenix", invalid, table, &error));

    const fs::path root = fs::temp_directory_path() /
        ("whittyarcade-high-score-test-" +
         std::to_string(test_process_id()));
    fs::create_directories(root);
    if (!test_set_environment("WHITTYARCADE_HISCORE_PATH", root)) return 1;
    if (!test_set_environment("HOME", root)) return 1;

    std::array<std::uint8_t, 0x10000> memory{};
    constexpr std::array<std::uint16_t, 18> spaces{{
        0x41e1,0x41c1,0x41a1,0x4181,0x4161,0x4141,
        0x4301,0x42e1,0x42c1,0x42a1,0x4281,0x4261,
        0x40c1,0x40a1,0x4081,0x4061,0x4041,0x4021,
    }};
    for (const std::uint16_t address : spaces) memory[address] = 0x20;

    high_score_runtime runtime("phoenix");
    const auto read = [&](std::uint32_t address) { return memory.at(address); };
    const auto write = [&](std::uint32_t address, std::uint8_t value) {
        memory.at(address) = value;
    };
    runtime.update(read, write);
    assert(fs::file_size(root / "phoenix.hi") == 27);
    memory[0x4389] = 0x00;
    memory[0x438a] = 0x12;
    memory[0x438b] = 0x30;
    for (int frame = 0; frame < 60; ++frame) runtime.update(read, write);
    runtime.flush(read);
    assert(high_score_report("phoenix").find("1,230") != std::string::npos);

    fs::remove_all(root);
    return 0;
}

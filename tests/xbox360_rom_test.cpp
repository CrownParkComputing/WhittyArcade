#include "test_platform.h"
#include "xbox360_rom.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void put_be32(std::array<std::uint8_t, 0x100>& bytes, std::size_t offset,
              std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void write_xex(const fs::path& path, std::uint32_t title_id) {
    std::array<std::uint8_t, 0x100> bytes{};
    bytes[0] = 'X';
    bytes[1] = 'E';
    bytes[2] = 'X';
    bytes[3] = '2';
    put_be32(bytes, 0x14, 1);
    put_be32(bytes, 0x18, 0x00040006);
    put_be32(bytes, 0x1c, 0x40);
    put_be32(bytes, 0x4c, title_id);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).put('\0');
}

} // namespace

int main(int argc, char* argv[]) {
    const fs::path root = fs::temp_directory_path() /
        ("whittyarcade-xbox360-rom-test-" +
         std::to_string(test_process_id()));
    fs::create_directories(root);
    write_xex(root / "default.xex",
              xbox360_rom_loader::robotron_title_id);

    assert(xbox360_rom_loader::identify_set(root.string()) ==
           xbox360_rom_set::robotron_2084);
    assert(xbox360_rom_loader::identify_set(
               (root / "default.xex").string()) ==
           xbox360_rom_set::robotron_2084);
    assert(!xbox360_rom_loader::inspect(root.string()));

    touch(root / "classic" / "fw.sr");
    touch(root / "classic" / "robotron.sr");
    touch(root / "media" / "uiresource.xpr");
    const xbox360_rom_info complete =
        xbox360_rom_loader::inspect(root.string());
    assert(complete);
    assert(complete.title_id == xbox360_rom_loader::robotron_title_id);
    assert(complete.set == xbox360_rom_set::robotron_2084);

    const fs::path other = root / "other";
    fs::create_directories(other);
    write_xex(other / "default.xex", 0x12345678);
    assert(xbox360_rom_loader::identify_set(other.string()) ==
           xbox360_rom_set::unknown);

    if (argc > 1) {
        const xbox360_rom_info real = xbox360_rom_loader::inspect(argv[1]);
        assert(real);
        assert(real.title_id == xbox360_rom_loader::robotron_title_id);
    }
    fs::remove_all(root);
    return 0;
}

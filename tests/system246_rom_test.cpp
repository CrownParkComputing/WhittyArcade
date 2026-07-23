#include "system246_rom.h"
#include "test_platform.h"

#include <minizip/zip.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void put_be32(std::vector<uint8_t>& bytes, std::size_t offset,
              uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

void put_be64(std::vector<uint8_t>& bytes, std::size_t offset,
              uint64_t value) {
    put_be32(bytes, offset, static_cast<uint32_t>(value >> 32));
    put_be32(bytes, offset + 4, static_cast<uint32_t>(value));
}

uint8_t hex_nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    return static_cast<uint8_t>(value - 'a' + 10);
}

void put_sha1(std::vector<uint8_t>& bytes, std::size_t offset,
              const std::string& value) {
    assert(value.size() == 40);
    for (std::size_t index = 0; index < 20; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(
            (hex_nibble(value[index * 2]) << 4) |
            hex_nibble(value[index * 2 + 1]));
    }
}

void write_test_chd(const fs::path& path, bool valid_sha = true) {
    std::vector<uint8_t> bytes(128, 0);
    const std::array<uint8_t, 8> magic{
        'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D'};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put_be32(bytes, 8, 124);
    put_be32(bytes, 12, 5);
    put_be64(bytes, 32, 258684928);
    put_be64(bytes, 48, 124);
    put_be32(bytes, 56, 4096);
    put_be32(bytes, 60, 512);
    put_sha1(bytes, 84, valid_sha ?
        "77bb70407511cbb12ab999410e797dcaf0779229" :
        "0000000000000000000000000000000000000000");
    bytes[124] = 'G';
    bytes[125] = 'D';
    bytes[126] = 'D';
    bytes[127] = 'D';
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void put_le32(std::vector<uint8_t>& bytes, std::size_t offset,
              uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void make_metadata_fixture(const fs::path& path) {
    zipFile archive = zipOpen64(path.string().c_str(), APPEND_STATUS_CREATE);
    assert(archive);
    zip_fileinfo info{};
    assert(zipOpenNewFileInZip(archive, "merged/rrv3vera.ic002", &info,
                               nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED,
                               Z_BEST_SPEED) == ZIP_OK);
    const uint8_t byte = 0;
    assert(zipWriteInFileInZip(archive, &byte, 1) == ZIP_OK);
    assert(zipCloseFileInZip(archive) == ZIP_OK);
    assert(zipClose(archive, nullptr) == ZIP_OK);

    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    bool patched = false;
    for (std::size_t offset = 0; offset + 28 <= bytes.size(); ++offset) {
        const bool local = bytes[offset] == 0x50 && bytes[offset + 1] == 0x4b &&
                           bytes[offset + 2] == 0x03 && bytes[offset + 3] == 0x04;
        const bool central = bytes[offset] == 0x50 && bytes[offset + 1] == 0x4b &&
                             bytes[offset + 2] == 0x01 && bytes[offset + 3] == 0x02;
        if (local) {
            put_le32(bytes, offset + 14, 0xdd20c4a2);
            put_le32(bytes, offset + 22, 0x800000);
        } else if (central) {
            put_le32(bytes, offset + 16, 0xdd20c4a2);
            put_le32(bytes, offset + 24, 0x800000);
            patched = true;
        }
    }
    assert(patched);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace

int main(int argc, char** argv) {
    using set = system246_rom_set;
    assert(std::string(system246_rom_loader::set_short_name(
               set::ridge_racer_v_arcade_battle)) == "rrvac");

    const fs::path root = fs::temp_directory_path() /
        ("whittyarcade-system246-rom-test-" +
         std::to_string(test_process_id()));
    fs::create_directories(root);
    const fs::path archive = root / "renamed.zip";
    make_metadata_fixture(archive);
    assert(system246_rom_loader::identify_set(archive.string()) ==
           set::ridge_racer_v_arcade_battle);

    const fs::path disc = root / "RRV1-A.CHD";
    write_test_chd(disc);
    const system246_disc_info inspected =
        system246_rom_loader::inspect_disc(disc.string());
    assert(inspected);
    assert(inspected.container == system246_disc_container::hard_disk_chd);
    assert(inspected.logical_bytes == 258684928);
    assert(system246_rom_loader::find_disc_path(archive.string()) ==
           disc.string());

    write_test_chd(disc, false);
    assert(!system246_rom_loader::inspect_disc(disc.string()));
    // The forged central-directory metadata identifies the set, but loading
    // still decompresses and CRC-checks the dongle contents.
    assert(!system246_rom_loader::load(archive.string()));

    if (argc == 3) {
        assert(system246_rom_loader::identify_set(argv[1]) ==
               set::ridge_racer_v_arcade_battle);
        const system246_disc_info real_disc =
            system246_rom_loader::inspect_disc(argv[2]);
        assert(real_disc);
        const system246_rom_load_result loaded =
            system246_rom_loader::load(argv[1]);
        assert(loaded);
        assert(loaded.disc_path == fs::path(argv[2]).lexically_normal().string());
    }

    fs::remove_all(root);
    return 0;
}

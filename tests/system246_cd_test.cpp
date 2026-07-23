#include "iop/namco_sys246/Iop_NamcoAcCdvdSectorTransfer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t sector_size = Iop::Namco::AC_CDVD_SECTOR_SIZE;

void test_multi_sector_read_is_contiguous() {
    std::vector<std::uint8_t> media(8 * sector_size);
    for (std::size_t sector = 0; sector < 8; ++sector) {
        std::fill_n(media.begin() + (sector * sector_size), sector_size,
                    static_cast<std::uint8_t>(0x20 + sector));
    }

    std::array<std::uint8_t, 3 * sector_size> destination{};
    std::vector<std::uint32_t> sectors;
    const std::uint32_t next = Iop::Namco::TransferSequentialSectors(
        2, 3, [&](std::uint32_t sector, std::size_t offset) {
            sectors.push_back(sector);
            std::copy_n(media.begin() + (sector * sector_size), sector_size,
                        destination.begin() + offset);
        });

    assert((sectors == std::vector<std::uint32_t>{2, 3, 4}));
    assert(next == 5);
    for (std::size_t index = 0; index < 3; ++index) {
        const auto expected = static_cast<std::uint8_t>(0x22 + index);
        assert(destination[index * sector_size] == expected);
        assert(destination[((index + 1) * sector_size) - 1] == expected);
    }
}

void test_stream_cursor_continues_across_requests() {
    std::uint32_t stream_position = 0x120;
    std::vector<std::uint32_t> sectors;
    std::vector<std::size_t> offsets;

    const auto read = [&](std::uint32_t count) {
        stream_position = Iop::Namco::TransferSequentialSectors(
            stream_position, count,
            [&](std::uint32_t sector, std::size_t offset) {
                sectors.push_back(sector);
                offsets.push_back(offset);
            });
    };

    read(3);
    read(2);

    assert((sectors == std::vector<std::uint32_t>{
        0x120, 0x121, 0x122, 0x123, 0x124}));
    assert((offsets == std::vector<std::size_t>{
        0, sector_size, 2 * sector_size, 0, sector_size}));
    assert(stream_position == 0x125);
}

void test_zero_length_read_does_not_advance() {
    bool called = false;
    const std::uint32_t next = Iop::Namco::TransferSequentialSectors(
        77, 0, [&](std::uint32_t, std::size_t) { called = true; });
    assert(!called);
    assert(next == 77);
}

void test_audio_read_is_complete_before_reply() {
    constexpr std::uint32_t first_sector = 21;
    constexpr std::uint32_t sector_count = 20;

    std::vector<std::uint8_t> media(
        (first_sector + sector_count) * sector_size, 0);
    for (std::uint32_t index = 0; index < sector_count; ++index) {
        const auto value = static_cast<std::uint8_t>(0x31 + index);
        std::fill_n(media.begin() +
                        ((first_sector + index) * sector_size),
                    sector_size, value);
    }

    std::array<std::uint8_t, sector_count * sector_size> destination;
    destination.fill(0xCC);
    std::vector<std::uint32_t> sectors;
    const std::uint32_t next = Iop::Namco::TransferSequentialSectors(
        first_sector, sector_count,
        [&](std::uint32_t sector, std::size_t offset) {
            sectors.push_back(sector);
            std::copy_n(media.begin() + (sector * sector_size), sector_size,
                        destination.begin() + offset);
        });

    // ACCDVD method 0x0A performs this operation before setting ret[1] and
    // notifying the music observer. No partially filled stream is exposed.
    assert(sectors.front() == 21);
    assert(sectors.back() == 40);
    assert(next == 41);
    for (std::uint32_t index = 0; index < sector_count; ++index) {
        const auto expected = static_cast<std::uint8_t>(0x31 + index);
        assert(destination[index * sector_size] == expected);
        assert(destination[((index + 1) * sector_size) - 1] == expected);
    }
}

void test_accdvd_destination_routing_matches_original_module() {
    using Iop::Namco::AcCdvdReadTarget;
    using Iop::Namco::GetAcCdvdReadTarget;

    assert(GetAcCdvdReadTarget(0x09, 0x40001234) ==
           AcCdvdReadTarget::EE_RAM);
    assert(GetAcCdvdReadTarget(0x0A, 0x00001234) ==
           AcCdvdReadTarget::IOP_RAM);
    assert(GetAcCdvdReadTarget(0x0A, 0x40001234) ==
           AcCdvdReadTarget::AC_RAM);
    assert(GetAcCdvdReadTarget(0x0A, 0x4FFFFFFF) ==
           AcCdvdReadTarget::AC_RAM);
    assert(GetAcCdvdReadTarget(0x0A, 0x50001234) ==
           AcCdvdReadTarget::IOP_RAM);
}

void test_rrv_iso_music_sector(const std::string& path) {
    constexpr std::uint32_t rrv_file_extent = 21;
    std::ifstream image(path, std::ios::binary);
    assert(image.good());
    image.seekg(static_cast<std::streamoff>(rrv_file_extent) * sector_size);

    std::array<std::uint8_t, sector_size> sector{};
    image.read(reinterpret_cast<char*>(sector.data()), sector.size());
    assert(image.gcount() == static_cast<std::streamsize>(sector.size()));

    const auto nonzero = std::count_if(
        sector.begin(), sector.end(),
        [](std::uint8_t value) { return value != 0; });
    assert(nonzero > 64);

    std::size_t plausible_adpcm_blocks = 0;
    for (std::size_t offset = 0; offset < sector.size(); offset += 16) {
        const std::uint8_t predictor = sector[offset] >> 4;
        const std::uint8_t flags = sector[offset + 1];
        plausible_adpcm_blocks += predictor <= 4 && (flags & ~0x07) == 0;
    }
    assert(plausible_adpcm_blocks >= 32);
}

} // namespace

int main(int argc, char** argv) {
    test_multi_sector_read_is_contiguous();
    test_stream_cursor_continues_across_requests();
    test_zero_length_read_does_not_advance();
    test_audio_read_is_complete_before_reply();
    test_accdvd_destination_routing_matches_original_module();
    if (argc == 2) {
        test_rrv_iso_music_sector(argv[1]);
    } else {
        assert(argc == 1);
    }
    return 0;
}

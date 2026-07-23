#include "play_media_cache.h"

#include "platform_paths.h"

#include <libchdr/chd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t rrv_iso_bytes = 258684928;
constexpr const char* rrv_disc_sha1 =
    "77bb70407511cbb12ab999410e797dcaf0779229";

bool is_valid_cached_iso(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec ||
        fs::file_size(path, ec) != rrv_iso_bytes || ec)
        return false;

    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(16 * 2048, std::ios::beg);
    std::array<uint8_t, 7> descriptor{};
    input.read(reinterpret_cast<char*>(descriptor.data()), descriptor.size());
    static constexpr std::array<uint8_t, 7> iso9660_pvd = {
        1, 'C', 'D', '0', '0', '1', 1,
    };
    return input.gcount() ==
               static_cast<std::streamsize>(descriptor.size()) &&
           descriptor == iso9660_pvd;
}

fs::path cache_root() {
    fs::path root = whitty_platform::data_root();
    if (root.empty()) {
        std::error_code ec;
        root = fs::current_path(ec);
        if (ec) return {};
    }
    return root / "WhittyArcade" / "cache" / "system246";
}

struct chd_closer {
    void operator()(chd_file* file) const {
        if (file) chd_close(file);
    }
};

} // namespace

bool prepare_system246_optical_media(
        const system246_rom_load_result& roms,
        std::string& optical_path, std::string& error) {
    optical_path.clear();
    error.clear();
    if (!roms) {
        error = roms.error.empty() ? "System 246 media is not loaded." :
                                     roms.error;
        return false;
    }
    if (roms.disc.container == system246_disc_container::cdrom_chd) {
        optical_path = roms.disc_path;
        return true;
    }
    if (roms.disc.container != system246_disc_container::hard_disk_chd) {
        error = "Unsupported System 246 disc container.";
        return false;
    }

    const fs::path directory = cache_root();
    if (directory.empty()) {
        error = "Could not determine the System 246 cache directory.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        error = "Could not create the System 246 media cache: " +
                ec.message();
        return false;
    }

    const fs::path cached = directory /
        (std::string("rrvac-") + rrv_disc_sha1 + ".iso");
    if (is_valid_cached_iso(cached)) {
        optical_path = cached.string();
        return true;
    }

    const fs::path temporary = cached.string() + ".tmp";
    fs::remove(temporary, ec);
    ec.clear();

    chd_file* raw_chd = nullptr;
    const chd_error open_result = chd_open(
        fs::path(roms.disc_path).string().c_str(), CHD_OPEN_READ, nullptr,
        &raw_chd);
    std::unique_ptr<chd_file, chd_closer> image(raw_chd);
    if (open_result != CHDERR_NONE || !image) {
        error = std::string("Could not open rrv1-a.chd: ") +
                chd_error_string(open_result);
        return false;
    }

    const chd_header* header = chd_get_header(image.get());
    if (!header || header->logicalbytes != rrv_iso_bytes ||
        header->hunkbytes == 0) {
        error = "rrv1-a.chd has unexpected decompressed geometry.";
        return false;
    }
    const uint64_t hunk_count =
        (header->logicalbytes + header->hunkbytes - 1) / header->hunkbytes;
    if (hunk_count > std::numeric_limits<uint32_t>::max()) {
        error = "rrv1-a.chd contains too many hunks.";
        return false;
    }

    std::fprintf(stdout,
                 "Preparing one-time System 246 ISO cache (%llu MiB)...\n",
                 static_cast<unsigned long long>(
                     (header->logicalbytes + ((1u << 20) - 1)) >> 20));
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Could not create the System 246 ISO cache.";
        return false;
    }
    std::vector<uint8_t> hunk(header->hunkbytes);
    uint64_t remaining = header->logicalbytes;
    for (uint64_t index = 0; index < hunk_count; ++index) {
        const chd_error read_result = chd_read(
            image.get(), static_cast<uint32_t>(index), hunk.data());
        if (read_result != CHDERR_NONE) {
            output.close();
            fs::remove(temporary, ec);
            error = std::string("Could not decompress rrv1-a.chd: ") +
                    chd_error_string(read_result);
            return false;
        }
        const std::size_t count = static_cast<std::size_t>(
            std::min<uint64_t>(remaining, hunk.size()));
        output.write(reinterpret_cast<const char*>(hunk.data()), count);
        if (!output) {
            output.close();
            fs::remove(temporary, ec);
            error = "Writing the System 246 ISO cache failed.";
            return false;
        }
        remaining -= count;
    }
    output.close();
    image.reset();

    if (!is_valid_cached_iso(temporary)) {
        fs::remove(temporary, ec);
        error = "Decompressed RRV media is not the expected ISO9660 image.";
        return false;
    }
    if (fs::exists(cached, ec)) {
        ec.clear();
        fs::remove(cached, ec);
        if (ec) {
            fs::remove(temporary, ec);
            error = "Could not replace an invalid System 246 media cache.";
            return false;
        }
    }
    ec.clear();
    fs::rename(temporary, cached, ec);
    if (ec) {
        const std::string rename_error = ec.message();
        std::error_code remove_error;
        fs::remove(temporary, remove_error);
        error = "Could not finalize the System 246 media cache: " +
                rename_error;
        return false;
    }
    optical_path = cached.string();
    std::fprintf(stdout, "System 246 ISO cache ready: %s\n",
                 optical_path.c_str());
    return true;
}

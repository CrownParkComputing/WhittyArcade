// MAME CHD v5 hard disk image reader.
//
// Reads the header and hunk map from a CHD v5 file and provides
// sector-level read access. Only the subset of the format used by
// raw hard disk images (metadata tag "GDDD") is supported.
//
// CHD v5 header layout (big-endian, 124 bytes):
//   Offset  Size  Field
//    0       8    Tag "MComprHD"
//    8       4    Header length (124)
//   12       4    Version (5)
//   16      16    4x Compression types (uint32_t each)
//   32       8    Logical bytes
//   40       8    Map offset
//   48       8    Meta offset
//   56       4    Hunk bytes
//   60       4    Unit bytes
//   64      40    Raw SHA-1 + overall SHA-1 (20 bytes each)
//  104      20    Parent SHA-1 (for diff disks)
//  124       -    Hunk map (hunk_count+1 entries of compressed sizes)
//
// Note: CHD v5 removed hunk_count from the header; it is calculated
// from logical_bytes / hunk_bytes.

#include "midway/midway_chd.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include <zlib.h>

namespace {

constexpr std::size_t chd_v5_header_size = 124;

uint32_t read_be32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint64_t read_be64(const uint8_t* bytes) {
    return (static_cast<uint64_t>(read_be32(bytes)) << 32) |
           read_be32(bytes + 4);
}

std::string hex_bytes(const uint8_t* bytes, std::size_t size) {
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i)
        text << std::setw(2) << static_cast<unsigned>(bytes[i]);
    return text.str();
}

struct chd_state {
    std::string path;
    chd_info info;
    // Cached hunk map: compressed size of each hunk (hunk_count entries)
    std::vector<uint64_t> hunk_map;
    // Offset in file to the start of hunk data (after hunk map)
    uint64_t hunk_data_offset{};
    bool valid{};
};

// Single global cache: open once per path. The session owns one game
// at a time, so this is sufficient.
chd_state g_chd;

} // namespace

chd_info chd_open(const std::string& path) {
    if (path.empty()) {
        chd_info result;
        result.error = "Empty CHD path.";
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        chd_info result;
        result.error = "Could not open CHD file: " + path;
        return result;
    }

    // Read header.
    std::array<uint8_t, chd_v5_header_size> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        chd_info result;
        result.error = "CHD file is truncated (header too small).";
        return result;
    }

    // Verify magic and version.
    if (std::memcmp(header.data(), "MComprHD", 8) != 0) {
        chd_info result;
        result.error = "Not a CHD file (bad magic).";
        return result;
    }
    if (read_be32(header.data() + 8) != chd_v5_header_size ||
        read_be32(header.data() + 12) != 5) {
        chd_info result;
        result.error = "Only CHD v5 images are supported.";
        return result;
    }

    chd_info result;
    // CHD v5 fields at correct offsets.
    const uint32_t compression  = read_be32(header.data() + 16);
    result.logical_bytes = read_be64(header.data() + 32);
    const uint64_t map_offset   = read_be64(header.data() + 40);
    const uint64_t meta_offset  = read_be64(header.data() + 48);
    result.hunk_bytes    = read_be32(header.data() + 56);
    result.unit_bytes    = read_be32(header.data() + 60);
    result.sha1                 = hex_bytes(header.data() + 64, 20);

    if (result.hunk_bytes == 0 || result.unit_bytes == 0) {
        result.error = "CHD has invalid hunk/unit geometry.";
        return result;
    }
    // CHD v5 removed hunk_count from the header; calculate it.
    const uint32_t hunk_count = static_cast<uint32_t>(
        (result.logical_bytes + result.hunk_bytes - 1) / result.hunk_bytes);
    result.hunk_count = hunk_count;

    // Read metadata tag.
    if (meta_offset == 0) {
        result.error = "CHD has no media metadata.";
        return result;
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(meta_offset), std::ios::beg);
    std::array<char, 4> tag{};
    input.read(tag.data(), tag.size());
    if (input.gcount() != static_cast<std::streamsize>(tag.size())) {
        result.error = "CHD metadata is truncated.";
        return result;
    }
    result.metadata_tag.assign(tag.data(), tag.size());

    if (result.metadata_tag == "GDDD")
        result.container = chd_container::hard_disk;
    else if (result.metadata_tag == "CHT2" || result.metadata_tag == "CHCD")
        result.container = chd_container::cdrom;
    else {
        result.error = "Unsupported CHD metadata type: " +
                       result.metadata_tag;
        return result;
    }

    // For KI, we only support uncompressed (compression == 0) or
    // zlib-compressed (compression == 1) hunks. The hunk map is
    // hunk_count+1 entries of uint64_t big-endian offsets.

    // Read the hunk map.
    // CHD v5: the hunk map has hunk_count entries (no +1 sentinel).
    // Each entry is a uint64_t compressed length.
    const std::size_t map_entries = static_cast<std::size_t>(hunk_count);
    const std::size_t map_bytes = map_entries * 8;
    std::vector<uint8_t> map_buf(map_bytes);
    input.seekg(static_cast<std::streamoff>(map_offset), std::ios::beg);
    input.read(reinterpret_cast<char*>(map_buf.data()),
               static_cast<std::streamsize>(map_bytes));
    if (input.gcount() != static_cast<std::streamsize>(map_bytes)) {
        result.error = "CHD hunk map is truncated.";
        return result;
    }

    // Cache in the global state.
    // CHD v5: map entries are compressed sizes. Convert to cumulative
    // offsets so the existing hunk-reading code (which uses
    // hunk_map[hunk_index+1] as the end) works correctly.
    g_chd.path = path;
    g_chd.info = result;
    g_chd.hunk_map.resize(map_entries + 1);  // +1 for sentinel
    uint64_t cum = 0;
    for (std::size_t i = 0; i < map_entries; ++i) {
        g_chd.hunk_map[i] = cum;
        cum += read_be64(map_buf.data() + i * 8);
    }
    g_chd.hunk_map[map_entries] = cum;  // sentinel: end of last hunk
    // CHD v5: hunk data starts right after the hunk map.
    g_chd.hunk_data_offset = map_offset + map_bytes;
    g_chd.valid = true;

    return result;
}

std::size_t chd_read_sectors(const std::string& path,
                             const chd_info& info,
                             uint64_t sector, void* dst,
                             std::size_t sector_count) {
    if (!g_chd.valid || g_chd.path != path) {
        // Re-open if needed.
        chd_info reopened = chd_open(path);
        if (!reopened) return 0;
    }
    if (sector * 512 + sector_count * 512 > info.logical_bytes)
        return 0;
    if (sector_count == 0) return 0;

    const uint32_t hunk_size = info.hunk_bytes;
    const uint32_t sectors_per_hunk = hunk_size / 512;
    uint8_t* out = static_cast<uint8_t*>(dst);
    std::size_t sectors_read = 0;

    std::ifstream input(g_chd.path, std::ios::binary);
    if (!input) return 0;

    std::vector<uint8_t> hunk_buf(hunk_size);
    std::vector<uint8_t> comp_buf;

    for (std::size_t i = 0; i < sector_count; ++i) {
        const uint64_t abs_sector = sector + i;
        const uint32_t hunk_index = static_cast<uint32_t>(abs_sector / sectors_per_hunk);
        const uint32_t hunk_offset = static_cast<uint32_t>(abs_sector % sectors_per_hunk) * 512;

        if (hunk_index >= info.hunk_count) continue;

        const uint64_t hunk_start = g_chd.hunk_map[hunk_index];
        const uint64_t hunk_end   = g_chd.hunk_map[hunk_index + 1];
        const uint64_t compressed = hunk_end - hunk_start;

        if (compressed == 0) {
            // Empty hunk: zero-fill.
            std::memset(out + i * 512, 0, 512);
            ++sectors_read;
            continue;
        }

        input.seekg(static_cast<std::streamoff>(g_chd.hunk_data_offset + hunk_start),
                    std::ios::beg);

        if (compressed == hunk_size) {
            // Uncompressed: read directly.
            input.read(reinterpret_cast<char*>(hunk_buf.data()),
                       static_cast<std::streamsize>(hunk_size));
            if (input.gcount() != static_cast<std::streamsize>(hunk_size))
                break;
        } else {
            // zlib-compressed: decompress.
            if (comp_buf.size() < compressed)
                comp_buf.resize(static_cast<std::size_t>(compressed));
            input.read(reinterpret_cast<char*>(comp_buf.data()),
                       static_cast<std::streamsize>(compressed));
            if (input.gcount() != static_cast<std::streamsize>(compressed))
                break;

            uLongf dest_len = hunk_size;
            const int zerr = uncompress(hunk_buf.data(), &dest_len,
                                        comp_buf.data(),
                                        static_cast<uLong>(compressed));
            if (zerr != Z_OK || dest_len != hunk_size) break;
        }

        std::memcpy(out + i * 512, hunk_buf.data() + hunk_offset, 512);
        ++sectors_read;
    }

    return sectors_read;
}

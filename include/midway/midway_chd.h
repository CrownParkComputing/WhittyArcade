// MAME CHD v5 hard disk image reader.
//
// Reads raw sectors from a CHD v5 image. Used by the Midway Wolf Unit
// board to access the Killer Instinct hard disk, and can be reused by
// any other board that boots from a CHD-backed IDE drive.
//
// The format is MAME's "Compressed Hunks of Data" v5, documented at
// https://github.com/mamedev/mame/blob/master/src/lib/util/chd.cpp
// This is a clean-room implementation that reads only the subset of
// the format needed by KI.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class chd_container : uint8_t {
    unknown,
    hard_disk,
    cdrom,
};

struct chd_info {
    chd_container container{chd_container::unknown};
    uint64_t logical_bytes{};
    uint32_t hunk_bytes{};
    uint32_t unit_bytes{};
    uint32_t hunk_count{};
    std::string sha1;
    std::string metadata_tag;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

// Open and parse a CHD v5 image. Returns info on success; info.error
// is non-empty on failure.
chd_info chd_open(const std::string& path);

// Read one 512-byte sector. Returns sector_count bytes written.
// sector * 512 must not exceed logical_bytes.
// Returns 0 on error.
std::size_t chd_read_sectors(const std::string& path,
                             const chd_info& info,
                             uint64_t sector, void* dst,
                             std::size_t sector_count);

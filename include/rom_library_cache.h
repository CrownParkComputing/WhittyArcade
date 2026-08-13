// rom_library_cache.h - SQLite-backed cache of the discovered library so an
// unchanged ROM/media tree loads in milliseconds instead of re-probing every
// archive/squashfs on each launch.
#pragma once

#include "arcade_types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace rom_cache {

// One cached discovery result, keyed by absolute path. The file size and
// mtime are the validity stamp: if a file on disk still has these, the cached
// label/board/publisher are still correct and the expensive probe is skipped.
struct entry {
    std::string path;
    std::uintmax_t size{};
    std::int64_t mtime_ns{};         // file_time_type since epoch, in ns
    std::string label;
    arcade_board_type board{arcade_board_type::system22};
    std::string publisher;
    std::string short_name;          // for media lookups / identity
};

using table = std::unordered_map<std::string, entry>;

// Absolute path of the cache database (data_root()/MANX/rom_cache.sqlite).
std::filesystem::path cache_path();

// Nanoseconds since the epoch of a file's last-write time (0 on error). Used
// as the validity stamp alongside file size.
std::int64_t mtime_ns(const std::filesystem::path& path);

// Load every cached entry into a map keyed by path. Creates the schema on
// first use. Empty table on any error (callers fall back to a full scan).
table load();

// Upsert one entry (INSERT OR REPLACE).
void upsert(const entry& value);

// Look up a path; returns true and fills `out` only when a row exists whose
// size and mtime_ns match the file on disk (i.e. the entry is still valid).
bool find(const std::string& path, std::uintmax_t size, std::int64_t mtime_ns,
          entry* out);

// Remove entries whose file no longer exists on disk. Call once after a scan
// so stale rows (deleted ROMs) do not accumulate or shadow future matches.
void prune_missing(const table& entries);

} // namespace rom_cache

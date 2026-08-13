// rom_library_cache.cpp - SQLite-backed library cache.
#include "rom_library_cache.h"
#include "platform_paths.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Convert a fs::file_time_type to nanoseconds since the epoch. file_time_type
// is a clock-duration; on Linux it is system_clock-based so casting to ns is
// exact and stable across runs.
// (mtime_ns is public in rom_library_cache.h; the definition lives below.)

// The one and only live connection. SQLite handles are thread-safe for the
// separate serialized access we do here (single-threaded discovery). Open
// lazily on first use; the DB lives under the user data root.
sqlite3* open_db() {
    static sqlite3* db = []() -> sqlite3* {
        fs::path dir = rom_cache::cache_path().parent_path();
        std::error_code ec;
        fs::create_directories(dir, ec);
        sqlite3* handle = nullptr;
        if (sqlite3_open(rom_cache::cache_path().string().c_str(), &handle) !=
            SQLITE_OK) {
            if (handle) sqlite3_close(handle);
            return nullptr;
        }
        // Fast local cache: WAL avoids fsync stalls on every upsert.
        sqlite3_exec(handle, "PRAGMA journal_mode=WAL;"
                             "PRAGMA synchronous=NORMAL;"
                             "CREATE TABLE IF NOT EXISTS roms ("
                             " path TEXT PRIMARY KEY,"
                             " size INTEGER NOT NULL,"
                             " mtime_ns INTEGER NOT NULL,"
                             " label TEXT NOT NULL,"
                             " board INTEGER NOT NULL,"
                             " publisher TEXT NOT NULL,"
                             " short_name TEXT NOT NULL"
                             ");",
                     nullptr, nullptr, nullptr);
        return handle;
    }();
    return db;
}

} // namespace

namespace rom_cache {

std::int64_t mtime_ns(const fs::path& path) {
    std::error_code ec;
    const fs::file_time_type ft = fs::last_write_time(path, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               ft.time_since_epoch())
        .count();
}

std::filesystem::path cache_path() {
    return manx_platform::data_root() / "MANX" / "rom_cache.sqlite";
}

table load() {
    table result;
    sqlite3* db = open_db();
    if (!db) return result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT path,size,mtime_ns,label,board,publisher,short_name FROM roms;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        entry e;
        e.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        e.size = static_cast<std::uintmax_t>(
            sqlite3_column_int64(stmt, 1));
        e.mtime_ns = sqlite3_column_int64(stmt, 2);
        e.label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        e.board = static_cast<arcade_board_type>(sqlite3_column_int(stmt, 4));
        e.publisher =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        e.short_name =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        result.emplace(e.path, std::move(e));
    }
    sqlite3_finalize(stmt);
    return result;
}

void upsert(const entry& value) {
    sqlite3* db = open_db();
    if (!db) return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO roms (path,size,mtime_ns,label,"
                      "board,publisher,short_name) VALUES (?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, value.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(value.size));
    sqlite3_bind_int64(stmt, 3, value.mtime_ns);
    sqlite3_bind_text(stmt, 4, value.label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, static_cast<int>(value.board));
    sqlite3_bind_text(stmt, 6, value.publisher.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, value.short_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void prune_missing(const table& entries) {
    sqlite3* db = open_db();
    if (!db) return;
    std::vector<std::string> gone;
    for (const auto& [path, value] : entries) {
        std::error_code ec;
        if (!fs::exists(value.path, ec)) gone.push_back(path);
    }
    if (gone.empty()) return;
    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM roms WHERE path=?;", -1, &stmt,
                       nullptr);
    for (const std::string& path : gone) {
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
}

bool find(const std::string& path, std::uintmax_t size, std::int64_t mtime_ns,
          entry* out) {
    sqlite3* db = open_db();
    if (!db || !out) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT label,board,publisher,short_name FROM roms "
                      "WHERE path=? AND size=? AND mtime_ns=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(size));
    sqlite3_bind_int64(stmt, 3, mtime_ns);
    const bool hit = sqlite3_step(stmt) == SQLITE_ROW;
    if (hit) {
        out->path = path;
        out->size = size;
        out->mtime_ns = mtime_ns;
        out->label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        out->board = static_cast<arcade_board_type>(sqlite3_column_int(stmt, 1));
        out->publisher = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        out->short_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    }
    sqlite3_finalize(stmt);
    return hit;
}

} // namespace rom_cache

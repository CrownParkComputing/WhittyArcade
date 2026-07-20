// WhittyArcade ROM library discovery, manifests and import support.
#pragma once

#include "arcade_catalog.h"

#include <cstddef>
#include <string>
#include <vector>

struct rom_import_result {
    std::size_t archives_scanned{};
    std::size_t games_found{};
    std::size_t archives_imported{};
    std::vector<std::string> details;
    std::string error;

    explicit operator bool() const { return error.empty(); }
    std::string summary() const;
};

struct rom_audit_result {
    bool success{};
    std::string set_name;
    std::string message;
};

// XDG data directory used for durable imports. Archives are copied unchanged
// beneath one subdirectory per board; they are never extracted or repacked.
std::string rom_library_path();

// Locate supported games in the durable library, established legacy folders,
// an explicit/current path, and WHITTYARCADE_ROM_PATH when it is set.
std::vector<rom_choice> discover_library_roms(const std::string& current_path);

// Import a ZIP, several ZIPs, or a directory containing a MAME collection.
// Directory scans use supported MAME short names, avoiding a costly probe of
// every unrelated archive in a large collection.
rom_import_result import_rom_path(const std::string& source_path);
rom_import_result import_rom_paths(const std::vector<std::string>& source_paths);
rom_audit_result audit_rom_path(const std::string& path);

// Human-readable, build-specific list used by both the frontend and README.
std::string required_rom_sets_text();

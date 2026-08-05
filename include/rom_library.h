// MANX ROM/CHD folders: read directly, no import step.
#pragma once

#include "arcade_catalog.h"

#include <cstddef>
#include <string>
#include <vector>

struct rom_audit_result {
    bool success{};
    std::string set_name;
    std::string message;
};

// Folders MANX reads from directly. Drop MAME set ZIPs into the ROM
// folder and disc images into the CHD folder; nothing is copied or repacked.
std::string rom_library_path();
std::string chd_library_path();

// Locate supported games in the ROM folder (and an explicit/current path).
std::vector<rom_choice> discover_library_roms(const std::string& current_path);

// Read-only completeness check for a selected set against its board loader.
rom_audit_result audit_rom_path(const std::string& path);

// Human-readable, build-specific list of supported sets.
std::string required_rom_sets_text();

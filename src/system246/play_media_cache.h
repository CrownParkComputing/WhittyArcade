#pragma once

#include "system246_rom.h"

#include <string>

// Play! expects optical-media CHD metadata. The known RRV MAME image is a
// hard-disk CHD containing an ISO9660 byte stream, so it is expanded once to
// a private, validated cache without modifying the user's source image.
bool prepare_system246_optical_media(
    const system246_rom_load_result& roms,
    std::string& optical_path, std::string& error);

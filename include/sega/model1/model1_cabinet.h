// Sega Model 1 operator EEPROM helpers.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace model1_cabinet {

using nvram_image = std::array<uint8_t, 0x80>;

inline constexpr std::size_t checksum_low = 0x08;
inline constexpr std::size_t checksum_high = 0x09;
inline constexpr std::size_t checksum_start = 0x0a;
inline constexpr std::size_t advertise_sound = 0x0b;

inline uint16_t checksum(const nvram_image& image) {
    // Sega's cabinet descriptor uses CRC-16/CCITT (polynomial 0x1021,
    // initial value zero) over bytes 0x0a..0x7f, stored little-endian.
    uint16_t crc = 0;
    for (std::size_t index = checksum_start; index < image.size(); ++index) {
        crc ^= static_cast<uint16_t>(image[index]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ?
                static_cast<uint16_t>((crc << 1) ^ 0x1021) :
                static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

inline bool checksum_valid(const nvram_image& image) {
    const uint16_t stored = static_cast<uint16_t>(
        image[checksum_low] | (uint16_t{image[checksum_high]} << 8));
    return stored == checksum(image);
}

inline bool attract_sound_enabled(const nvram_image& image) {
    return image[advertise_sound] != 0;
}

inline void set_attract_sound(nvram_image& image, bool enabled) {
    image[advertise_sound] = enabled ? 1 : 0;
    const uint16_t crc = checksum(image);
    image[checksum_low] = static_cast<uint8_t>(crc);
    image[checksum_high] = static_cast<uint8_t>(crc >> 8);
}

} // namespace model1_cabinet

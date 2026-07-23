#include "video_deinterlace.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

constexpr int rgba_channels = 4;

} // namespace

bool system246_field_deinterlacer::process(
        const std::uint8_t* field, int width, int field_height,
        bool /*odd_field*/, std::vector<std::uint8_t>& progressive) {
    progressive.clear();
    if (!field || width <= 0 || field_height <= 0 ||
        width > std::numeric_limits<int>::max() / rgba_channels ||
        field_height > std::numeric_limits<int>::max() / 2)
        return false;

    const std::size_t row_bytes =
        static_cast<std::size_t>(width) * rgba_channels;
    const std::size_t frame_size =
        row_bytes * static_cast<std::size_t>(field_height) * 2;
    if (m_width != width || m_field_height != field_height) {
        reset();
        m_width = width;
        m_field_height = field_height;
    }
    progressive.resize(frame_size);
    for (int output_y = 0; output_y < field_height; ++output_y) {
        const std::uint8_t* source =
            field + static_cast<std::size_t>(output_y) * row_bytes;
        std::uint8_t* destination =
            progressive.data() +
            static_cast<std::size_t>(output_y * 2) * row_bytes;
        std::memcpy(destination, source, row_bytes);
        std::memcpy(destination + row_bytes, source, row_bytes);
    }
    return true;
}

void system246_field_deinterlacer::reset() {
    m_width = 0;
    m_field_height = 0;
}

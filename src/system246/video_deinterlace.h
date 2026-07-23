#pragma once

#include <cstdint>
#include <vector>

// RRV renders a complete progressive 640x224 picture into every FFMD field.
// Treating those pictures as complementary scanline sets makes fine text and
// the entire scene alternate between clean and combed frames. Captured
// even/odd field pairs use the same row origin, so expand either picture
// independently with an exact 2x vertical nearest/bob operation.
class system246_field_deinterlacer {
public:
    bool process(const std::uint8_t* rgba_field, int width, int field_height,
                 bool odd_field, std::vector<std::uint8_t>& progressive);
    void reset();

private:
    int m_width{};
    int m_field_height{};
};

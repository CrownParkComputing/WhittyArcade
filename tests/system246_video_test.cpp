#include "system246/video_deinterlace.h"
#include "gs/GsMemoryRange.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {

using pixel = std::array<std::uint8_t, 4>;

std::vector<std::uint8_t> make_field(pixel top, pixel bottom) {
    return {top[0], top[1], top[2], top[3],
            bottom[0], bottom[1], bottom[2], bottom[3]};
}

std::vector<std::uint8_t> make_three_row_field(
        pixel top, pixel middle, pixel bottom) {
    return {top[0], top[1], top[2], top[3],
            middle[0], middle[1], middle[2], middle[3],
            bottom[0], bottom[1], bottom[2], bottom[3]};
}

pixel at(const std::vector<std::uint8_t>& frame, int y) {
    const std::size_t offset = static_cast<std::size_t>(y) * 4;
    return {frame[offset + 0], frame[offset + 1],
            frame[offset + 2], frame[offset + 3]};
}

void test_field_is_reconstructed_at_full_height() {
    system246_field_deinterlacer deinterlacer;
    const auto field = make_field({10, 20, 30, 40}, {30, 40, 50, 60});
    std::vector<std::uint8_t> output;
    assert(deinterlacer.process(field.data(), 1, 2, false, output));
    assert(output.size() == 1 * 4 * 4);
    assert((at(output, 0) == pixel{10, 20, 30, 40}));
    assert((at(output, 1) == pixel{10, 20, 30, 40}));
    assert((at(output, 2) == pixel{30, 40, 50, 60}));
    assert((at(output, 3) == pixel{30, 40, 50, 60}));
}

void test_each_field_is_expanded_without_temporal_mix() {
    system246_field_deinterlacer deinterlacer;
    const auto first_field = make_field({10, 10, 10, 255},
                                        {30, 30, 30, 255});
    const auto second_field = make_field({0, 0, 0, 255},
                                         {20, 20, 20, 255});
    std::vector<std::uint8_t> even_output;
    std::vector<std::uint8_t> odd_output;
    assert(deinterlacer.process(
        first_field.data(), 1, 2, false, even_output));
    assert(deinterlacer.process(
        second_field.data(), 1, 2, true, odd_output));
    // Both parities are complete progressive pictures in RRV.
    assert((at(even_output, 0) == pixel{10, 10, 10, 255}));
    assert((at(even_output, 1) == pixel{10, 10, 10, 255}));
    assert((at(odd_output, 0) == pixel{0, 0, 0, 255}));
    assert((at(odd_output, 1) == pixel{0, 0, 0, 255}));
    assert((at(odd_output, 2) == pixel{20, 20, 20, 255}));
}

void test_fields_are_not_woven_across_render_times() {
    system246_field_deinterlacer deinterlacer;
    const auto first = make_field({10, 10, 10, 255}, {30, 30, 30, 255});
    const auto second = make_field({0, 0, 0, 255}, {20, 20, 20, 255});
    std::vector<std::uint8_t> output;
    assert(deinterlacer.process(first.data(), 1, 2, false, output));
    assert(deinterlacer.process(second.data(), 1, 2, true, output));
    assert((at(output, 0) == pixel{0, 0, 0, 255}));
    assert((at(output, 1) == pixel{0, 0, 0, 255}));
    assert((at(output, 2) == pixel{20, 20, 20, 255}));
    assert((at(output, 3) == pixel{20, 20, 20, 255}));
}

void test_static_fields_have_identical_persistent_output() {
    system246_field_deinterlacer deinterlacer;
    const auto even = make_three_row_field(
        {0, 0, 0, 255}, {100, 100, 100, 255}, {100, 100, 100, 255});
    const auto odd = even;
    std::vector<std::uint8_t> output;
    std::vector<std::uint8_t> stable_even;
    std::vector<std::uint8_t> stable_odd;
    assert(deinterlacer.process(even.data(), 1, 3, false, output));
    assert(deinterlacer.process(odd.data(), 1, 3, true, output));
    assert(deinterlacer.process(even.data(), 1, 3, false, output));
    assert(deinterlacer.process(odd.data(), 1, 3, true, output));
    assert(deinterlacer.process(even.data(), 1, 3, false, stable_even));
    assert(deinterlacer.process(odd.data(), 1, 3, true, stable_odd));
    assert(stable_even == stable_odd);
    assert((at(stable_even, 1) == pixel{0, 0, 0, 255}));
    assert((at(stable_even, 2) == pixel{100, 100, 100, 255}));
}

void test_field_parity_does_not_change_geometry() {
    system246_field_deinterlacer deinterlacer;
    const auto even = make_three_row_field(
        {10, 10, 10, 255}, {90, 90, 90, 255}, {170, 170, 170, 255});
    const auto odd = even;
    std::vector<std::uint8_t> output;
    std::vector<std::uint8_t> stable_even;
    std::vector<std::uint8_t> stable_odd;
    assert(deinterlacer.process(even.data(), 1, 3, false, output));
    assert(deinterlacer.process(odd.data(), 1, 3, true, output));
    assert(deinterlacer.process(even.data(), 1, 3, false, stable_even));
    assert(deinterlacer.process(odd.data(), 1, 3, true, stable_odd));
    assert((at(stable_even, 0) == pixel{10, 10, 10, 255}));
    assert((at(stable_even, 1) == pixel{10, 10, 10, 255}));
    assert((at(stable_even, 2) == pixel{90, 90, 90, 255}));
    assert((at(stable_even, 3) == pixel{90, 90, 90, 255}));
    assert(stable_even == stable_odd);
    assert((at(stable_odd, 0) == pixel{10, 10, 10, 255}));
    assert((at(stable_odd, 1) == pixel{10, 10, 10, 255}));
    assert((at(stable_odd, 2) == pixel{90, 90, 90, 255}));
    assert((at(stable_odd, 3) == pixel{90, 90, 90, 255}));
}

void test_moving_field_uses_current_field_without_combing() {
    system246_field_deinterlacer deinterlacer;
    const auto still = make_three_row_field(
        {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255});
    std::vector<std::uint8_t> output;
    assert(deinterlacer.process(still.data(), 1, 3, false, output));
    assert(deinterlacer.process(still.data(), 1, 3, true, output));

    const auto moving = make_three_row_field(
        {0, 0, 0, 255}, {200, 200, 200, 255}, {0, 0, 0, 255});
    assert(deinterlacer.process(moving.data(), 1, 3, false, output));
    assert((at(output, 0) == pixel{0, 0, 0, 255}));
    assert((at(output, 1) == pixel{0, 0, 0, 255}));
    assert((at(output, 2) == pixel{200, 200, 200, 255}));
    assert((at(output, 3) == pixel{200, 200, 200, 255}));
    assert((at(output, 4) == pixel{0, 0, 0, 255}));
}

void test_invalid_input_is_rejected() {
    system246_field_deinterlacer deinterlacer;
    std::vector<std::uint8_t> output{1, 2, 3};
    assert(!deinterlacer.process(nullptr, 1, 2, false, output));
    assert(output.empty());
}

void test_gs_texture_inside_render_target_is_detected() {
    constexpr std::uint32_t ram_size = 4 * 1024 * 1024;
    constexpr std::uint32_t page_size = 8192;
    const auto framebuffer = GsMemoryRange::MakePageRange(
        0x100000, 640, 448, 64, 32, page_size, ram_size);
    const auto texture_inside = GsMemoryRange::MakePageRange(
        0x120000, 64, 32, 64, 32, page_size, ram_size);
    assert(GsMemoryRange::Overlaps(
        framebuffer, texture_inside, ram_size));
}

void test_gs_adjacent_ranges_do_not_overlap() {
    constexpr std::uint32_t ram_size = 4 * 1024 * 1024;
    const GsMemoryRange::Range first{0x10000, 0x2000};
    const GsMemoryRange::Range adjacent{0x12000, 0x2000};
    assert(!GsMemoryRange::Overlaps(first, adjacent, ram_size));
}

void test_gs_wrapped_ranges_overlap_at_zero() {
    constexpr std::uint32_t ram_size = 4 * 1024 * 1024;
    const GsMemoryRange::Range wrapped{ram_size - 0x1000, 0x2000};
    const GsMemoryRange::Range low_page{0, 0x1000};
    assert(GsMemoryRange::Overlaps(wrapped, low_page, ram_size));
}

} // namespace

int main() {
    test_field_is_reconstructed_at_full_height();
    test_each_field_is_expanded_without_temporal_mix();
    test_fields_are_not_woven_across_render_times();
    test_static_fields_have_identical_persistent_output();
    test_field_parity_does_not_change_geometry();
    test_moving_field_uses_current_field_without_combing();
    test_invalid_input_is_rejected();
    test_gs_texture_inside_render_target_is_detected();
    test_gs_adjacent_ranges_do_not_overlap();
    test_gs_wrapped_ranges_overlap_at_zero();
    return 0;
}

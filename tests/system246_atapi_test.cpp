#include "system246/atapi_registers.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

using system246::atapi_command_result;
using system246::atapi_opcode;
using system246::atapi_packet;
using system246::atapi_registers;

constexpr std::uint32_t sector_size = 2048;

atapi_packet make_packet(atapi_opcode opcode, std::uint32_t lba = 0,
                         std::uint16_t transfer_length = 0) {
    atapi_packet packet;
    packet.bytes[0] = static_cast<std::uint8_t>(opcode);
    packet.bytes[2] = static_cast<std::uint8_t>(lba >> 24);
    packet.bytes[3] = static_cast<std::uint8_t>(lba >> 16);
    packet.bytes[4] = static_cast<std::uint8_t>(lba >> 8);
    packet.bytes[5] = static_cast<std::uint8_t>(lba);
    packet.bytes[7] = static_cast<std::uint8_t>(transfer_length >> 8);
    packet.bytes[8] = static_cast<std::uint8_t>(transfer_length);
    return packet;
}

// A fake "disc": sector N contains sector_size bytes all equal to (N & 0xFF).
bool read_fake_sector(std::uint32_t sector, std::uint8_t* destination) {
    for (std::uint32_t i = 0; i < sector_size; ++i)
        destination[i] = static_cast<std::uint8_t>(sector & 0xFF);
    return true;
}

bool always_fails(std::uint32_t, std::uint8_t*) { return false; }

void test_test_unit_ready_reports_media_state() {
    atapi_registers no_media(sector_size, 0);
    const atapi_command_result absent =
        no_media.handle_command(make_packet(atapi_opcode::test_unit_ready),
                                nullptr);
    assert(absent.error);

    atapi_registers with_media(sector_size, 100);
    const atapi_command_result present =
        with_media.handle_command(make_packet(atapi_opcode::test_unit_ready),
                                  nullptr);
    assert(!present.error);
}

void test_inquiry_reports_cd_dvd_removable_device() {
    atapi_registers regs(sector_size, 100);
    const atapi_command_result result =
        regs.handle_command(make_packet(atapi_opcode::inquiry), nullptr);
    assert(!result.error);
    assert(result.data_length == 36);
    assert(regs.pio_buffer().size() == 36);
    assert(regs.pio_buffer()[0] == 0x05); // CD/DVD-ROM peripheral type
    assert((regs.pio_buffer()[1] & 0x80) != 0); // removable medium bit
}

void test_read_capacity_reflects_media_and_sector_size() {
    atapi_registers regs(sector_size, 100);
    const atapi_command_result result =
        regs.handle_command(make_packet(atapi_opcode::read_capacity),
                            nullptr);
    assert(!result.error);
    assert(result.data_length == 8);
    const auto& buffer = regs.pio_buffer();
    const std::uint32_t last_lba =
        (static_cast<std::uint32_t>(buffer[0]) << 24) |
        (static_cast<std::uint32_t>(buffer[1]) << 16) |
        (static_cast<std::uint32_t>(buffer[2]) << 8) | buffer[3];
    const std::uint32_t reported_sector_size =
        (static_cast<std::uint32_t>(buffer[4]) << 24) |
        (static_cast<std::uint32_t>(buffer[5]) << 16) |
        (static_cast<std::uint32_t>(buffer[6]) << 8) | buffer[7];
    assert(last_lba == 99);
    assert(reported_sector_size == sector_size);
}

void test_read_capacity_without_media_is_an_error() {
    atapi_registers regs(sector_size, 0);
    const atapi_command_result result =
        regs.handle_command(make_packet(atapi_opcode::read_capacity),
                            nullptr);
    assert(result.error);
}

void test_read_10_fills_buffer_from_sector_reader() {
    atapi_registers regs(sector_size, 100);
    const atapi_command_result result = regs.handle_command(
        make_packet(atapi_opcode::read_10, /*lba=*/5, /*count=*/2),
        read_fake_sector);
    assert(!result.error);
    assert(result.data_length == sector_size * 2);
    assert(regs.pio_buffer()[0] == 5);
    assert(regs.pio_buffer()[sector_size] == 6);
}

// This is the single most important case this whole component exists to
// get right: real System 246 disc drivers issue zero-sector READ_10
// commands continuously while streaming, as an ordinary readiness poll.
// Treating that as an error (or as "nothing happened" in a way a caller
// might mistake for a stall) breaks a working stream; it must complete
// as an ordinary success with no data.
void test_read_10_with_zero_sectors_is_not_an_error() {
    atapi_registers regs(sector_size, 100);
    const atapi_command_result result = regs.handle_command(
        make_packet(atapi_opcode::read_10, /*lba=*/5, /*count=*/0),
        always_fails);
    assert(!result.error);
    assert(result.data_length == 0);
    assert(regs.pio_buffer().empty());
}

void test_read_10_past_end_of_media_is_an_error() {
    atapi_registers regs(sector_size, 10);
    const atapi_command_result result = regs.handle_command(
        make_packet(atapi_opcode::read_10, /*lba=*/9, /*count=*/2),
        read_fake_sector);
    assert(result.error);
}

void test_read_10_sector_read_failure_is_an_error() {
    atapi_registers regs(sector_size, 100);
    const atapi_command_result result = regs.handle_command(
        make_packet(atapi_opcode::read_10, /*lba=*/0, /*count=*/1),
        always_fails);
    assert(result.error);
}

void test_read_10_without_media_is_an_error() {
    atapi_registers regs(sector_size, 0);
    const atapi_command_result result = regs.handle_command(
        make_packet(atapi_opcode::read_10, /*lba=*/0, /*count=*/1),
        read_fake_sector);
    assert(result.error);
}

void test_mode_sense_known_pages_return_expected_lengths() {
    atapi_registers regs(sector_size, 100);

    atapi_packet error_recovery_page =
        make_packet(atapi_opcode::mode_sense);
    error_recovery_page.bytes[2] = 0x01;
    const atapi_command_result page01 =
        regs.handle_command(error_recovery_page, nullptr);
    assert(!page01.error);
    assert(page01.data_length == 20);

    atapi_packet capabilities_page = make_packet(atapi_opcode::mode_sense);
    capabilities_page.bytes[2] = 0x2A;
    const atapi_command_result page2a =
        regs.handle_command(capabilities_page, nullptr);
    assert(!page2a.error);
    assert(page2a.data_length == 28);
}

void test_mode_sense_respects_allocation_length() {
    atapi_registers regs(sector_size, 100);
    atapi_packet packet = make_packet(atapi_opcode::mode_sense);
    packet.bytes[2] = 0x2A;
    packet.bytes[7] = 0;
    packet.bytes[8] = 8; // allocation length: only 8 bytes wanted
    const atapi_command_result result = regs.handle_command(packet, nullptr);
    assert(!result.error);
    assert(result.data_length == 8);
    assert(regs.pio_buffer().size() == 8);
}

void test_mode_select_completes_without_error() {
    atapi_registers regs(sector_size, 100);
    const atapi_command_result result =
        regs.handle_command(make_packet(atapi_opcode::mode_select), nullptr);
    assert(!result.error);
}

void test_streaming_and_speed_commands_are_accepted() {
    atapi_registers regs(sector_size, 100);
    assert(!regs.handle_command(make_packet(atapi_opcode::set_streaming),
                                nullptr)
                .error);
    assert(!regs.handle_command(make_packet(atapi_opcode::set_cd_speed),
                                nullptr)
                .error);
}

void test_unknown_opcode_completes_without_error() {
    atapi_registers regs(sector_size, 100);
    atapi_packet unknown{};
    unknown.bytes[0] = 0xFF;
    const atapi_command_result result = regs.handle_command(unknown, nullptr);
    assert(!result.error);
}

void test_set_media_updates_capacity() {
    atapi_registers regs(sector_size, 0);
    assert(regs.handle_command(make_packet(atapi_opcode::test_unit_ready),
                               nullptr)
               .error);
    regs.set_media(50);
    assert(!regs.handle_command(make_packet(atapi_opcode::test_unit_ready),
                                nullptr)
                .error);
}

} // namespace

int main() {
    test_test_unit_ready_reports_media_state();
    test_inquiry_reports_cd_dvd_removable_device();
    test_read_capacity_reflects_media_and_sector_size();
    test_read_capacity_without_media_is_an_error();
    test_read_10_fills_buffer_from_sector_reader();
    test_read_10_with_zero_sectors_is_not_an_error();
    test_read_10_past_end_of_media_is_an_error();
    test_read_10_sector_read_failure_is_an_error();
    test_read_10_without_media_is_an_error();
    test_mode_sense_known_pages_return_expected_lengths();
    test_mode_sense_respects_allocation_length();
    test_mode_select_completes_without_error();
    test_streaming_and_speed_commands_are_accepted();
    test_unknown_opcode_completes_without_error();
    test_set_media_updates_capacity();
    return 0;
}

#include "atapi_registers.h"

namespace system246 {

namespace {

constexpr std::size_t inquiry_response_size = 36;
constexpr std::size_t read_capacity_response_size = 8;

} // namespace

atapi_registers::atapi_registers(std::uint32_t sector_size,
                                 std::uint32_t total_sectors)
    : m_sector_size(sector_size), m_total_sectors(total_sectors) {}

void atapi_registers::set_media(std::uint32_t total_sectors) {
    m_total_sectors = total_sectors;
}

atapi_command_result atapi_registers::handle_command(
        const atapi_packet& packet,
        const atapi_sector_reader& read_sector) {
    switch (packet.opcode()) {
    case atapi_opcode::test_unit_ready:
        return handle_test_unit_ready();
    case atapi_opcode::inquiry:
        return handle_inquiry(packet);
    case atapi_opcode::read_capacity:
        return handle_read_capacity();
    case atapi_opcode::read_10:
        return handle_read_10(packet, read_sector);
    case atapi_opcode::mode_sense:
        return handle_mode_sense(packet);
    case atapi_opcode::mode_select:
        return handle_mode_select(packet);
    case atapi_opcode::set_streaming:
    case atapi_opcode::set_cd_speed:
    default:
        // set_streaming/set_cd_speed are parameter-setting commands with no
        // data returned to the host; accepting them unconditionally is
        // sufficient for a CD/DVD target that doesn't vary its read
        // behavior by speed or streaming profile. An unrecognized opcode
        // gets the same bare completion here — a real task-file model
        // would also set a sense key, which the caller-side adapter is
        // responsible for once wired to real ATA status/error registers.
        m_buffer.clear();
        return {false, 0};
    }
}

atapi_command_result atapi_registers::handle_test_unit_ready() {
    m_buffer.clear();
    if (m_total_sectors == 0) return {true, 0};
    return {false, 0};
}

atapi_command_result atapi_registers::handle_inquiry(
        const atapi_packet& packet) {
    m_buffer.assign(inquiry_response_size, 0);
    m_buffer[0] = 0x05; // peripheral device type: CD/DVD-ROM
    m_buffer[1] = 0x80; // removable medium
    m_buffer[3] = 0x21; // response data format
    m_buffer[4] = static_cast<std::uint8_t>(inquiry_response_size - 5);
    const std::uint8_t allocation_length = packet.bytes[4];
    std::size_t response_length = inquiry_response_size;
    if (allocation_length > 0 &&
        allocation_length < static_cast<std::uint8_t>(response_length))
        response_length = allocation_length;
    m_buffer.resize(response_length);
    return {false, static_cast<std::uint32_t>(response_length)};
}

atapi_command_result atapi_registers::handle_read_capacity() {
    if (m_total_sectors == 0) {
        m_buffer.clear();
        return {true, 0};
    }
    m_buffer.assign(read_capacity_response_size, 0);
    const std::uint32_t last_lba = m_total_sectors - 1;
    m_buffer[0] = static_cast<std::uint8_t>(last_lba >> 24);
    m_buffer[1] = static_cast<std::uint8_t>(last_lba >> 16);
    m_buffer[2] = static_cast<std::uint8_t>(last_lba >> 8);
    m_buffer[3] = static_cast<std::uint8_t>(last_lba);
    m_buffer[4] = static_cast<std::uint8_t>(m_sector_size >> 24);
    m_buffer[5] = static_cast<std::uint8_t>(m_sector_size >> 16);
    m_buffer[6] = static_cast<std::uint8_t>(m_sector_size >> 8);
    m_buffer[7] = static_cast<std::uint8_t>(m_sector_size);
    return {false, read_capacity_response_size};
}

atapi_command_result atapi_registers::handle_read_10(
        const atapi_packet& packet, const atapi_sector_reader& read_sector) {
    const std::uint32_t lba = packet.lba();
    const std::uint16_t sector_count = packet.transfer_length();

    // A zero-sector READ_10 is a normal, expected streaming-readiness poll,
    // not an error: real disc drivers issue these continuously while
    // streaming to ask "is the drive ready for the next real read yet?".
    // Treating this as a failure (as an earlier design iteration risked
    // doing by only reasoning about the SIF-level protocol, which never
    // surfaces this behavior at all) would make a genuine driver believe
    // the drive had faulted mid-stream.
    if (sector_count == 0) {
        m_buffer.clear();
        return {false, 0};
    }
    if (m_total_sectors == 0) {
        m_buffer.clear();
        return {true, 0};
    }

    m_buffer.assign(static_cast<std::size_t>(sector_count) * m_sector_size,
                    0);
    for (std::uint16_t index = 0; index < sector_count; ++index) {
        const std::uint32_t sector = lba + index;
        if (sector >= m_total_sectors ||
            !read_sector(sector,
                         m_buffer.data() +
                             static_cast<std::size_t>(index) *
                                 m_sector_size)) {
            m_buffer.clear();
            return {true, 0};
        }
    }
    return {false, static_cast<std::uint32_t>(m_buffer.size())};
}

atapi_command_result atapi_registers::handle_mode_sense(
        const atapi_packet& packet) {
    const std::uint8_t page_code = packet.mode_page_code();
    const std::uint16_t allocation_length = packet.allocation_length();
    std::size_t response_length = 8;
    m_buffer.assign(64, 0);
    if (page_code == 0x01) {
        // Error recovery parameters page.
        response_length = 20;
        m_buffer[0] = 0x00;
        m_buffer[1] = 0x12;
    } else if (page_code == 0x2A) {
        // CD/DVD capabilities and mechanical status page.
        response_length = 28;
        m_buffer[0] = 0x00;
        m_buffer[1] = 0x1A;
        m_buffer[8] = 0x2A;
        m_buffer[9] = 0x12;
    } else {
        m_buffer[0] = 0x00;
        m_buffer[1] = 0x06;
    }
    if (allocation_length > 0 &&
        allocation_length < static_cast<std::uint16_t>(response_length))
        response_length = allocation_length;
    m_buffer.resize(response_length);
    return {false, static_cast<std::uint32_t>(response_length)};
}

atapi_command_result atapi_registers::handle_mode_select(
        const atapi_packet& packet) {
    // The actual parameter payload arrives in a separate data-out phase
    // this pure state machine does not model; a real adapter accepts the
    // write here and completes without error, matching how a target with
    // no mode pages worth rejecting behaves.
    (void)packet;
    m_buffer.clear();
    return {false, 0};
}

} // namespace system246

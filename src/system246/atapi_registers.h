#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace system246 {

// SCSI Multimedia Commands (MMC) opcodes for the CD/DVD-ROM ATAPI command
// subset a System 246/256 disc driver uses. This is the public SCSI MMC
// command set, not anything proprietary to a specific title or copied from
// any reference implementation; the specific values are standardized.
enum class atapi_opcode : std::uint8_t {
    test_unit_ready = 0x00,
    inquiry = 0x12,
    read_capacity = 0x25,
    read_10 = 0x28,
    mode_select = 0x55,
    mode_sense = 0x5A,
    set_streaming = 0xB6,
    set_cd_speed = 0xBB,
};

// A 12-byte ATAPI command packet as delivered over the PACKET command
// protocol's data-out phase.
struct atapi_packet {
    std::array<std::uint8_t, 12> bytes{};

    atapi_opcode opcode() const {
        return static_cast<atapi_opcode>(bytes[0]);
    }
    std::uint32_t lba() const {
        return (static_cast<std::uint32_t>(bytes[2]) << 24) |
               (static_cast<std::uint32_t>(bytes[3]) << 16) |
               (static_cast<std::uint32_t>(bytes[4]) << 8) |
               static_cast<std::uint32_t>(bytes[5]);
    }
    std::uint16_t transfer_length() const {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[7]) << 8) | bytes[8]);
    }
    std::uint8_t mode_page_code() const { return bytes[2] & 0x3F; }
    std::uint16_t allocation_length() const {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[7]) << 8) | bytes[8]);
    }
};

// Reads exactly one logical sector (sized per the constructor's
// sector_size) into destination. Returns false on failure (e.g. no media,
// out-of-range sector); callers surface that as a command error.
using atapi_sector_reader =
    std::function<bool(std::uint32_t sector, std::uint8_t* destination)>;

// Result of processing one command: whether it completed as an error, and
// how many bytes are now available in pio_buffer() for the caller's PIO or
// DMA transfer path. A zero-length successful READ_10 (data_length == 0,
// error == false) is a normal, expected outcome, not a special case: real
// System 246 disc drivers issue zero-sector READ_10 commands repeatedly as
// a streaming-readiness poll, and it must not be treated as an error.
struct atapi_command_result {
    bool error{};
    std::uint32_t data_length{};
};

// Pure ATAPI command state machine: no filesystem, no host I/O, no timing.
// Deliberately kept host-independent so it is unit-testable on its own —
// a thin adapter elsewhere wires atapi_sector_reader to a real optical
// media filesystem and drives the actual IOP-side register/interrupt
// model this stands in for.
class atapi_registers {
public:
    // sector_size: bytes per logical block (2048 for CD/DVD data).
    // total_sectors: reported by READ_CAPACITY; 0 means "no media".
    atapi_registers(std::uint32_t sector_size, std::uint32_t total_sectors);

    void set_media(std::uint32_t total_sectors);

    atapi_command_result handle_command(const atapi_packet& packet,
                                        const atapi_sector_reader& read_sector);

    const std::vector<std::uint8_t>& pio_buffer() const { return m_buffer; }

private:
    atapi_command_result handle_test_unit_ready();
    atapi_command_result handle_inquiry(const atapi_packet& packet);
    atapi_command_result handle_read_capacity();
    atapi_command_result handle_read_10(const atapi_packet& packet,
                                        const atapi_sector_reader& read_sector);
    atapi_command_result handle_mode_sense(const atapi_packet& packet);
    atapi_command_result handle_mode_select(const atapi_packet& packet);

    std::uint32_t m_sector_size;
    std::uint32_t m_total_sectors;
    std::vector<std::uint8_t> m_buffer;
};

} // namespace system246

// Sega Model 1 cabinet I/O boards.
#pragma once

#include "arcade_types.h"
#include "sega/model1/model1_rom.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

enum class model1_io_board_type : uint8_t {
    standard_315_5338a,
    advanced_tmpz84c015,
};

// The main board communicates with the cabinet through an MB8421 dual-port
// RAM. This device owns everything on the other side of that RAM so Model 1
// games do not need game-specific mailbox emulation in the V60 bus.
class model1_io_board {
public:
    model1_io_board(model1_io_board_type type,
                    const std::vector<uint8_t>& firmware,
                    std::vector<uint8_t>& dual_port_ram,
                    std::array<uint8_t, 0x80>& eeprom);
    ~model1_io_board();

    model1_io_board(const model1_io_board&) = delete;
    model1_io_board& operator=(const model1_io_board&) = delete;

    void reset();
    void execute(int clocks);
    void set_inputs(model1_rom_set game, const input_state& state);
    // Light-gun cabinet input for the advanced (TMPZ84C015 / model1io2) board
    // used by Model 2 games such as Virtua Cop. Digital coin/start/trigger
    // lines plus two 10-bit gun coordinates per player, read by the firmware
    // through the 315-5338A ports and the on-board FPGA.
    void set_gun_inputs(const input_state& state);
    void set_dip_switches(const std::array<uint8_t, 3>& switches);

    bool active() const;
    uint16_t program_counter() const;
    uint64_t executed_clocks() const;

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};

// Sega Z80 Digital Sound Board used by Star Wars Arcade and later Sega boards.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class model1_dsb {
public:
    model1_dsb();
    ~model1_dsb();

    model1_dsb(const model1_dsb&) = delete;
    model1_dsb& operator=(const model1_dsb&) = delete;

    bool initialize(const std::vector<uint8_t>& cpu_rom,
                    const std::vector<uint8_t>& mpeg_rom);
    void reset();
    void receive_uart(uint8_t data);
    void execute(int clocks);

    // Adds signed stereo samples to an existing mixer accumulator.
    void render(int32_t* stereo, std::size_t frames, int output_rate,
                int gain_percent);

    bool active() const;
    uint16_t program_counter() const;
    uint64_t executed_clocks() const;
    uint64_t received_bytes() const;
    uint64_t trigger_count() const;
    bool playing() const;

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};

// ROM-backed Sega Model 1 geometry board.
#pragma once

#include "model1_tgp_device.h"
#include "mb86233.h"
#include "mb86233_native.h"

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

class model1_tgp_lle final : public model1_tgp_device {
public:
    model1_tgp_lle(const std::vector<uint8_t>& program,
                   const std::vector<uint8_t>& copro_data,
                   const std::vector<uint8_t>& lookup_tables,
                   bool legacy_external_map,
                   bool legacy_transfer_order);

    void reset() override;
    void push(uint32_t value, uint32_t source_pc) override;
    bool output_ready() const override { return !m_output_fifo.empty(); }
    uint32_t pop_output() override;
    uint16_t ram_address() const override { return m_v60_ram_address; }
    void set_ram_address(uint16_t value) override { m_v60_ram_address = value; }
    uint16_t read_ram_half(unsigned half) override;
    void write_ram_half(unsigned half, uint16_t value) override;
    bool input_pending() const override { return !m_input_fifo.empty(); }
    int execute(int cycles) override {
        return m_use_legacy_core ? m_core.execute(cycles) :
                                 m_native_core.execute(cycles);
    }
    uint16_t geometry_pc() const override {
        return m_use_legacy_core ? m_core.program_counter() :
                                 m_native_core.program_counter();
    }
    bool is_lle() const override { return true; }

private:
    static uint32_t read_word(const std::vector<uint8_t>& bytes,
                              std::size_t word);
    uint32_t data_read(uint16_t address);
    void data_write(uint16_t address, uint32_t value);
    uint32_t io_read(uint16_t address);
    void io_write(uint16_t address, uint32_t value);
    uint32_t rf_read(uint16_t address);
    void rf_write(uint16_t address, uint32_t value);
    uint32_t table_word(std::size_t index) const;
    uint32_t copro_word(std::size_t index) const;
    void stall_core();
    void set_core_gpio0(bool state);

    mb86233_core m_core;
    mb86233_native_core m_native_core;
    std::vector<uint32_t> m_program;
    std::vector<uint32_t> m_copro_data;
    std::vector<uint32_t> m_tables;
    std::array<uint32_t, 0x400> m_internal_ram{};
    std::array<uint32_t, 0x2000> m_shared_ram{};
    std::deque<uint32_t> m_input_fifo;
    std::deque<uint32_t> m_output_fifo;

    uint16_t m_v60_ram_address{};
    uint16_t m_v60_ram_latch[2]{};
    uint32_t m_external_base{};
    // The reconstructed Virtua Formula program targets the external-port
    // behavior of the original MB86233 core.  Keep every port latched: in
    // particular it computes an atan approximation itself, writes it to
    // port 0x27, then reads the hardware-adjusted result back.
    std::array<uint32_t, 0x10> m_external_ports{};
    std::array<uint32_t, 4> m_native_ram_address{};
    std::array<uint32_t, 4> m_native_atan_base{};
    uint32_t m_native_sincos_base{};
    uint32_t m_native_inv_base{};
    uint32_t m_native_isqrt_base{};
    uint32_t m_native_data_base{};
    uint32_t m_last_source_pc{};
    bool m_legacy_external_map{};
    bool m_legacy_transfer_order{};
    bool m_use_legacy_core{};
    bool m_trace{};
};

// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// Framework-free Fujitsu MB86233 core adapter.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s32 = int32_t;

inline float mb86233_u2f(u32 value) {
    union { u32 integer; float real; } bits{value};
    return bits.real;
}

inline u32 mb86233_f2u(float value) {
    union { float real; u32 integer; } bits{};
    bits.real = value;
    return bits.integer;
}

class mb86233_core {
public:
    using read_cb = std::function<u32(u16)>;
    using write_cb = std::function<void(u16, u32)>;

    enum st_flags {
        F_ZRC=0x00000001, F_ZRD=0x00000002, F_SGC=0x00000004,
        F_SGD=0x00000008, F_CPC=0x00000010, F_CPD=0x00000020,
        F_OVC=0x00000040, F_OVD=0x00000080, F_UNC=0x00000100,
        F_UND=0x00000200, F_DVZC=0x00000400, F_DVZD=0x00000800,
        F_CA=0x00001000, F_CPP=0x00002000, F_OVM=0x00004000,
        F_UNM=0x00008000, F_SIF0=0x00010000, F_SIF1=0x00020000,
        F_SOF0=0x00040000, F_PIF=0x00100000, F_POF=0x00200000,
        F_PAIF=0x00400000, F_PAOF=0x00800000, F_F0S=0x01000000,
        F_F1S=0x02000000, F_IT=0x04000000, F_ZX0=0x08000000,
        F_ZX1=0x10000000, F_ZX2=0x20000000, F_ZC0=0x40000000,
        F_ZC1=0x80000000
    };

    void set_program_callbacks(read_cb read) { m_program_read = std::move(read); }
    void set_data_callbacks(read_cb read, write_cb write) {
        m_data_read = std::move(read); m_data_write = std::move(write);
    }
    void set_io_callbacks(read_cb read, write_cb write) {
        m_io_read = std::move(read); m_io_write = std::move(write);
    }
    void set_rf_callbacks(read_cb read, write_cb write) {
        m_rf_read = std::move(read); m_rf_write = std::move(write);
    }

    void reset();
    int execute(int cycles);
    void stall() { m_stall = true; }
    void set_gpio0(bool state) { m_gpio0 = state; }
    u16 program_counter() const { return m_pc; }

private:
    read_cb m_program_read, m_data_read, m_io_read, m_rf_read;
    write_cb m_data_write, m_io_write, m_rf_write;
    int m_icount{0};

    u32 m_st{}, m_a{}, m_b{}, m_d{}, m_p{};
    u32 m_alu_stmask{}, m_alu_stset{}, m_alu_r1{}, m_alu_r2{};
    u16 m_ppc{}, m_pc{}, m_sp{}, m_b0{}, m_b1{}, m_x0{}, m_x1{};
    u16 m_i0{}, m_i1{}, m_vsmr{}, m_pcs[4]{}, m_mask{}, m_m{};
    u8 m_r{}, m_rpc{}, m_c0{}, m_c1{}, m_sft{}, m_vsm{};
    bool m_gpio0{}, m_gpio1{}, m_gpio2{}, m_gpio3{};
    bool m_stall{};

    u32 program_read(u16 address) const {
        return m_program_read ? m_program_read(address) : 0;
    }
    u32 data_read(u16 address) const {
        return m_data_read ? m_data_read(address) : 0;
    }
    void data_write(u16 address, u32 value) {
        if (m_data_write) m_data_write(address, value);
    }
    u32 io_read(u16 address) const { return m_io_read ? m_io_read(address) : 0; }
    void io_write(u16 address, u32 value) {
        if (m_io_write) m_io_write(address, value);
    }
    u32 rf_read(u16 address) const { return m_rf_read ? m_rf_read(address) : 0; }
    void rf_write(u16 address, u32 value) {
        if (m_rf_write) m_rf_write(address, value);
    }

    static s32 s24_32(u32 val);
    static u32 set_exp(u32 val, u32 exp);
    static u32 set_mant(u32 val, u32 mant);
    static u32 get_exp(u32 val);
    static u32 get_mant(u32 val);
    void testdz();
    void alu_update_st();
    void alu_pre(u32 alu);
    void alu_post(u32 alu);
    u16 ea_pre_0(u32 r);
    void ea_post_0(u32 r);
    u16 ea_pre_1(u32 r);
    void ea_post_1(u32 r);
    void pcs_push();
    void pcs_pop();
    u32 read_reg(u32 r);
    void write_reg(u32 r, u32 v);
    void write_mem_internal_1(u32 r, u32 v, bool bank);
    void write_mem_io_1(u32 r, u32 v);
    void execute_run();
};

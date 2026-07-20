// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// Framework-free Virtua Racing/Formula TGP command-level emulation.
#pragma once

#include "model1_tgp_device.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

using u16 = uint16_t;
using u32 = uint32_t;
using s16 = int16_t;

inline float model1_u2f(u32 value) {
    union { u32 integer; float real; } bits{value};
    return bits.real;
}
inline u32 model1_f2u(float value) {
    union { float real; u32 integer; } bits{value};
    return bits.integer;
}

class model1_tgp_hle final : public model1_tgp_device {
public:
    model1_tgp_hle();
    void set_copro_data(const std::vector<uint8_t>& bytes);
    void reset() override;
    void push(u32 value, u32 source_pc) override;
    bool output_ready() const override;
    u32 pop_output() override;
    u16 ram_address() const override { return m_v60_copro_ram_adr; }
    void set_ram_address(u16 value) override { m_v60_copro_ram_adr = value; }
    u16 read_ram_half(unsigned half) override;
    void write_ram_half(unsigned half, u16 value) override;
    uint64_t unimplemented_count() const override { return m_unimplemented_count; }
    u32 last_unimplemented_function() const override { return m_last_unimplemented_function; }
    u32 last_unimplemented_pc() const override { return m_last_unimplemented_pc; }

private:
    static constexpr int MAT_STACK_SIZE = 32;
    struct fifo {
        std::deque<u32> values;
        void push(u32 value) { values.push_back(value); }
        u32 pop() {
            if (values.empty()) return 0xffffffff;
            const u32 value = values.front();
            values.pop_front();
            return value;
        }
        u32 peek(std::size_t index) const { return values.at(index); }
        bool is_empty() const { return values.empty(); }
        std::size_t size() const { return values.size(); }
        void clear() { values.clear(); }
    };
    struct copro_data_view {
        std::vector<u32> words;
        const u32& as_u32(std::size_t index) const {
            static const u32 empty = 0;
            return words.empty() ? empty : words[index & (words.size() - 1)];
        }
    };

    void fifoout_push(u32 data);
    void fifoout_push_f(float data);
    u32 fifoin_pop();
    float fifoin_pop_f();
    u16 ram_get_i();
    float ram_get_f();
    void copro_hle_vf();
    void copro_hle_swa();
    u32 next_random();

    void acc_add(); void acc_div(); void acc_get(); void acc_geti();
    void acc_mul(); void acc_set(); void acc_seti(); void acc_sub();
    void anglep(); void anglev(); void car_move(); void catmull_rom();
    void clear_stack(); void col_setcirc(); void col_testpt();
    void colbox_set(); void colbox_test(); void cpa(); void distance();
    void distance3(); void f100(); void f102(); void f103(); void f42();
    void f43(); void f43_swa(); void f47(); void f49_swa();
    void f50_swa(); void f52(); void f56(); void f80(); void f89();
    void f92(); void f93(); void f94(); void f98(); void f99();
    void fadd(); void fcos_m1(); void fcosm_m1(); void fdiv();
    void fmul(); void fsin_m1(); void fsinm_m1(); void fsqrt();
    void fsub(); void ftoi(); void groundbox_set(); void groundbox_test();
    void int_normal(); void int_point(); void intercept(); void itof();
    void load_base(); void matrix_ident(); void matrix_mul();
    void matrix_pop(); void matrix_push(); void matrix_rdir();
    void matrix_read(); void matrix_readt(); void matrix_rotx();
    void matrix_roty(); void matrix_rotz(); void matrix_rtrans();
    void matrix_scale(); void matrix_sdir(); void matrix_trans();
    void matrix_unrot(); void matrix_write(); void normalize();
    void push_and_ident(); void ram_setadr(); void ram_trans();
    void track_lookup(); void track_read_info(); void track_read_quad();
    void track_read_tri(); void track_select(); void transform_point();
    void transpose(); void triangle_normal(); void vlength();
    void vmat_flatten(); void vmat_load(); void vmat_load1();
    void vmat_mul(); void vmat_read(); void vmat_restore();
    void vmat_save(); void vmat_store(); void xyz2rqf();

    using tgp_func = void (model1_tgp_hle::*)();
    struct function { tgp_func cb; int count; };
    static const function ftab_vf[];
    static const function ftab_swa[];
    static float tsin(s16 angle);
    static float tcos(s16 angle);

    fifo m_input_storage;
    fifo m_output_storage;
    fifo* m_copro_fifo_in{&m_input_storage};
    fifo* m_copro_fifo_out{&m_output_storage};
    copro_data_view m_copro_data_storage;
    copro_data_view* m_copro_data{&m_copro_data_storage};
    std::vector<u32> m_copro_ram_data;
    u32 m_pushpc{0};
    u32 m_random_state{0x1f123bb5};
    u32 m_copro_hle_active_list_pos{0};
    u32 m_copro_hle_active_list_length{0};
    u32 m_copro_hle_list_length{0};
    u16 m_copro_hle_ram_scan_adr{0};
    u16 m_v60_copro_ram_adr{0};
    u16 m_v60_copro_ram_latch[2]{};
    float m_cmat[12]{};
    float m_mat_stack[MAT_STACK_SIZE][12]{};
    float m_mat_vector[21][12]{};
    int32_t m_mat_stack_pos{0};
    float m_acc{0};
    float m_tgp_vf_xmin{0}, m_tgp_vf_xmax{0};
    float m_tgp_vf_zmin{0}, m_tgp_vf_zmax{0};
    float m_tgp_vf_ygnd{0}, m_tgp_vf_yflr{0}, m_tgp_vf_yjmp{0};
    float m_tgp_vr_circx{0}, m_tgp_vr_circy{0}, m_tgp_vr_circrad{0};
    float m_tgp_vr_cbox[12]{};
    int m_tgp_vr_select{0};
    float m_tgp_int_px{0}, m_tgp_int_py{0}, m_tgp_int_pz{0};
    u32 m_tgp_int_adr{0};
    float m_tgp_vr_base[4]{};
    int m_ccount{0};
    uint64_t m_unimplemented_count{0};
    u32 m_last_unimplemented_function{0};
    u32 m_last_unimplemented_pc{0};
};

// system22_tms320c25.h - standalone core used by the Namco C71 DSPs.
// Instruction behavior is adapted from MAME's BSD-3-Clause tms320c2x core.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

constexpr int TMS320C2X_INT0 = 0;
constexpr int TMS320C2X_INT1 = 1;
constexpr int TMS320C2X_INT2 = 2;
constexpr int TMS320C2X_TINT = 3;
constexpr int TMS320C2X_RINT = 4;
constexpr int TMS320C2X_XINT = 5;
constexpr int TMS320C2X_TRAP = 6;
constexpr int TMS320C2X_FSX = 7;

union tms_pair32 {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    struct { uint8_t l, h, h2, h3; } b;
    struct { uint16_t l, h; } w;
    struct { int8_t l, h, h2, h3; } sb;
    struct { int16_t l, h; } sw;
#else
    struct { uint8_t h3, h2, h, l; } b;
    struct { uint16_t h, l; } w;
    struct { int8_t h3, h2, h, l; } sb;
    struct { int16_t h, l; } sw;
#endif
    uint32_t d;
    int32_t sd;
};

class tms320c2x_device {
public:
    using read_callback = std::function<uint16_t(uint16_t)>;
    using write_callback = std::function<void(uint16_t, uint16_t)>;
    using pin_read_callback = std::function<uint16_t()>;
    using pin_write_callback = std::function<void(uint16_t)>;

    tms320c2x_device();

    bool load_internal_rom(const uint8_t* data, std::size_t size);
    void set_program_callbacks(read_callback read, write_callback write = {});
    void set_data_callbacks(read_callback read, write_callback write);
    void set_io_callbacks(read_callback read, write_callback write);
    void set_bio_callback(pin_read_callback read) { m_bio_in = std::move(read); }
    void set_serial_callbacks(pin_read_callback read, pin_write_callback write) {
        m_dr_in = std::move(read);
        m_dx_out = std::move(write);
    }
    void set_xf_callback(pin_write_callback write) { m_xf_out = std::move(write); }

    void reset();
    int execute(int clocks);
    void set_input(int irqline, bool asserted);

    uint16_t program_counter() const { return m_PC; }
    bool has_internal_rom() const { return m_rom_loaded; }

private:
    using opcode_func = void (tms320c2x_device::*)();
    struct tms320c2x_opcode {
        uint8_t cycles;
        opcode_func function;
    };
    static const tms320c2x_opcode s_opcode_main[256];
    static const tms320c2x_opcode s_opcode_CE_subset[256];
    static const tms320c2x_opcode s_opcode_Dx_subset[8];

    uint16_t program_read(uint16_t address) const;
    void program_write(uint16_t address, uint16_t data);
    uint16_t data_read(uint16_t address) const;
    void data_write(uint16_t address, uint16_t data);
    uint16_t io_read(uint16_t address) const;
    void io_write(uint16_t address, uint16_t data);

    read_callback m_external_program_read;
    write_callback m_external_program_write;
    read_callback m_external_data_read;
    write_callback m_external_data_write;
    read_callback m_external_io_read;
    write_callback m_external_io_write;
    pin_read_callback m_bio_in;
    pin_read_callback m_dr_in;
    pin_write_callback m_dx_out;
    pin_write_callback m_xf_out;

    std::array<uint16_t, 0x1000> m_internal_rom{};
    std::array<uint16_t, 0x100> m_b0{};
    std::array<uint16_t, 0x100> m_b1{};
    std::array<uint16_t, 0x20> m_b2{};
    bool m_b0_program{false};
    bool m_rom_loaded{false};

    static constexpr unsigned m_stack_limit = 7;
    uint16_t m_PREVPC{};
    uint16_t m_PC{};
    uint16_t m_PFC{};
    uint16_t m_STR0{};
    uint16_t m_STR1{};
    uint8_t m_IFR{};
    uint8_t m_RPTC{};
    tms_pair32 m_ACC{};
    tms_pair32 m_Preg{};
    uint16_t m_Treg{};
    uint16_t m_AR[8]{};
    uint16_t m_STACK[8]{};
    tms_pair32 m_ALU{};
    uint16_t m_drr{};
    uint16_t m_dxr{};
    uint16_t m_tim{};
    uint16_t m_prd{};
    uint16_t m_imr{};
    uint16_t m_greg{};
    uint16_t m_fixed_STR1{0x0180};
    uint8_t m_timerover{};
    tms_pair32 m_opcode{};
    bool m_idle{};
    int m_external_mem_access{};
    int m_init_load_addr{};
    int m_tms320c2x_dec_cycles{};
    tms_pair32 m_oldacc{};
    uint32_t m_memaccess{};
    int m_icount{};
    bool m_waiting_for_serial_frame{};

    uint16_t drr_r();
    void drr_w(uint16_t data);
    uint16_t dxr_r();
    void dxr_w(uint16_t data);
    uint16_t tim_r();
    void tim_w(uint16_t data);
    uint16_t prd_r();
    void prd_w(uint16_t data);
    uint16_t imr_r();
    void imr_w(uint16_t data);
    uint16_t greg_r();
    void greg_w(uint16_t data);

    void CLR0(uint16_t flag);
    void SET0(uint16_t flag);
    void CLR1(uint16_t flag);
    void SET1(uint16_t flag);
    void MODIFY_DP(int data);
    void MODIFY_PM(int data);
    void MODIFY_ARP(int data);
    uint16_t reverse_carry_add(uint16_t arg0, uint16_t arg1);
    template <bool IgnoreARPHack = false> void MODIFY_AR_ARP();
    void CALCULATE_ADD_CARRY();
    void CALCULATE_SUB_CARRY();
    void CALCULATE_ADD_OVERFLOW(int32_t addval);
    void CALCULATE_SUB_OVERFLOW(int32_t subval);
    uint16_t POP_STACK();
    void PUSH_STACK(uint16_t data);
    void SHIFT_Preg_TO_ALU();
    template <bool IgnoreARPHack = false> void GETDATA(int shift, int signext);
    void PUTDATA(uint16_t data);
    void PUTDATA_SST(uint16_t data);

    void opcodes_CE();
    void opcodes_Dx();
    void illegal();
    void abst(); void add(); void addc(); void addh(); void addk(); void adds();
    void addt(); void adlk(); void adrk(); void and_(); void andk(); void apac();
    void br(); void bacc(); void banz(); void bbnz(); void bbz(); void bc();
    void bgez(); void bgz(); void bioz(); void bit(); void bitt(); void blez();
    void blkd(); void blkp(); void blz(); void bnc(); void bnv(); void bnz();
    void bv(); void bz(); void cala(); void call(); void cmpl(); void cmpr();
    void cnfd(); void cnfp(); void conf(); void dint(); void dmov(); void eint();
    void fort(); void idle(); void in(); void lac(); void lack(); void lact();
    void lalk();
    template <unsigned N> void lar_ar();
    template <unsigned N> void lark_ar();
    void ldp(); void ldpk(); void lph(); void lrlk(); void lst(); void lst1();
    void lt(); void lta(); void ltd(); void ltp(); void lts(); void mac();
    void macd(); void mar(); void mpy(); void mpya(); void mpyk(); void mpys();
    void mpyu(); void neg(); void nop(); void norm(); void or_(); void ork();
    void out(); void pac(); void pop(); void popd(); void pshd(); void push();
    void rc(); void ret(); void rfsm(); void rhm(); void rol(); void ror();
    void rovm(); void rpt(); void rptk(); void rsxm(); void rtc(); void rtxm();
    void rxf(); void sach(); void sacl();
    template <unsigned N> void sar_ar();
    void sblk(); void sbrk_ar(); void sc(); void sfl(); void sfr(); void sfsm();
    void shm(); void sovm(); void spac(); void sph(); void spl(); void spm();
    void sqra(); void sqrs(); void sst(); void sst1(); void ssxm(); void stc();
    void stxm(); void sub(); void subb(); void subc(); void subh(); void subk();
    void subs(); void subt(); void sxf(); void tblr(); void tblw(); void trap();
    void xor_(); void xork(); void zalh(); void zalr(); void zals();

    int process_IRQs();
    void process_timer(int clocks);
    void common_reset();
};

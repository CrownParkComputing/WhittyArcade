// license:BSD-3-Clause
// copyright-holders:Steve Ellenoff, Manuel Abadia, Couriersud
/*****************************************************************************
 *
 *   mcs51.h
 *   Portable MCS-51 Family Emulator
 *
 *   Chips in the family:
 *   8051 Product Line (8031,8051,8751)
 *   8052 Product Line (8032,8052,8752)
 *
 *   Copyright Steve Ellenoff, all rights reserved.
 *
 *  This work is based on:
 *  #1) 'Intel(tm) MC51 Microcontroller Family Users Manual' and
 *  #2) 8051 simulator by Travis Marlatte
 *  #3) Portable UPI-41/8041/8741/8042/8742 emulator V0.1 by Juergen Buchmueller (MAME CORE)
 *
 * 2008, October, Couriersud
 * - Rewrite of timer, interrupt and serial code
 * - addition of CMOS features
 * - internal memory maps
 *
 * Standalone adaptation for WhittyArcade: the MAME device scaffolding
 * (address spaces, devcb, save states, debugger) is replaced with plain
 * member storage and std::function callbacks. The instruction, timer,
 * interrupt and serial implementation is the MAME code, unchanged.
 *****************************************************************************/

#ifndef WHITTY_MCS51_MCS51_H
#define WHITTY_MCS51_MCS51_H

#pragma once

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>

using offs_t = uint32_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

#ifndef WHITTY_MCS51_LINE_STATES
#define WHITTY_MCS51_LINE_STATES
enum { CLEAR_LINE = 0, ASSERT_LINE = 1 };
#endif

template <typename T, typename U>
constexpr auto BIT(T value, U bit) noexcept {
    return (value >> bit) & T(1);
}

// N-bit field starting at `bit` (the three-argument MAME BIT).
template <typename T, typename U, typename V>
constexpr auto BIT(T value, U bit, V width) noexcept {
    return (value >> bit) & ((T(1) << width) - 1);
}

inline void logerror(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
}

[[noreturn]] inline void fatalerror(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::abort();
}

enum
{
	MCS51_PC=1, MCS51_SP, MCS51_PSW, MCS51_ACC, MCS51_B, MCS51_DPTR, MCS51_DPH, MCS51_DPL, MCS51_IE, MCS51_IP,
	MCS51_P0, MCS51_P1, MCS51_P2, MCS51_P3,
	MCS51_R0, MCS51_R1, MCS51_R2, MCS51_R3, MCS51_R4, MCS51_R5, MCS51_R6, MCS51_R7, MCS51_RB,
	MCS51_TCON, MCS51_TMOD, MCS51_TL0, MCS51_TL1, MCS51_TH0, MCS51_TH1
};

enum
{
	MCS51_INT0_LINE = 0,    /* P3.2: External Interrupt 0 */
	MCS51_INT1_LINE,        /* P3.3: External Interrupt 1 */
	MCS51_T0_LINE,          /* P3.4: Timer 0 External Input */
	MCS51_T1_LINE,          /* P3.5: Timer 1 External Input */
	MCS51_T2_LINE,          /* P1.0: Timer 2 External Input */
	MCS51_T2EX_LINE,        /* P1.1: Timer 2 Capture Reload Trigger */

	DS5002FP_PFI_LINE       /* DS5002FP Power fail interrupt */
};


class mcs51_cpu_device
{
public:
	using port_read_fn = std::function<uint8_t()>;
	using port_write_fn = std::function<void(uint8_t)>;
	using bus_read_fn = std::function<uint8_t(offs_t)>;
	using bus_write_fn = std::function<void(offs_t, uint8_t)>;

	// program_width is log2 of the internal ROM (12 for an 8051/8751's
	// 4 KiB), data_width 7 for 128 bytes of internal RAM or 8 for 256.
	mcs51_cpu_device(int program_width, int data_width, uint8_t features = 0);
	virtual ~mcs51_cpu_device() = default;

	void set_port_in(unsigned port, port_read_fn fn) {
		if (port < 4) m_port_in_cb[port].fn = std::move(fn);
	}
	void set_port_out(unsigned port, port_write_fn fn) {
		if (port < 4) m_port_out_cb[port].fn = std::move(fn);
	}
	// External (MOVX) data memory.
	void set_external_memory(bus_read_fn read, bus_write_fn write) {
		m_io.read_fn = std::move(read);
		m_io.write_fn = std::move(write);
	}
	void load_rom(const uint8_t* data, uint32_t size) {
		m_rom.fill(0xff);
		if (data) {
			if (size > m_rom.size()) size = m_rom.size();
			for (uint32_t index = 0; index < size; ++index)
				m_rom[index] = data[index];
		}
	}

	void reset() { device_reset(); }
	// Run for the given number of machine cycles (12 clocks each);
	// returns the number actually executed.
	int execute(int cycles) {
		m_icount = cycles;
		execute_run();
		return cycles - m_icount;
	}
	void set_input_line(int line, int state) {
		execute_set_input(line, state);
	}
	uint16_t pc() const { return m_pc; }

	void set_port_forced_input(uint8_t port, uint8_t forced_input) { m_forced_inputs[port] = forced_input; }

protected:
	// device-level operations (former MAME device overrides)
	void device_reset();
	void execute_run();
	void execute_set_input(int inputnum, int state);

	//Internal stuff
	uint16_t  m_ppc;            //previous pc
	uint16_t  m_pc;             //current pc
	uint16_t  m_features;       //features of this cpu
	uint8_t   m_rwm;            //Signals that the current instruction is a read/write/modify instruction

	int     m_inst_cycles;        /* cycles for the current instruction */
	const uint32_t m_rom_size;    /* size (in bytes) of internal program ROM/EPROM */
	int     m_ram_mask;           /* second ram bank for indirect access available ? */
	int     m_num_interrupts;     /* number of interrupts supported */
	int     m_recalc_parity;      /* recalculate parity before next instruction */
	uint32_t  m_last_line_state;    /* last state of input lines line */
	int     m_t0_cnt;             /* number of 0->1 transitions on T0 line */
	int     m_t1_cnt;             /* number of 0->1 transitions on T1 line */
	int     m_t2_cnt;             /* number of 0->1 transitions on T2 line */
	int     m_t2ex_cnt;           /* number of 0->1 transitions on T2EX line */
	int     m_cur_irq_prio;       /* Holds value of the current IRQ Priority Level; -1 if no irq */
	uint8_t   m_irq_active;         /* mask which irq levels are serviced */
	uint8_t   m_irq_prio[8];        /* interrupt priority */

	uint8_t   m_forced_inputs[4];   /* allow read even if configured as output */

	// JB-related hacks
	uint8_t m_last_op;
	uint8_t m_last_bit;

	int     m_icount;

	struct mcs51_uart
	{
		uint8_t   data_out;       //Data to send out
		uint8_t   data_in;
		uint8_t   txbit;
		uint8_t   rxbit;
		uint8_t   rxb8;

		int     smod_div;       /* signal divided by 2^SMOD */
		int     rx_clk;         /* rx clock */
		int     tx_clk;         /* tx clock */
	} m_uart;            /* internal uart */

	/* Internal Ram: SFRs live at index 0x80-0xff, exactly the indices the
	 * SFR address constants carry. */
	std::array<uint8_t, 0x100> m_sfr_ram{};
	std::array<uint8_t, 0x100> m_scratchpad{};

	/* SFR Callbacks */
	virtual void sfr_write(size_t offset, uint8_t data);
	virtual uint8_t sfr_read(size_t offset);

	void transmit(int state);

	/* Memory spaces (standalone shims with MAME accessor names) */
	std::array<uint8_t, 0x1000> m_rom{};
	struct program_space {
		mcs51_cpu_device* cpu = nullptr;
		uint8_t read_byte(offs_t a) const {
			return (cpu->m_rom_size && a < cpu->m_rom_size)
			           ? cpu->m_rom[a & (cpu->m_rom.size() - 1)]
			           : 0xff;
		}
	} m_program;
	struct data_space {
		mcs51_cpu_device* cpu = nullptr;
		uint8_t read_byte(offs_t a) const {
			return (a & 0x100) ? cpu->m_sfr_ram[a & 0xff]
			                   : cpu->m_scratchpad[a & 0xff];
		}
		void write_byte(offs_t a, uint8_t d) const {
			if (a & 0x100)
				cpu->m_sfr_ram[a & 0xff] = d;
			else
				cpu->m_scratchpad[a & 0xff] = d;
		}
	} m_data;
	struct io_space {
		bus_read_fn read_fn;
		bus_write_fn write_fn;
		uint8_t read_byte(offs_t a) const {
			return read_fn ? read_fn(a) : 0xff;
		}
		void write_byte(offs_t a, uint8_t d) const {
			if (write_fn) write_fn(a, d);
		}
	} m_io;

	struct port_in_proxy {
		port_read_fn fn;
		uint8_t operator()() const { return fn ? fn() : 0xff; }
	};
	struct port_out_proxy {
		port_write_fn fn;
		void operator()(uint8_t value) const { if (fn) fn(value); }
	};
	std::array<port_in_proxy, 4> m_port_in_cb;
	std::array<port_out_proxy, 4> m_port_out_cb;

	/* DS5002FP (kept so shared code paths compile; unused here) */
	struct {
		uint8_t   previous_ta;        /* Previous Timed Access value */
		uint8_t   ta_window;          /* Limed Access window */
		uint8_t   range;              /* Memory Range */
		uint8_t   mcon;                   /* bootstrap loader MCON register */
		uint8_t   rpctl;                  /* bootstrap loader RPCTL register */
		uint8_t   crc;                    /* bootstrap loader CRC register */
		int32_t   rnr_delay;              /* delay before new random number available */
	} m_ds5002fp;

	uint8_t m_rtemp;

	static const uint8_t mcs51_cycles[256];

	uint8_t iram_iread(offs_t a);
	void iram_iwrite(offs_t a, uint8_t d);
	void clear_current_irq();
	uint8_t r_acc();
	uint8_t r_psw();
	virtual offs_t external_ram_iaddr(offs_t offset, offs_t mem_mask);
	uint8_t iram_read(size_t offset);
	void iram_write(size_t offset, uint8_t data);
	void push_pc();
	void pop_pc();
	void set_parity();
	uint8_t bit_address_r(uint8_t offset);
	void bit_address_w(uint8_t offset, uint8_t bit);
	void do_add_flags(uint8_t a, uint8_t data, uint8_t c);
	void do_sub_flags(uint8_t a, uint8_t data, uint8_t c);
	void transmit_receive(int source);
	void update_timer_t0(int cycles);
	void update_timer_t1(int cycles);
	void update_timer_t2(int cycles);
	void update_timers(int cycles);
	void update_serial(int source);
	void update_irq_prio(uint8_t ipl, uint8_t iph);
	void execute_op(uint8_t op);
	void check_irqs();
	void burn_cycles(int cycles);
	void acall(uint8_t r);
	void add_a_byte(uint8_t r);
	void add_a_mem(uint8_t r);
	void add_a_ir(uint8_t r);
	void add_a_r(uint8_t r);
	void addc_a_byte(uint8_t r);
	void addc_a_mem(uint8_t r);
	void addc_a_ir(uint8_t r);
	void addc_a_r(uint8_t r);
	void ajmp(uint8_t r);
	void anl_mem_a(uint8_t r);
	void anl_mem_byte(uint8_t r);
	void anl_a_byte(uint8_t r);
	void anl_a_mem(uint8_t r);
	void anl_a_ir(uint8_t r);
	void anl_a_r(uint8_t r);
	void anl_c_bitaddr(uint8_t r);
	void anl_c_nbitaddr(uint8_t r);
	void cjne_a_byte(uint8_t r);
	void cjne_a_mem(uint8_t r);
	void cjne_ir_byte(uint8_t r);
	void cjne_r_byte(uint8_t r);
	void clr_bitaddr(uint8_t r);
	void clr_c(uint8_t r);
	void clr_a(uint8_t r);
	void cpl_bitaddr(uint8_t r);
	void cpl_c(uint8_t r);
	void cpl_a(uint8_t r);
	void da_a(uint8_t r);
	void dec_a(uint8_t r);
	void dec_mem(uint8_t r);
	void dec_ir(uint8_t r);
	void dec_r(uint8_t r);
	void div_ab(uint8_t r);
	void djnz_mem(uint8_t r);
	void djnz_r(uint8_t r);
	void inc_a(uint8_t r);
	void inc_mem(uint8_t r);
	void inc_ir(uint8_t r);
	void inc_r(uint8_t r);
	void inc_dptr(uint8_t r);
	void jb(uint8_t r);
	void jbc(uint8_t r);
	void jc(uint8_t r);
	void jmp_iadptr(uint8_t r);
	void jnb(uint8_t r);
	void jnc(uint8_t r);
	void jnz(uint8_t r);
	void jz(uint8_t r);
	void lcall(uint8_t r);
	void ljmp(uint8_t r);
	void mov_a_byte(uint8_t r);
	void mov_a_mem(uint8_t r);
	void mov_a_ir(uint8_t r);
	void mov_a_r(uint8_t r);
	void mov_mem_byte(uint8_t r);
	void mov_mem_mem(uint8_t r);
	void mov_ir_byte(uint8_t r);
	void mov_r_byte(uint8_t r);
	void mov_mem_ir(uint8_t r);
	void mov_mem_r(uint8_t r);
	void mov_dptr_byte(uint8_t r);
	void mov_bitaddr_c(uint8_t r);
	void mov_ir_mem(uint8_t r);
	void mov_r_mem(uint8_t r);
	void mov_mem_a(uint8_t r);
	void mov_ir_a(uint8_t r);
	void mov_r_a(uint8_t r);
	void movc_a_iapc(uint8_t r);
	void mov_c_bitaddr(uint8_t r);
	void movc_a_iadptr(uint8_t r);
	void movx_a_idptr(uint8_t r);
	void movx_a_ir(uint8_t r);
	void movx_idptr_a(uint8_t r);
	void movx_ir_a(uint8_t r);
	void mul_ab(uint8_t r);
	void nop(uint8_t r);
	void orl_mem_a(uint8_t r);
	void orl_mem_byte(uint8_t r);
	void orl_a_byte(uint8_t r);
	void orl_a_mem(uint8_t r);
	void orl_a_ir(uint8_t r);
	void orl_a_r(uint8_t r);
	void orl_c_bitaddr(uint8_t r);
	void orl_c_nbitaddr(uint8_t r);
	void pop(uint8_t r);
	void push(uint8_t r);
	void ret(uint8_t r);
	void reti(uint8_t r);
	void rl_a(uint8_t r);
	void rlc_a(uint8_t r);
	void rr_a(uint8_t r);
	void rrc_a(uint8_t r);
	void setb_c(uint8_t r);
	void setb_bitaddr(uint8_t r);
	void sjmp(uint8_t r);
	void subb_a_byte(uint8_t r);
	void subb_a_mem(uint8_t r);
	void subb_a_ir(uint8_t r);
	void subb_a_r(uint8_t r);
	void swap_a(uint8_t r);
	void xch_a_mem(uint8_t r);
	void xch_a_ir(uint8_t r);
	void xch_a_r(uint8_t r);
	void xchd_a_ir(uint8_t r);
	void xrl_mem_a(uint8_t r);
	void xrl_mem_byte(uint8_t r);
	void xrl_a_byte(uint8_t r);
	void xrl_a_mem(uint8_t r);
	void xrl_a_ir(uint8_t r);
	void xrl_a_r(uint8_t r);
	void illegal(uint8_t r);
	uint8_t ds5002fp_protected(size_t offset, uint8_t data, uint8_t ta_mask, uint8_t mask);
};


/* The 8751: 4 KiB internal EPROM, 128 bytes internal RAM. */
class i8751_cpu : public mcs51_cpu_device
{
public:
	i8751_cpu() : mcs51_cpu_device(12, 7) {}
};

#endif // WHITTY_MCS51_MCS51_H

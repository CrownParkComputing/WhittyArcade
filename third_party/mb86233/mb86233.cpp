// license:BSD-3-Clause
// copyright-holders:Olivier Galibert

#include "mb86233.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#define logerror(...) std::fprintf(stderr, __VA_ARGS__)
#define u2f mb86233_u2f
#define f2u mb86233_f2u

/*
  Driver based on the initial reverse-engineering of Elsemi, extended,
  generalized and made to look more like a cpu since then thanks in
  part to a "manual" that barely deserves the name.

  The 86232 has 512 32-bits dwords of triple-port memory (1 write, 2
  read).  The 86233/86234 have instead two normal (1 read, 1 write,
  non-simultaneous) independant ram banks, one of 256 dwords and one
  of 512.

  The ram banks are mapped at 0x000-0x0ff and 0x200-0x3ff (proven by
  geometrizer code that clears the ram at startup).  Move and load
  instructions kind of target a specific ram, but do it by adding
  0x200 to the address on one side of the other, which can then end up
  anywhere.  In particular model1 coprocessor has the output fifo at
  0x400, which is sometimes hit by having x1 at 0x200 and using the
  automatic 0x200 adder.  Theorically external accesses to 100-1ff and
  400+ seem to be routed externally, since they're used for the fifo
  in model 1.

  The cpu can theorically work in either floating point (32-bits ieee
  flots) or fixed point (32/36/48 bits registers) modes.  All sega
  programs start by activating floating point and staying there, so
  fixed point is not implemented.

  An interrupt is used to update the rf0 (status? leds?) registers in
  the coprocessor programs.  It's on bit 1 of the mask (irq3?) and
  vector 0x004.  It's probably periodic, maybe on vblank.  Note that
  the copro programs never initialize the stack pointer.  Interrupts
  are not implemented at this point.

  The 86233 and 86234 dies are slightly different in the die shots,
  but there's no known programming-level difference at this point.
  It's unclear whether some register-file linked functionality is
  internal or external though (fifos, banking in model2/86234), so
  there may lie the actual differences.
*/


void mb86233_core::reset()
{
	m_pc = 0;
	m_ppc = 0;
	m_st = F_ZRC|F_ZRD|F_ZX0|F_ZX1|F_ZX2|F_ZC0|F_ZC1;
	m_sp = 0;

	m_a = 0;
	m_b = 0;
	m_d = 0;
	m_p = 0;
	m_r = 1;
	m_rpc = 1;
	m_c0 = 1;
	m_c1 = 1;
	m_b0 = 0;
	m_b1 = 0;
	m_x0 = 0;
	m_x1 = 0;
	m_i0 = 0;
	m_i1 = 0;
	m_sft = 0;
	m_vsm = 0;
	m_vsmr = 7;
	m_mask = 0;
	m_m = 1;

	m_alu_stmask = 0;
	m_alu_stset = 0;
	m_alu_r1 = 0;
	m_alu_r2 = 0;

	std::fill(std::begin(m_pcs), std::end(m_pcs), 0);

	m_stall = false;
	m_gpio0 = m_gpio1 = m_gpio2 = m_gpio3 = false;
}

s32 mb86233_core::s24_32(u32 val)
{
	if(val & 0x00800000)
		return val | 0xff000000;
	else
		return val & 0x00ffffff;
}

u32 mb86233_core::set_exp(u32 val, u32 exp)
{
	return (val & 0x807fffff) | ((exp & 0xff) << 23);
}

u32 mb86233_core::set_mant(u32 val, u32 mant)
{
	return (val & 0x07f800000) | ((mant & 0x00800000) << 8) | (mant & 0x007fffff);
}

u32 mb86233_core::get_exp(u32 val)
{
	return (val >> 23) & 0xff;
}

u32 mb86233_core::get_mant(u32 val)
{
	return val & 0x80000000 ? val | 0x7f800000 : val & 0x807fffff;
}

void mb86233_core::pcs_push()
{
	for(unsigned int i=3; i; i--)
		m_pcs[i] = m_pcs[i-1];
	m_pcs[0] = m_pc;
}

void mb86233_core::pcs_pop()
{
	m_pc = m_pcs[0];
	for(unsigned int i=0; i != 3; i++)
		m_pcs[i] = m_pcs[i+1];
}

void mb86233_core::testdz()
{
	if(m_d)
		m_st &= ~F_ZRD;
	else
		m_st |= F_ZRD;
	if(m_d & 0x80000000)
		m_st |= F_SGD;
	else
		m_st &= ~F_SGD;
}

void mb86233_core::alu_pre(u32 alu)
{
	switch(alu) {
	case 0x00: break; // no alu

	case 0x01: {
		// andd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d & m_a;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x02: {
		// orad
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d | m_a;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x03: {
		// eord
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d ^ m_a;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x04: {
		// notd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = ~m_d;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x05: {
		// fcpd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		u32 r = f2u(u2f(m_d) - u2f(m_a));
		m_alu_stset = r ? r & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x06: {
		// fmad
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_a));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x07: {
		// fsbd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) - u2f(m_a));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x08: {
		// fml
		m_alu_stmask = 0;
		m_alu_r1 = f2u(u2f(m_a) * u2f(m_b));
		m_alu_stset = 0;
		break;
	}

	case 0x09: {
		// fmsd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_p));
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x0a: {
		// fmrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) - u2f(m_p));
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x0b: {
		// fabd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d & 0x7fffffff;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x0c: {
		// fsmd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_p));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x0d: {
		// fspd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_p;
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x0e: {
		// cxfd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(s32(m_d));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x0f: {
		// cfxd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		switch((m_m >> 1) & 3) {
		case 0: m_alu_r1 = s32(roundf(u2f(m_d))); break;
		case 1: m_alu_r1 = s32(ceilf(u2f(m_d))); break;
		case 2: m_alu_r1 = s32(floorf(u2f(m_d))); break;
		case 3: m_alu_r1 = s32(u2f(m_d)); break;
		}
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x10: {
		// fdvd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) / u2f(m_a));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x11: {
		// fned
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d ? m_d ^ 0x80000000 : 0;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x13: {
		// d = b + a
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_b) + u2f(m_a));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x14: {
		// d = b - a
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_b) - u2f(m_a));
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x16: {
		// lsrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d >> m_sft;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x17: {
		// lsld
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d << m_sft;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x18: {
		// asrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = s32(m_d) >> m_sft;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x19: {
		// asld
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = s32(m_d) << m_sft;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x1a: {
		// addd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d + m_a;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	case 0x1b: {
		// subd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d - m_a;
		m_alu_stset = m_alu_r1 ? m_alu_r1 & 0x80000000 ? F_SGD : 0 : F_ZRD;
		break;
	}

	default:
		logerror("unhandled alu pre %02x\n", alu);
		break;
	}
}

void mb86233_core::alu_update_st()
{
	m_st = (m_st & ~m_alu_stmask) | m_alu_stset;
}

void mb86233_core::alu_post(u32 alu)
{
	switch(alu) {
	case 0x00: break; // no alu

	case 0x05:
		// flags only
		alu_update_st();
		break;

	case 0x01: case 0x02: case 0x03: case 0x04:
	case 0x06: case 0x07: case 0x0b: case 0x0c:
	case 0x0e: case 0x0f: case 0x10: case 0x11:
	case 0x13: case 0x14: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1a: case 0x1b:
		// d update
		m_d = m_alu_r1;
		alu_update_st();
		break;

	case 0x08:
		// p update
		m_p = m_alu_r1;
		break;

	case 0x09: case 0x0a: case 0xd:
		// d, p update
		m_d = m_alu_r1;
		m_p = m_alu_r2;
		alu_update_st();
		break;

	default:
		logerror("unhandled alu post %02x\n", alu);
		break;
	}
}

u16 mb86233_core::ea_pre_0(u32 r)
{
	switch(r & 0x180) {
	case 0x000: return r & 0x7f;
	case 0x080: case 0x100: return (r & 0x7f) + m_b0 + m_x0;
	case 0x180: {
		switch(r & 0x60) {
		case 0x00: return m_b0 + m_x0;
		case 0x20: return m_x0;
		case 0x40: return m_b0 + (m_x0 & m_vsmr);
		case 0x60: return m_x0 & m_vsmr;
		}
	}
	}
	return 0;
}

void mb86233_core::ea_post_0(u32 r)
{
	if(!(r & 0x100))
		return;
	if(!(r & 0x080))
		m_x0 += m_i0;
	else {
		if(r & 0x10)
			m_x0 += (r & 0xf) - 0x10;
		else
			m_x0 += r & 0xf;
	}
}

u16 mb86233_core::ea_pre_1(u32 r)
{
	switch(r & 0x180) {
	case 0x000: return r & 0x7f;
	case 0x080: case 0x100: return (r & 0x7f) + m_b1 + m_x1;
	case 0x180: {
		switch(r & 0x60) {
		case 0x00: return m_b1 + m_x1;
		case 0x20: return m_x1;
		case 0x40: return m_b1 + (m_x1 & m_vsmr);
		case 0x60: return m_x1 & m_vsmr;
		}
	}
	}
	return 0;
}

void mb86233_core::ea_post_1(u32 r)
{
	if(!(r & 0x100))
		return;
	if(!(r & 0x080))
		m_x1 += m_i1;
	else {
		if(r & 0x10)
			m_x1 += (r & 0xf) - 0x10;
		else
			m_x1 += r & 0xf;
	}
}

u32 mb86233_core::read_reg(u32 r)
{
	r &= 0x3f;
	if(r >= 0x20 && r < 0x30)
		return rf_read(r & 0x1f);
	switch(r) {
	case 0x00: return m_b0;
	case 0x01: return m_b1;
	case 0x02: return m_x0;
	case 0x03: return m_x1;

	case 0x0c: return m_c0;
	case 0x0d: return m_c1;

	case 0x10: return m_a;
	case 0x11: return get_exp(m_a);
	case 0x12: return get_mant(m_a);
	case 0x13: return m_b;
	case 0x14: return get_exp(m_b);
	case 0x15: return get_mant(m_b);
	case 0x19: return m_d;
		/* c */
	case 0x1a: return get_exp(m_d);
	case 0x1b: return get_mant(m_d);
	case 0x1c: return m_p;
	case 0x1d: return get_exp(m_p);
	case 0x1e: return get_mant(m_p);
	case 0x1f: return m_sft;

	case 0x34: return m_rpc;

	default:
		logerror("unimplemented read_reg(%02x)\n", r);
		return 0;
	}
}

void mb86233_core::write_reg(u32 r, u32 v)
{
	r &= 0x3f;
	if(r >= 0x20 && r < 0x30) {
		rf_write(r & 0x1f, v);
		return;
	}
	switch(r) {
	case 0x00: m_b0 = v; break;
	case 0x01: m_b1 = v; break;
	case 0x02: m_x0 = v; break;
	case 0x03: m_x1 = v; break;

	case 0x05: m_i0 = v; break;
	case 0x06: m_i1 = v; break;

	case 0x08: m_sp = v; break;

	case 0x0a: m_vsm = v & 7; m_vsmr = (8 << m_vsm) - 1; break;

	case 0x0c:
		m_c0 = v;
		if(m_c0 == 1)
			m_st |= F_ZC0;
		else
			m_st &= ~F_ZC0;
		break;

	case 0x0d:
		m_c1 = v;
		if(m_c1 == 1)
			m_st |= F_ZC1;
		else
			m_st &= ~F_ZC1;
		break;

	case 0x0f: break;

	case 0x10: m_a = v; break;
	case 0x11: m_a = set_exp(m_a, v); break;
	case 0x12: m_a = set_mant(m_a, v); break;
	case 0x13: m_b = v; break;
	case 0x14: m_b = set_exp(m_b, v); break;
	case 0x15: m_b = set_mant(m_b, v); break;
		/* c */
	case 0x19: m_d = v; testdz(); break;
	case 0x1a: m_d = set_exp(m_d, v); testdz(); break;
	case 0x1b: m_d = set_mant(m_d, v); testdz(); break;
	case 0x1c: m_p = v; break;
	case 0x1d: m_p = set_exp(m_p, v); break;
	case 0x1e: m_p = set_mant(m_p, v); break;
	case 0x1f: m_sft = v; break;

	case 0x34: m_rpc = v; break;
	case 0x3c: m_mask = v; break;

	default:
		logerror("unimplemented write_reg(%02x, %08x)\n", r, v);
		break;
	}
}

void mb86233_core::write_mem_internal_1(u32 r, u32 v, bool bank)
{
	u16 ea = ea_pre_1(r);
	if(bank)
		ea += 0x200;
	data_write(ea, v);
	ea_post_1(r);
}

void mb86233_core::write_mem_io_1(u32 r, u32 v)
{
	u16 ea = ea_pre_1(r);
	io_write(ea, v);
	ea_post_1(r);
}

int mb86233_core::execute(int cycles)
{
	if (cycles <= 0) return 0;
	m_icount = cycles;
	execute_run();
	return cycles - m_icount;
}

void mb86233_core::execute_run()
{
	while(m_icount > 0) {
		m_ppc = m_pc;
		// Standalone core: debugger hook omitted.
		u32 opcode = program_read(m_pc++);

		switch((opcode >> 26) & 0x3f) {
		case 0x00: {
			// lab
			u32 r1 = opcode & 0x1ff;
			u32 r2 = (opcode >> 9) & 0x1ff;
			u32 alu = (opcode >> 21) & 0x1f;
			u32 op = (opcode >> 18) & 0x7;

			alu_pre(alu);

			switch(op) {
			case 0: case 1: {
				// lab mem, mem (e)

				u32 ea1 = ea_pre_0(r1);
				u32 v1 = data_read(ea1);
				if(m_stall) goto do_stall;

				u32 ea2 = ea_pre_1(r2);
				u32 v2 = io_read(ea2);
				if(m_stall) goto do_stall;

				ea_post_0(r1);
				ea_post_1(r2);

				m_a = v1;
				m_b = v2;
				break;
			}

			case 3: {
				// lab mem, mem + 0x200

				u32 ea1 = ea_pre_0(r1);
				u32 v1 = data_read(ea1);
				if(m_stall) goto do_stall;

				u32 ea2 = ea_pre_1(r2) + 0x200;
				u32 v2 = data_read(ea2);
				if(m_stall) goto do_stall;

				ea_post_0(r1);
				ea_post_1(r2);

				m_a = v1;
				m_b = v2;
				break;
			}

			case 4: {
				// lab mem + 0x200, mem

				u32 ea1 = ea_pre_0(r1) + 0x200;
				u32 v1 = data_read(ea1);
				if(m_stall) goto do_stall;

				u32 ea2 = ea_pre_1(r2);
				u32 v2 = data_read(ea2);
				if(m_stall) goto do_stall;

				ea_post_0(r1);
				ea_post_1(r2);

				m_a = v1;
				m_b = v2;
				break;
			}

			default:
				logerror("unhandled lab subop %x\n", op);
				logerror("%x\n", m_ppc);
				break;

			}

			alu_post(alu);
			break;
		}


		case 0x07: {
			// ld / mov
			u32 r1 = opcode & 0x1ff;
			u32 r2 = (opcode >> 9) & 0x1ff;
			u32 alu = (opcode >> 21) & 0x1f;
			u32 op = (opcode >> 18) & 0x7;

			alu_pre(alu);

			switch(op) {
			case 0: {
				// mov mem, mem (e)
				u32 ea = ea_pre_0(r1);
				u32 v = data_read(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				write_mem_io_1(r2, v);
				break;
			}

			case 1: {
				// mov mem + 0x200, mem (e)
				u32 ea = ea_pre_0(r1) + 0x200*0;
				u32 v = data_read(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				write_mem_io_1(r2, v);
				break;
			}

			case 2: {
				// mov mem (e), mem
				u32 ea = ea_pre_0(r1);
				u32 v = io_read(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				write_mem_internal_1(r2, v, false);
				break;
			}

			case 3: {
				// mov mem, mem + 0x200
				u32 ea = ea_pre_0(r1);
				u32 v = data_read(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				write_mem_internal_1(r2, v, true);
				break;
			}

			case 4: {
				// mov mem + 0x200, mem
				u32 ea = ea_pre_0(r1) + 0x200;
				u32 v = data_read(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				write_mem_internal_1(r2, v, false);
				break;
			}

			case 5: {
				// mov mem (o), mem
				u32 ea = ea_pre_0(r1);
				u32 v = program_read(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				write_mem_internal_1(r2, v, false);
				break;
			}

			case 7: {
				switch(r2 >> 6) {
				case 0: {
					// mov reg, mem
					u32 v = read_reg(r2);
					if(m_stall) goto do_stall;
					write_mem_internal_1(r1, v, false);
					break;
				}

				case 1: {
					// mov reg, mem (e)
					u32 v = read_reg(r2);
					if(m_stall) goto do_stall;
					write_mem_io_1(r1, v);
					break;
				}

				case 2: {
					// mov mem + 0x200, reg
					u32 ea = ea_pre_1(r1) + 0x200;
					u32 v = data_read(ea);
					if(m_stall) goto do_stall;
					ea_post_1(r1);
					write_reg(r2, v);
					break;
				}

				case 3: {
					// mov mem, reg
					u32 ea = ea_pre_1(r1);
					u32 v = data_read(ea);
					if(m_stall) goto do_stall;
					ea_post_1(r1);
					write_reg(r2, v);
					break;
				}

				case 4: {
					// mov mem (e), reg
					u32 ea = ea_pre_1(r1);
					u32 v = io_read(ea);
					if(m_stall) goto do_stall;
					ea_post_1(r1);
					write_reg(r2, v);
					break;
				}

				case 5: {
					// mov mem (o), reg
					u32 ea = ea_pre_0(r1);
					u32 v = program_read(ea);
					if(m_stall) goto do_stall;
					ea_post_0(r1);
					write_reg(r2, v);
					break;
				}

				case 6: {
					// mov reg, reg
					u32 v = read_reg(r1);
					if(m_stall) goto do_stall;
					write_reg(r2, v);
					break;
				}

				default:
					logerror("unhandler ld/mov subop 7/%x\n", r2 >> 6);
					break;
				}
				break;
			}

			default:
				logerror("unhandler ld/mov subop %x\n", op);
				break;
			}

			alu_post(alu);
			break;
		}

		case 0x0d: {
			// stm/clm
			u32 sub2 = (opcode >> 17) & 7;

			// Theorically has restricted alu too

			switch(sub2) {
			case 5:
				// stmh
				// bit 0 = floating point
				// bit 1-2 = rounding mode
				m_m = opcode;
				break;

			default:
				logerror("unimplemented opcode 0d/%x\n", sub2);
				break;
			}
			break;
		}

		case 0x0e: {
			// lipl / lia / lib / lid
			switch((opcode >> 24) & 0x3) {
			case 0:
				m_p = (m_p & 0xffffff000000) | (opcode & 0xffffff);
				break;
			case 1:
				m_a = s24_32(opcode);
				break;
			case 2:
				m_b = s24_32(opcode);
				break;
			case 3:
				m_d = s24_32(opcode);
				testdz();
				break;
			}
			break;
		}

		case 0x0f: {
			// rep/clr0/clr1/set
			u32 alu = (opcode >> 20) & 0x1f;
			u32 sub2 = (opcode >> 17) & 7;

			alu_pre(alu);

			switch(sub2) {
			case 0:
				// clr0
				if(opcode & 0x0004) m_a = 0;
				if(opcode & 0x0008) m_b = 0;
				if(opcode & 0x0010) m_d = 0;
				break;

			case 1:
				// clr1 - flags mapping unknown
				break;

			case 2: {
				// rep
				u8 r = opcode & 0x8000 ? read_reg(opcode) : opcode;
				if(m_stall) goto do_stall;
				m_r = r;
				goto rep_start;
			}

			case 3:
				// set - flags mapping unknown
				// 0800 = enable interrupt flag
				break;

			default:
				logerror("unimplemented opcode 0f/%x\n", sub2);
				break;
			}

			alu_post(alu);
			break;
		}

		case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f: {
			// ldi
			write_reg(opcode >> 24, s24_32(opcode));
			break;
		}

		case 0x2f: case 0x3f: {
			// Conditional branch of every kind
			u32 cond = ( opcode >> 20 ) & 0x1f;
			u32 subtype = ( opcode >> 17 ) & 7;
			u32 data = opcode & 0xffff;
			bool invert = opcode & 0x40000000;

			bool cond_passed = false;

			switch(cond) {
			case 0x00: // zrd - d zero
				cond_passed = m_st & F_ZRD;
				break;

			case 0x01: // ged - d >= 0
				cond_passed = !(m_st & F_SGD);
				break;

			case 0x02: // led - d <= 0
				cond_passed = m_st & (F_ZRD | F_SGD);
				break;

			case 0x0a: // gpio0
				cond_passed = m_gpio0;
				break;

			case 0x0b: // gpio1
				cond_passed = m_gpio1;
				break;

			case 0x0c: // gpio2
				cond_passed = m_gpio2;
				break;

			case 0x10: // zc0 - c0 == 1
				cond_passed = !(m_st & F_ZC0);
				break;

			case 0x11: // zc1 - c1 == 1
				cond_passed = !(m_st & F_ZC1);
				break;

			case 0x12: // gpio3
				cond_passed = m_gpio3;
				break;

			case 0x16: // alw - always
				cond_passed = true;
				break;

			default:
				logerror("unimplemented condition %x\n", cond);
				break;
			}
			if(invert)
				cond_passed = !cond_passed;

			if(cond_passed) {
				switch(subtype) {
				case 0: // brif #adr
					m_pc = data;
					break;

				case 1: // brul
					if(opcode & 0x4000) {
						// brul reg
						u32 v = read_reg(opcode);
						if(m_stall) goto do_stall;
						m_pc = v;
					} else {
						// brul adr
						u32 ea = ea_pre_0(opcode);
						u32 v = data_read(ea);
						if(m_stall) goto do_stall;
						ea_post_0(opcode);
						m_pc = v;
					}
					break;

				case 2: // bsif #adr
					pcs_push();
					m_pc = data;
					break;

				case 3: // bsul
					if(opcode & 0x4000) {
						// bsul reg
						u32 v = read_reg(opcode);
						if(m_stall) goto do_stall;
						pcs_push();
						m_pc = v;
					} else {
						// bsul adr
						u32 ea = ea_pre_0(opcode);
						u32 v = data_read(ea);
						if(m_stall) goto do_stall;
						ea_post_0(opcode);
						pcs_push();
						m_pc = v;
					}
					break;

				case 5: // rtif #adr
					pcs_pop();
					break;

				case 6: { // ldif adr, rn
					u32 ea = ea_pre_0(opcode);
					u32 v = data_read(ea);
					if(m_stall) goto do_stall;
					ea_post_0(opcode);
					write_reg(opcode >> 9, v);
					break;
				}

				default:
					logerror("unimplemented branch subtype %x\n", subtype);
					break;
				}
			}

			if(subtype < 2)
				switch(cond) {
				case 0x10:
					if(m_c0 != 1) {
						m_c0 --;
						if(m_c0 == 1)
							m_st |= F_ZC0;
					}
					break;

				case 0x11:
					if(m_c1 != 1) {
						m_c1 --;
						if(m_c1 == 1)
							m_st |= F_ZC1;
					}
				break;
				}

			break;
		}

		default:
			logerror("unimplemented opcode type %02x\n", (opcode >> 26) & 0x3f);
			break;
		}

		if(m_r != 1) {
			m_pc = m_ppc;
			m_r --;
		}

	rep_start:
		if (false) {
		do_stall:
			// Retry the instruction when the host supplies FIFO data. Ending
			// this slice avoids burning the remaining budget on empty retries.
			m_pc = m_ppc;
			m_stall = false;
			m_icount = 0;
			return;
		}
		m_icount--;
	}
}

// license:BSD-3-Clause
// copyright-holders:Juergen Buchmueller
/*****************************************************************************
 *
 *   z80.cpp
 *   Portable Z80 emulator V3.9
 *
 *   TODO:
 *    - Interrupt mode 0 should be able to execute arbitrary opcodes
 *    - If LD A,I or LD A,R is interrupted, P/V flag gets reset, even if IFF2
 *      was set before this instruction (implemented, but not enabled: we need
 *      document Z80 types first, see below)
 *    - Ideally, the tiny differences between Z80 types should be supported,
 *      currently known differences:
 *       - LD A,I/R P/V flag reset glitch is fixed on CMOS Z80
 *       - OUT (C),0 outputs 0 on NMOS Z80, $FF on CMOS Z80
 *       - SCF/CCF X/Y flags is ((flags | A) & 0x28) on SGS/SHARP/ZiLOG NMOS Z80,
 *         (flags & A & 0x28).
 *         However, recent findings say that SCF/CCF X/Y results depend on whether
 *         or not the previous instruction touched the flag register.
 *      This Z80 emulator assumes a ZiLOG NMOS model.
 *
 *   Changes in 0.243:
 *    Foundation for M cycles emulation. Currently we preserve cc_* tables with total timings.
 *    execute_run() behavior (simplified) ...
 *   Changes in 3.9:
 *    - Fixed cycle counts for LD IYL/IXL/IYH/IXH,n [Marshmellow]
 *    - Fixed X/Y flags in CCF/SCF/BIT, ZEXALL is happy now [hap]
 *    - Simplified DAA, renamed MEMPTR (3.8) to WZ, added TODO [hap]
 *    - Fixed IM2 interrupt cycles [eke]
 *   Changes in 3.8 [Miodrag Milanovic]:
 *    - Added MEMPTR register (according to informations provided
 *      by Vladimir Kladov
 *    - BIT n,(HL) now return valid values due to use of MEMPTR
 *    - Fixed BIT 6,(XY+o) undocumented instructions
 *   Changes in 3.7 [Aaron Giles]:
 *    - Changed NMI handling. NMIs are now latched in set_irq_state
 *      but are not taken there. Instead they are taken at the start of the
 *      execute loop.
 *    - Changed IRQ handling. IRQ state is set in set_irq_state but not taken
 *      except during the inner execute loop.
 *    - Removed x86 assembly hacks and obsolete timing loop catchers.
 *   Changes in 3.6:
 *    - Got rid of the code that would inexactly emulate a Z80, i.e. removed
 *      all the #if Z80_EXACT #else branches.
 *    - Removed leading underscores from local register name shortcuts as
 *      this violates the C99 standard.
 *    - Renamed the registers inside the Z80 context to lower case to avoid
 *      ambiguities (shortcuts would have had the same names as the fields
 *      of the structure).
 *   Changes in 3.5:
 *    - Implemented OTIR, INIR, etc. without look-up table for PF flag.
 *      [Ramsoft, Sean Young]
 *   Changes in 3.4:
 *    - Removed Z80-MSX specific code as it's not needed any more.
 *    - Implemented DAA without look-up table [Ramsoft, Sean Young]
 *   Changes in 3.3:
 *    - Fixed undocumented flags XF & YF in the non-asm versions of CP,
 *      and all the 16 bit arithmetic instructions. [Sean Young]
 *   Changes in 3.2:
 *    - Fixed undocumented flags XF & YF of RRCA, and CF and HF of
 *      INI/IND/OUTI/OUTD/INIR/INDR/OTIR/OTDR [Sean Young]
 *   Changes in 3.1:
 *    - removed the REPEAT_AT_ONCE execution of LDIR/CPIR etc. opcodes
 *      for readabilities sake and because the implementation was buggy
 *      (and i was not able to find the difference)
 *   Changes in 3.0:
 *    - 'finished' switch to dynamically overrideable cycle count tables
 *   Changes in 2.9:
 *    - added methods to access and override the cycle count tables
 *    - fixed handling and timing of multiple DD/FD prefixed opcodes
 *   Changes in 2.8:
 *    - OUTI/OUTD/OTIR/OTDR also pre-decrement the B register now.
 *      This was wrong because of a bug fix on the wrong side
 *      (astrocade sound driver).
 *   Changes in 2.7:
 *    - removed z80_vm specific code, it's not needed (and never was).
 *   Changes in 2.6:
 *    - BUSY_LOOP_HACKS needed to call change_pc() earlier, before
 *      checking the opcodes at the new address, because otherwise they
 *      might access the old (wrong or even nullptr) banked memory region.
 *      Thanks to Sean Young for finding this nasty bug.
 *   Changes in 2.5:
 *    - Burning cycles always adjusts the ICount by a multiple of 4.
 *    - In REPEAT_AT_ONCE cases the r register wasn't incremented twice
 *      per repetition as it should have been. Those repeated opcodes
 *      could also underflow the ICount.
 *    - Simplified TIME_LOOP_HACKS for BC and added two more for DE + HL
 *      timing loops. i think those hacks weren't endian safe before too.
 *   Changes in 2.4:
 *    - z80_reset zaps the entire context, sets IX and IY to 0xffff(!) and
 *      sets the Z flag. With these changes the Tehkan World Cup driver
 *      _seems_ to work again.
 *   Changes in 2.3:
 *    - External termination of the execution loop calls z80_burn() and
 *      z80_vm_burn() to burn an amount of cycles (r adjustment)
 *    - Shortcuts which burn CPU cycles (BUSY_LOOP_HACKS and TIME_LOOP_HACKS)
 *      now also adjust the r register depending on the skipped opcodes.
 *   Changes in 2.2:
 *    - Fixed bugs in CPL, SCF and CCF instructions flag handling.
 *    - Changed variable ea and arg16() function to u32; this
 *      produces slightly more efficient code.
 *    - The DD/FD XY CB opcodes where XY is 40-7F and Y is not 6/E
 *      are changed to calls to the X6/XE opcodes to reduce object size.
 *      They're hardly ever used so this should not yield a speed penalty.
 *   New in 2.0:
 *    - Optional more exact Z80 emulation (#define Z80_EXACT 1) according
 *      to a detailed description by Sean Young which can be found at:
 *      http://www.msxnet.org/tech/z80-documented.pdf
 *****************************************************************************/

#include "emu.h"
#include "z80.h"
#include "z80dasm.h"

#include "z80.inc"

static bool tables_initialised = false;
std::unique_ptr<u8[]> z80_device::SZ = std::make_unique<u8[]>(0x100);       // zero and sign flags
std::unique_ptr<u8[]> z80_device::SZ_BIT = std::make_unique<u8[]>(0x100);   // zero, sign and parity/overflow (=zero) flags for BIT opcode
std::unique_ptr<u8[]> z80_device::SZP = std::make_unique<u8[]>(0x100);      // zero, sign and parity flags
std::unique_ptr<u8[]> z80_device::SZHV_inc = std::make_unique<u8[]>(0x100); // zero, sign, half carry and overflow flags INC r8
std::unique_ptr<u8[]> z80_device::SZHV_dec = std::make_unique<u8[]>(0x100); // zero, sign, half carry and overflow flags DEC r8

std::unique_ptr<u8[]> z80_device::SZHVC_add = std::make_unique<u8[]>(2 * 0x100 * 0x100);
std::unique_ptr<u8[]> z80_device::SZHVC_sub = std::make_unique<u8[]>(2 * 0x100 * 0x100);


/***************************************************************
 * Enter halt state; write 1 to callback on first execution
 ***************************************************************/
void z80_device::halt()
{
	if (!m_halt)
	{
		m_halt = 1;
		m_halt_cb(1);
	}
}

/***************************************************************
 * Leave halt state; write 0 to callback
 ***************************************************************/
void z80_device::leave_halt()
{
	if (m_halt)
	{
		m_halt = 0;
		m_halt_cb(0);
	}
}

/***************************************************************
 * Read a byte from given memory location
 ***************************************************************/
u8 z80_device::data_read(u16 addr)
{
	return m_data.read_interruptible(translate_memory_address(addr));
}

/***************************************************************
 * Write a byte to given memory location
 ***************************************************************/
void z80_device::data_write(u16 addr, u8 value)
{
	m_data.write_interruptible(translate_memory_address((u32)addr), value);
}

/***************************************************************
 * rop() is identical to rm() except it is used for
 * reading opcodes. In case of system with memory mapped I/O,
 * this function can be used to greatly speed up emulation
 ***************************************************************/
u8 z80_device::opcode_read()
{
	return m_opcodes.read_byte(translate_memory_address(PCD));
}

/****************************************************************
 * arg() is identical to rop() except it is used
 * for reading opcode arguments. This difference can be used to
 * support systems that use different encoding mechanisms for
 * opcodes and opcode arguments
 * out: TDAT8
 ***************************************************************/
u8 z80_device::arg_read()
{
	return m_args.read_byte(translate_memory_address(PCD));
}

/***************************************************************
 * INC  r8
 ***************************************************************/
void z80_device::inc(u8 &r)
{
	++r;
	set_f((F & CF) | SZHV_inc[r]);
}

/***************************************************************
 * DEC  r8
 ***************************************************************/
void z80_device::dec(u8 &r)
{
	--r;
	set_f((F & CF) | SZHV_dec[r]);
}

/***************************************************************
 * RLCA
 ***************************************************************/
void z80_device::rlca()
{
	A = (A << 1) | (A >> 7);
	set_f((F & (SF | ZF | PF)) | (A & (YF | XF | CF)));
}

/***************************************************************
 * RRCA
 ***************************************************************/
void z80_device::rrca()
{
	set_f((F & (SF | ZF | PF)) | (A & CF));
	A = (A >> 1) | (A << 7);
	F |= (A & (YF | XF));
}

/***************************************************************
 * RLA
 ***************************************************************/
void z80_device::rla()
{
	u8 res = (A << 1) | (F & CF);
	u8 c = (A & 0x80) ? CF : 0;
	set_f((F & (SF | ZF | PF)) | c | (res & (YF | XF)));
	A = res;
}

/***************************************************************
 * RRA
 ***************************************************************/
void z80_device::rra()
{
	u8 res = (A >> 1) | (F << 7);
	u8 c = (A & 0x01) ? CF : 0;
	set_f((F & (SF | ZF | PF)) | c | (res & (YF | XF)));
	A = res;
}

/***************************************************************
 * ADD  A,n
 ***************************************************************/
void z80_device::add_a(u8 value)
{
	u32 ah = AFD & 0xff00;
	u32 res = (u8)((ah >> 8) + value);
	set_f(SZHVC_add[ah | res]);
	A = res;
}

/***************************************************************
 * ADC  A,n
 ***************************************************************/
void z80_device::adc_a(u8 value)
{
	u32 ah = AFD & 0xff00, c = AFD & 1;
	u32 res = (u8)((ah >> 8) + value + c);
	set_f(SZHVC_add[(c << 16) | ah | res]);
	A = res;
}

/***************************************************************
 * SUB  n
 ***************************************************************/
void z80_device::sub(u8 value)
{
	u32 ah = AFD & 0xff00;
	u32 res = (u8)((ah >> 8) - value);
	set_f(SZHVC_sub[ah | res]);
	A = res;
}

/***************************************************************
 * SBC  A,n
 ***************************************************************/
void z80_device::sbc_a(u8 value)
{
	u32 ah = AFD & 0xff00, c = AFD & 1;
	u32 res = (u8)((ah >> 8) - value - c);
	set_f(SZHVC_sub[(c << 16) | ah | res]);
	A = res;
}

/***************************************************************
 * NEG
 ***************************************************************/
void z80_device::neg()
{
	u8 value = A;
	A = 0;
	sub(value);
}

/***************************************************************
 * DAA
 ***************************************************************/
void z80_device::daa()
{
	u8 a = A;
	if (F & NF)
	{
		if ((F&HF) | ((A&0xf)>9)) a-=6;
		if ((F&CF) | (A>0x99)) a-=0x60;
	}
	else
	{
		if ((F&HF) | ((A&0xf)>9)) a+=6;
		if ((F&CF) | (A>0x99)) a+=0x60;
	}

	set_f((F&(CF|NF)) | (A>0x99) | ((A^a)&HF) | SZP[a]);
	A = a;
}

/***************************************************************
 * AND  n
 ***************************************************************/
void z80_device::and_a(u8 value)
{
	A &= value;
	set_f(SZP[A] | HF);
}

/***************************************************************
 * OR   n
 ***************************************************************/
void z80_device::or_a(u8 value)
{
	A |= value;
	set_f(SZP[A]);
}

/***************************************************************
 * XOR  n
 ***************************************************************/
void z80_device::xor_a(u8 value)
{
	A ^= value;
	set_f(SZP[A]);
}

/***************************************************************
 * CP   n
 ***************************************************************/
void z80_device::cp(u8 value)
{
	unsigned val = value;
	u32 ah = AFD & 0xff00;
	u32 res = (u8)((ah >> 8) - val);
	set_f((SZHVC_sub[ah | res] & ~(YF | XF)) | (val & (YF | XF)));
}

/***************************************************************
 * EXX
 ***************************************************************/
void z80_device::exx()
{
	using std::swap;
	swap(m_bc, m_bc2);
	swap(m_de, m_de2);
	swap(m_hl, m_hl2);
}

/***************************************************************
 * RLC  r8
 ***************************************************************/
u8 z80_device::rlc(u8 value)
{
	unsigned res = value;
	unsigned c = (res & 0x80) ? CF : 0;
	res = ((res << 1) | (res >> 7)) & 0xff;
	set_f(SZP[res] | c);
	return res;
}

/***************************************************************
 * RRC  r8
 ***************************************************************/
u8 z80_device::rrc(u8 value)
{
	unsigned res = value;
	unsigned c = (res & 0x01) ? CF : 0;
	res = ((res >> 1) | (res << 7)) & 0xff;
	set_f(SZP[res] | c);
	return res;
}

/***************************************************************
 * RL   r8
 ***************************************************************/
u8 z80_device::rl(u8 value)
{
	unsigned res = value;
	unsigned c = (res & 0x80) ? CF : 0;
	res = ((res << 1) | (F & CF)) & 0xff;
	set_f(SZP[res] | c);
	return res;
}

/***************************************************************
 * RR   r8
 ***************************************************************/
u8 z80_device::rr(u8 value)
{
	unsigned res = value;
	unsigned c = (res & 0x01) ? CF : 0;
	res = ((res >> 1) | (F << 7)) & 0xff;
	set_f(SZP[res] | c);
	return res;
}

/***************************************************************
 * SLA  r8
 ***************************************************************/
u8 z80_device::sla(u8 value)
{
	unsigned res = value;
	unsigned c = (res & 0x80) ? CF : 0;
	res = (res << 1) & 0xff;
	set_f(SZP[res] | c);
	return res;
}

/***************************************************************
 * SRA  r8
 ***************************************************************/
u8 z80_device::sra(u8 value)
{
	unsigned res = value;
	unsigned c = (res & 0x01) ? CF : 0;
	res = ((res >> 1) | (res & 0x80)) & 0xff;
	set_f(SZP[res] | c);
	return res;
}

/***************************************************************
 * SLL  r8
 ***************************************************************/
u8 z80_device::sll(u8 value)
{
	unsigned res = value;
	unsigned c = (res & 0x80) ? CF : 0;
	res = ((res << 1) | 0x01) & 0xff;
	set_f(SZP[res] | c);
	return res;
}

/***************************************************************
 * SRL  r8
 ***************************************************************/
u8 z80_device::srl(u8 value)
{
	unsigned res = value;
	unsigned c = (res & 0x01) ? CF : 0;
	res = (res >> 1) & 0xff;
	set_f(SZP[res] | c);
	return res;
}

/***************************************************************
 * BIT  bit,r8
 ***************************************************************/
void z80_device::bit(int bit, u8 value)
{
	set_f((F & CF) | HF | (SZ_BIT[value & (1 << bit)] & ~(YF | XF)) | (value & (YF | XF)));
}

/***************************************************************
 * BIT  bit,(HL)
 ***************************************************************/
void z80_device::bit_hl(int bit, u8 value)
{
	set_f((F & CF) | HF | (SZ_BIT[value & (1 << bit)] & ~(YF | XF)) | (WZ_H & (YF | XF)));
}

/***************************************************************
 * BIT  bit,(IX/Y+o)
 ***************************************************************/
void z80_device::bit_xy(int bit, u8 value)
{
	set_f((F & CF) | HF | (SZ_BIT[value & (1 << bit)] & ~(YF | XF)) | ((m_ea >> 8) & (YF | XF)));
}

/***************************************************************
 * RES  bit,r8
 ***************************************************************/
u8 z80_device::res(int bit, u8 value)
{
	return value & ~(1 << bit);
}

/***************************************************************
 * SET  bit,r8
 ***************************************************************/
u8 z80_device::set(int bit, u8 value)
{
	return value | (1 << bit);
}

void z80_device::block_io_interrupted_flags()
{
	F &= ~(YF | XF);
	F |= (PC >> 8) & (YF | XF);
	if (F & CF)
	{
		F &= ~HF;
		if (TDAT8 & 0x80)
		{
			F ^= (SZP[(B - 1) & 0x07] ^ PF) & PF;
			if ((B & 0x0f) == 0x00) F |= HF;
		}
		else
		{
			F ^= (SZP[(B + 1) & 0x07] ^ PF) & PF;
			if ((B & 0x0f) == 0x0f) F |= HF;
		}
	}
	else
	{
		F ^=(SZP[B & 0x07] ^ PF) & PF;
	}
}

/***************************************************************
 * EI
 ***************************************************************/
void z80_device::ei()
{
	m_iff1 = m_iff2 = 1;
	m_after_ei = true;
}

void z80_device::set_f(u8 f)
{
	QT = 0;
	F = f;
}

void z80_device::illegal_1()
{
	LOGUNDOC("ill. opcode $%02x $%02x ($%04x)\n",
			 m_opcodes.read_byte(translate_memory_address((PCD - 1) & 0xffff)), m_opcodes.read_byte(translate_memory_address(PCD)), PCD - 1);
}

void z80_device::illegal_2()
{
	LOGUNDOC("ill. opcode $ed $%02x\n",
			 m_opcodes.read_byte(translate_memory_address((PCD - 1) & 0xffff)));
}

/****************************************************************************
 * Processor initialization
 ****************************************************************************/
void z80_device::device_validity_check(validity_checker &valid) const
{
	cpu_device::device_validity_check(valid);

	if (4 > m_m1_cycles)
		osd_printf_error("M1 cycles %u is less than minimum 4\n", m_m1_cycles);
	if (3 > m_memrq_cycles)
		osd_printf_error("MEMRQ cycles %u is less than minimum 3\n", m_memrq_cycles);
	if (4 > m_iorq_cycles)
		osd_printf_error("IORQ cycles %u is less than minimum 4\n", m_iorq_cycles);
}

void z80_device::device_start()
{
	if (!tables_initialised)
	{
		u8 *padd = &SZHVC_add[  0*256];
		u8 *padc = &SZHVC_add[256*256];
		u8 *psub = &SZHVC_sub[  0*256];
		u8 *psbc = &SZHVC_sub[256*256];
		for (int oldval = 0; oldval < 256; oldval++)
		{
			for (int newval = 0; newval < 256; newval++)
			{
				// add or adc w/o carry set
				int val = newval - oldval;
				*padd = (newval) ? ((newval & 0x80) ? SF : 0) : ZF;
				*padd |= (newval & (YF | XF));  // undocumented flag bits 5+3
				if( (newval & 0x0f) < (oldval & 0x0f) ) *padd |= HF;
				if( newval < oldval ) *padd |= CF;
				if( (val^oldval^0x80) & (val^newval) & 0x80 ) *padd |= VF;
				padd++;

				// adc with carry set
				val = newval - oldval - 1;
				*padc = (newval) ? ((newval & 0x80) ? SF : 0) : ZF;
				*padc |= (newval & (YF | XF));  // undocumented flag bits 5+3
				if( (newval & 0x0f) <= (oldval & 0x0f) ) *padc |= HF;
				if( newval <= oldval ) *padc |= CF;
				if( (val^oldval^0x80) & (val^newval) & 0x80 ) *padc |= VF;
				padc++;

				// cp, sub or sbc w/o carry set
				val = oldval - newval;
				*psub = NF | ((newval) ? ((newval & 0x80) ? SF : 0) : ZF);
				*psub |= (newval & (YF | XF));  // undocumented flag bits 5+3
				if( (newval & 0x0f) > (oldval & 0x0f) ) *psub |= HF;
				if( newval > oldval ) *psub |= CF;
				if( (val^oldval) & (oldval^newval) & 0x80 ) *psub |= VF;
				psub++;

				// sbc with carry set
				val = oldval - newval - 1;
				*psbc = NF | ((newval) ? ((newval & 0x80) ? SF : 0) : ZF);
				*psbc |= (newval & (YF | XF));  /* undocumented flag bits 5+3 */
				if( (newval & 0x0f) >= (oldval & 0x0f) ) *psbc |= HF;
				if( newval >= oldval ) *psbc |= CF;
				if( (val^oldval) & (oldval^newval) & 0x80 ) *psbc |= VF;
				psbc++;
			}
		}

		for (int i = 0; i < 256; i++)
		{
			int p = 0;
			if( i&0x01 ) ++p;
			if( i&0x02 ) ++p;
			if( i&0x04 ) ++p;
			if( i&0x08 ) ++p;
			if( i&0x10 ) ++p;
			if( i&0x20 ) ++p;
			if( i&0x40 ) ++p;
			if( i&0x80 ) ++p;
			SZ[i] = i ? i & SF : ZF;
			SZ[i] |= (i & (YF | XF));         // undocumented flag bits 5+3
			SZ_BIT[i] = i ? i & SF : ZF | PF;
			SZ_BIT[i] |= (i & (YF | XF));     // undocumented flag bits 5+3
			SZP[i] = SZ[i] | ((p & 1) ? 0 : PF);
			SZHV_inc[i] = SZ[i];
			if( i == 0x80 ) SZHV_inc[i] |= VF;
			if( (i & 0x0f) == 0x00 ) SZHV_inc[i] |= HF;
			SZHV_dec[i] = SZ[i] | NF;
			if( i == 0x7f ) SZHV_dec[i] |= VF;
			if( (i & 0x0f) == 0x0f ) SZHV_dec[i] |= HF;
		}

		tables_initialised = true;
	}

	save_item(NAME(m_prvpc.w.l));
	save_item(NAME(PC));
	save_item(NAME(SP));
	save_item(NAME(AF));
	save_item(NAME(BC));
	save_item(NAME(DE));
	save_item(NAME(HL));
	save_item(NAME(IX));
	save_item(NAME(IY));
	save_item(NAME(WZ));
	save_item(NAME(m_af2.w.l));
	save_item(NAME(m_bc2.w.l));
	save_item(NAME(m_de2.w.l));
	save_item(NAME(m_hl2.w.l));
	save_item(NAME(m_r));
	save_item(NAME(m_r2));
	save_item(NAME(m_q));
	save_item(NAME(m_qtemp));
	save_item(NAME(m_iff1));
	save_item(NAME(m_iff2));
	save_item(NAME(m_halt));
	save_item(NAME(m_im));
	save_item(NAME(m_i));
	save_item(NAME(m_nmi_state));
	save_item(NAME(m_nmi_pending));
	save_item(NAME(m_irq_state));
	save_item(NAME(m_wait_state));
	save_item(NAME(m_busrq_state));
	save_item(NAME(m_busack_state));
	save_item(NAME(m_after_ei));
	save_item(NAME(m_after_ldair));
	save_item(NAME(m_ref));
	save_item(NAME(m_tmp_irq_vector));
	save_item(NAME(m_shared_addr.w));
	save_item(NAME(m_shared_data.w));
	save_item(NAME(m_shared_data2.w));

	// Reset registers to their initial values
	PRVPC = 0;
	PCD = 0;
	SPD = 0;
	AFD = 0;
	BCD = 0;
	DED = 0;
	HLD = 0;
	IXD = 0;
	IYD = 0;
	WZ = 0;
	m_af2.d = 0;
	m_bc2.d = 0;
	m_de2.d = 0;
	m_hl2.d = 0;
	m_r = 0;
	m_r2 = 0;
	m_iff1 = 0;
	m_iff2 = 0;
	m_halt = 0;
	m_im = 0;
	m_i = 0;
	m_nmi_state = 0;
	m_nmi_pending = 0;
	m_irq_state = 0;
	m_wait_state = 0;
	m_busrq_state = 0;
	m_busack_state = 0;
	m_after_ei = 0;
	m_after_ldair = 0;
	m_ea = 0;

	space(AS_PROGRAM).cache(m_args);
	space(has_space(AS_OPCODES) ? AS_OPCODES : AS_PROGRAM).cache(m_opcodes);
	space(AS_PROGRAM).specific(m_data);
	space(AS_IO).specific(m_io);

	IX = IY = 0xffff; // IX and IY are FFFF after a reset!
	set_f(ZF);        // Zero flag is set

	// set up the state table
	state_add(STATE_GENPC,     "PC",        m_pc.w.l).callimport();
	state_add(STATE_GENPCBASE, "CURPC",     m_prvpc.w.l).callimport().noshow();
	state_add(Z80_SP,          "SP",        SP);
	state_add(STATE_GENFLAGS,  "GENFLAGS",  F).noshow().formatstr("%8s");
	state_add(Z80_A,           "A",         A).noshow();
	state_add(Z80_B,           "B",         B).noshow();
	state_add(Z80_C,           "C",         C).noshow();
	state_add(Z80_D,           "D",         D).noshow();
	state_add(Z80_E,           "E",         E).noshow();
	state_add(Z80_H,           "H",         H).noshow();
	state_add(Z80_L,           "L",         L).noshow();
	state_add(Z80_AF,          "AF",        AF);
	state_add(Z80_BC,          "BC",        BC);
	state_add(Z80_DE,          "DE",        DE);
	state_add(Z80_HL,          "HL",        HL);
	state_add(Z80_IX,          "IX",        IX);
	state_add(Z80_IY,          "IY",        IY);
	state_add(Z80_AF2,         "AF2",       m_af2.w.l);
	state_add(Z80_BC2,         "BC2",       m_bc2.w.l);
	state_add(Z80_DE2,         "DE2",       m_de2.w.l);
	state_add(Z80_HL2,         "HL2",       m_hl2.w.l);
	state_add(Z80_WZ,          "WZ",        WZ);
	state_add(Z80_R,           "R",         m_rtemp).callimport().callexport();
	state_add(Z80_I,           "I",         m_i);
	state_add(Z80_IM,          "IM",        m_im).mask(0x3);
	state_add(Z80_IFF1,        "IFF1",      m_iff1).mask(0x1);
	state_add(Z80_IFF2,        "IFF2",      m_iff2).mask(0x1);
	state_add(Z80_HALT,        "HALT",      m_halt).mask(0x1);

	// set our instruction counter
	set_icountptr(m_icount);
}

void nsc800_device::device_start()
{
	z80_device::device_start();

	save_item(NAME(m_nsc800_irq_state));
}

/****************************************************************************
 * Do a reset
 ****************************************************************************/
void z80_device::device_reset()
{
	leave_halt();

	m_ref = 0xffff00;
	PC = 0x0000;
	m_i = 0;
	m_r = 0;
	m_r2 = 0;
	m_nmi_pending = false;
	m_after_ei = false;
	m_after_ldair = false;
	m_iff1 = 0;
	m_iff2 = 0;

	WZ = PCD;
}

void nsc800_device::device_reset()
{
	z80_device::device_reset();
	memset(m_nsc800_irq_state, 0, sizeof(m_nsc800_irq_state));
}

void z80_device::do_op()
{
	#include "cpu/z80/z80.hxx"
}

void nsc800_device::do_op()
{
	#include "cpu/z80/ncs800.hxx"
}

/****************************************************************************
 * Execute 'cycles' T-states.
 ****************************************************************************/
void z80_device::execute_run()
{
	if (m_wait_state)
	{
		m_icount = 0; // stalled
		return;
	}

	while (m_icount > 0)
	{
		do_op();
	}
}

void z80_device::execute_set_input(int inputnum, int state)
{
	switch (inputnum)
	{
	case Z80_INPUT_LINE_BUSRQ:
		m_busrq_state = state;
		break;

	case INPUT_LINE_NMI:
		// mark an NMI pending on the rising edge
		if (m_nmi_state == CLEAR_LINE && state != CLEAR_LINE)
			m_nmi_pending = true;
		m_nmi_state = state;
		break;

	case INPUT_LINE_IRQ0:
		// update the IRQ state via the daisy chain
		m_irq_state = state;
		if (daisy_chain_present())
			m_irq_state = (daisy_update_irq_state() == ASSERT_LINE) ? ASSERT_LINE : m_irq_state;

		// the main execute loop will take the interrupt
		break;

	case Z80_INPUT_LINE_WAIT:
		m_wait_state = state;
		break;

	default:
		break;
	}
}

void nsc800_device::execute_set_input(int inputnum, int state)
{
	switch (inputnum)
	{
	case NSC800_RSTA:
		m_nsc800_irq_state[NSC800_RSTA] = state;
		break;

	case NSC800_RSTB:
		m_nsc800_irq_state[NSC800_RSTB] = state;
		break;

	case NSC800_RSTC:
		m_nsc800_irq_state[NSC800_RSTC] = state;
		break;

	default:
		z80_device::execute_set_input(inputnum, state);
		break;
	}
}

/**************************************************************************
 * STATE IMPORT/EXPORT
 **************************************************************************/
void z80_device::state_import(const device_state_entry &entry)
{
	switch (entry.index())
	{
	case STATE_GENPC:
		m_prvpc = m_pc;
		break;

	case STATE_GENPCBASE:
		m_pc = m_prvpc;
		break;

	case Z80_R:
		m_r = m_rtemp & 0x7f;
		m_r2 = m_rtemp & 0x80;
		break;

	default:
		fatalerror("CPU_IMPORT_STATE() called for unexpected value\n");
	}
}

void z80_device::state_export(const device_state_entry &entry)
{
	switch (entry.index())
	{
	case Z80_R:
		m_rtemp = (m_r & 0x7f) | (m_r2 & 0x80);
		break;

	default:
		fatalerror("CPU_EXPORT_STATE() called for unexpected value\n");
	}
}

void z80_device::state_string_export(const device_state_entry &entry, std::string &str) const
{
	switch (entry.index())
	{
		case STATE_GENFLAGS:
			str = string_format("%c%c%c%c%c%c%c%c",
				F & 0x80 ? 'S':'.',
				F & 0x40 ? 'Z':'.',
				F & 0x20 ? 'Y':'.',
				F & 0x10 ? 'H':'.',
				F & 0x08 ? 'X':'.',
				F & 0x04 ? 'P':'.',
				F & 0x02 ? 'N':'.',
				F & 0x01 ? 'C':'.');
			break;
	}
}

/**************************************************************************
 * disassemble - call the disassembly helper function
 **************************************************************************/
std::unique_ptr<util::disasm_interface> z80_device::create_disassembler()
{
	return std::make_unique<z80_disassembler>();
}

z80_device::z80_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: z80_device(mconfig, Z80, tag, owner, clock)
{
}

z80_device::z80_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock) :
	cpu_device(mconfig, type, tag, owner, clock),
	z80_daisy_chain_interface(mconfig, *this),
	m_program_config("program", ENDIANNESS_LITTLE, 8, 16, 0),
	m_opcodes_config("opcodes", ENDIANNESS_LITTLE, 8, 16, 0),
	m_io_config("io", ENDIANNESS_LITTLE, 8, 16, 0),
	m_irqack_cb(*this),
	m_refresh_cb(*this),
	m_nomreq_cb(*this),
	m_halt_cb(*this),
	m_busack_cb(*this),
	m_m1_cycles(4),
	m_memrq_cycles(3),
	m_iorq_cycles(4)
{
}

device_memory_interface::space_config_vector z80_device::memory_space_config() const
{
	if (has_configured_map(AS_OPCODES))
	{
		return space_config_vector {
				std::make_pair(AS_PROGRAM, &m_program_config),
				std::make_pair(AS_OPCODES, &m_opcodes_config),
				std::make_pair(AS_IO,      &m_io_config) };
	}
	else
	{
		return space_config_vector {
				std::make_pair(AS_PROGRAM, &m_program_config),
				std::make_pair(AS_IO,      &m_io_config) };
	}
}

DEFINE_DEVICE_TYPE(Z80, z80_device, "z80", "Zilog Z80")

nsc800_device::nsc800_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: z80_device(mconfig, NSC800, tag, owner, clock)
{
}

DEFINE_DEVICE_TYPE(NSC800, nsc800_device, "nsc800", "National Semiconductor NSC800")

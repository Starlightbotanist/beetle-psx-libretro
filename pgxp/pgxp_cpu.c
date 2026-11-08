#include <string.h>
#include <math.h>

#include "pgxp_cpu.h"
#include "pgxp_value.h"
#include "pgxp_mem.h"


#include "limits.h"

/* CPU registers */
PGXP_value CPU_reg_mem[34];
/*PGXP_value CPU_Hi, CPU_Lo; */
PGXP_value CP0_reg_mem[32];

PGXP_value* CPU_reg = CPU_reg_mem;
PGXP_value* CP0_reg = CP0_reg_mem;

static PGXP_value pending_load;
static uint32_t pending_load_reg;
/* Lightrec may execute the first instruction of a branch target before
 * publishing a load. Keep its saved and deferred shadows with that owner. */
static PGXP_value saved_load[32], deferred_load[32];
static uint32_t deferred_load_mask;

/* Instruction register decoding */
#define op(_instr)		(_instr >> 26)			/* The op part of the instruction register */
#define func(_instr)	((_instr) & 0x3F)		/* The funct part of the instruction register */
#define sa(_instr)		((_instr >>  6) & 0x1F) /* The sa part of the instruction register */
#define rd(_instr)		((_instr >> 11) & 0x1F)	/* The rd part of the instruction register */
#define rt(_instr)		((_instr >> 16) & 0x1F)	/* The rt part of the instruction register */
#define rs(_instr)		((_instr >> 21) & 0x1F)	/* The rs part of the instruction register */
#define imm(_instr)		(_instr & 0xFFFF)		/* The immediate part of the instruction register */

void PGXP_InitCPU()
{
	memset(CPU_reg_mem, 0, sizeof(CPU_reg_mem));
	memset(CP0_reg_mem, 0, sizeof(CP0_reg_mem));
	CPU_reg[0] = PGXP_value_zero;
	pending_load_reg = 0;
	deferred_load_mask = 0;
}

void PGXP_CPU_SaveLoad(uint32_t reg)
{
	if (reg != 0 && reg < 32)
		saved_load[reg] = CPU_reg[reg];
}

void PGXP_CPU_DeferLoad(uint32_t reg, int discard)
{
	if (reg != 0 && reg < 32)
	{
		if (!discard)
		{
			deferred_load[reg] = CPU_reg[reg];
			deferred_load_mask |= 1u << reg;
		}
		CPU_reg[reg] = saved_load[reg];
	}
}

void PGXP_CPU_FinishLoad(uint32_t reg, uint32_t value, int commit)
{
	if (reg != 0 && reg < 32)
	{
		if (commit)
		{
			if (deferred_load_mask & (1u << reg))
			{
				Validate(&deferred_load[reg], value);
				CPU_reg[reg] = deferred_load[reg];
			}
			else
				SetValue(&CPU_reg[reg], value);
		}
		deferred_load_mask &= ~(1u << reg);
	}
}

PGXP_value *PGXP_CPU_PendingLoad(uint32_t instr)
{
	pending_load_reg = rt(instr);
	return &pending_load;
}

static void LoadPartialWord(PGXP_value *dest, uint32_t value,
		uint32_t addr, int whole_word)
{
	if (whole_word)
		ValidateAndCopyMem(dest, addr, value);
	else
	{
		/* Merged register bytes do not validate the memory owner. Without
		 * a whole-word identity, retain only the native coordinate value. */
		*dest = PGXP_value_zero;
		SetValue(dest, value);
	}
	PGXP_CPU_ValidateColor(dest);
}

void PGXP_CPU_LoadDelayed(uint32_t instr, uint32_t value, uint32_t addr)
{
	PGXP_value *dest = PGXP_CPU_PendingLoad(instr);
	switch (op(instr))
	{
	case 0x20: case 0x24:
		SetValue(dest, value);
		dest->flags = 0;
		break;
	case 0x21: case 0x25:
		ValidateAndCopyMem16(dest, addr, value, op(instr) == 0x21);
		break;
	case 0x22: case 0x26:
		LoadPartialWord(dest, value, addr,
			(addr & 3) == (op(instr) == 0x22 ? 3 : 0));
		break;
	default:
		ValidateAndCopyMem(dest, addr, value);
		break;
	}
	PGXP_CPU_ValidateColor(dest);
}

void PGXP_CPU_CommitLoad(uint32_t reg, uint32_t value, uint32_t written)
{
	/* The caller supplies the register actually written by this instruction;
	 * a faulting arithmetic instruction has no destination. */
	if (reg != 0 && reg < 32 && reg != written)
	{
		if (pending_load_reg == reg)
		{
			Validate(&pending_load, value);
			CPU_reg[reg] = pending_load;
		}
		else
			SetValue(&CPU_reg[reg], value);
	}
	pending_load_reg = 0;
}

/* invalidate register (invalid 8 bit read) */
void InvalidLoad(uint32_t addr, uint32_t code, uint32_t value)
{
	uint32_t reg = ((code >> 16) & 0x1F); /* The rt part of the instruction register */
	PGXP_value* pD = NULL;
	PGXP_value p = {0};

	if (reg == 0)
		return;

	p.x = p.y = -1337; /* default values */

					   /*p.valid = 0;
					    *p.count = value; */
	pD = ReadMem(addr);

	if (pD)
	{
		p.count = addr;
		p = *pD;
	}
	else
	{
		p.count = value;
	}

	p.flags = 0;

	/* invalidate register */
	CPU_reg[reg] = p;
}

/* invalidate memory address (invalid 8 bit write) */
void InvalidStore(uint32_t addr, uint32_t code, uint32_t value)
{
	uint32_t reg = ((code >> 16) & 0x1F); /* The rt part of the instruction register */
	PGXP_value* dest = ReadMem(addr);
	if (dest)
	{
		/* Retire in place. The remaining bytes are unchanged, including
		 * the native halves needed by a later partial merge. */
		dest->flags = 0;
		dest->count = (reg * 1000) + value;
	}
}

/* ============================================================
 * Arithmetic with immediate value
 * ============================================================ */
void PGXP_CPU_ADDI(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
	/* Rt = Rs + Imm (signed) */
	psx_value tempImm;
	PGXP_value ret;
	float of;

	if (rt(instr) == 0)
		return;

	Validate(&CPU_reg[rs(instr)], rsVal);
	ret = CPU_reg[rs(instr)];
	if (imm(instr) == 0)
	{
		CPU_reg[rt(instr)] = ret;
		CPU_reg[rt(instr)].value = rtVal;
		return;
	}
	tempImm.d = imm(instr);
	tempImm.sd = (int32_t)(int16_t)tempImm.w.l;	/* sign extend low 16 -> 32 */

	ret.x = f16Unsign(ret.x);
	ret.x += tempImm.w.l;

	/* carry on over/underflow */
	of = (ret.x >= (float)USHRT_MAX + 1.f) ? 1.f : (ret.x < 0) ? -1.f : 0.f;
	ret.x = f16Sign(ret.x);
	ret.y += tempImm.sw.h + of;

	/* truncate on overflow/underflow */
	ret.y += (ret.y >= (float)SHRT_MAX + 1.f) ? -(USHRT_MAX + 1) : (ret.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

	ret.flags &= ~PGXP_COLOR;
	CPU_reg[rt(instr)] = ret;
	CPU_reg[rt(instr)].value = rtVal;
}

void PGXP_CPU_ADDIU(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
	/* Rt = Rs + Imm (signed) (unsafe?) */
	PGXP_CPU_ADDI(instr, rtVal, rsVal);
}

void PGXP_CPU_ANDI(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
	/* Rt = Rs & Imm */
	psx_value vRt;
	PGXP_value ret;

	if (rt(instr) == 0)
		return;

	Validate(&CPU_reg[rs(instr)], rsVal);
	ret = CPU_reg[rs(instr)];

	vRt.d = rtVal;

	ret.y = 0.f;	/* remove upper 16-bits */

	switch (imm(instr))
	{
	case 0:
		/* if 0 then x == 0 */
		ret.x = 0.f;
		break;
	case 0xFFFF:
		/* if saturated then x == x */
		break;
	default:
		/* otherwise x is low precision value */
		ret.x = vRt.sw.l;
		ret.flags |= VALID_0;
	}

	ret.flags |= VALID_1;

	ret.flags &= ~PGXP_COLOR;
	CPU_reg[rt(instr)] = ret;
	CPU_reg[rt(instr)].value = rtVal;
}

void PGXP_CPU_ORI(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
	/* Rt = Rs | Imm */
	psx_value vRt;
	PGXP_value ret;

	if (rt(instr) == 0)
		return;

	Validate(&CPU_reg[rs(instr)], rsVal);
	ret = CPU_reg[rs(instr)];

	vRt.d = rtVal;

	switch (imm(instr))
	{
	case 0:
		/* if 0 then x == x */
		break;
	default:
		/* otherwise x is low precision value */
		ret.x = vRt.sw.l;
		ret.flags |= VALID_0;
	}

	ret.value = rtVal;
	if (imm(instr) != 0)
		ret.flags &= ~PGXP_COLOR;
	CPU_reg[rt(instr)] = ret;
}

void PGXP_CPU_XORI(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
	/* Rt = Rs ^ Imm */
	psx_value vRt;
	PGXP_value ret;

	if (rt(instr) == 0)
		return;

	Validate(&CPU_reg[rs(instr)], rsVal);
	ret = CPU_reg[rs(instr)];

	vRt.d = rtVal;

	switch (imm(instr))
	{
	case 0:
		/* if 0 then x == x */
		break;
	default:
		/* otherwise x is low precision value */
		ret.x = vRt.sw.l;
		ret.flags |= VALID_0;
	}

	ret.value = rtVal;
	if (imm(instr) != 0)
		ret.flags &= ~PGXP_COLOR;
	CPU_reg[rt(instr)] = ret;
}

/* Comparisons have architectural Boolean results, not interpolated
 * coordinates. Unrelated input fractions cannot change that result or
 * provide a depth for it. */
static void PGXP_CPU_SetBoolean(uint32_t reg, uint32_t value)
{
	if (reg != 0)
	{
		CPU_reg[reg] = PGXP_value_zero;
		CPU_reg[reg].x = (float)value;
		CPU_reg[reg].value = value;
	}
}

void PGXP_CPU_SLTI(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
	PGXP_CPU_SetBoolean(rt(instr), rtVal);
}

void PGXP_CPU_SLTIU(uint32_t instr, uint32_t rtVal, uint32_t rsVal)
{
	PGXP_CPU_SetBoolean(rt(instr), rtVal);
}

/* ============================================================
 * Load Upper
 * ============================================================ */
void PGXP_CPU_LUI(uint32_t instr, uint32_t rtVal)
{
	/*Rt = Imm << 16 */
	if (rt(instr) == 0)
		return;

	CPU_reg[rt(instr)] = PGXP_value_zero;
	CPU_reg[rt(instr)].y = (float)(int16_t)imm(instr);
	CPU_reg[rt(instr)].hFlags = VALID_HALF;
	CPU_reg[rt(instr)].value = rtVal;
	CPU_reg[rt(instr)].flags = VALID_01;
}

/* ============================================================
 * Register Arithmetic
 * ============================================================ */

void PGXP_CPU_ADD(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Rd = Rs + Rt (signed) */
	PGXP_value ret;
	float of;

	if (rd(instr) == 0)
		return;

	Validate(&CPU_reg[rs(instr)], rsVal);
	Validate(&CPU_reg[rt(instr)], rtVal);

	/* Addition with the architectural zero register is an exact identity.
	 * Preserve the complete other operand shadow instead of blending. */
	if (rt(instr) == 0)
	{
		CPU_reg[rd(instr)] = CPU_reg[rs(instr)];
		CPU_reg[rd(instr)].value = rdVal;
		return;
	}
	if (rs(instr) == 0)
	{
		CPU_reg[rd(instr)] = CPU_reg[rt(instr)];
		CPU_reg[rd(instr)].value = rdVal;
		return;
	}

	/* iCB: Only require one valid input */
	if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
	{
		MakeValid(&CPU_reg[rs(instr)], rsVal);
		MakeValid(&CPU_reg[rt(instr)], rtVal);
	}

	ret = CPU_reg[rs(instr)];

	ret.x = f16Unsign(ret.x);
	ret.x += f16Unsign(CPU_reg[rt(instr)].x);

	/* carry on over/underflow */
	of = (ret.x >= (float)USHRT_MAX + 1.f) ? 1.f : (ret.x < 0) ? -1.f : 0.f;
	ret.x = f16Sign(ret.x);
	ret.y += CPU_reg[rt(instr)].y + of;

	/* truncate on overflow/underflow */
	ret.y += (ret.y >= (float)SHRT_MAX + 1.f) ? -(USHRT_MAX + 1) : (ret.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

	/* TODO: decide which "z/w" component to use */

	ret.halfFlags[0] &= CPU_reg[rt(instr)].halfFlags[0];
	ret.gFlags |= CPU_reg[rt(instr)].gFlags;
	ret.lFlags |= CPU_reg[rt(instr)].lFlags;
	ret.hFlags |= CPU_reg[rt(instr)].hFlags;

	ret.value = rdVal;

	ret.flags &= ~PGXP_COLOR;
	CPU_reg[rd(instr)] = ret;
}

void PGXP_CPU_ADDU(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Rd = Rs + Rt (signed) (unsafe?) */
	PGXP_CPU_ADD(instr, rdVal, rsVal, rtVal);
}

void PGXP_CPU_ADDU_Identity(uint32_t instr, uint32_t rdVal, uint32_t rsVal)
{
	unsigned dest = rd(instr);
	unsigned source = rs(instr);

	/* This transport is intentionally limited to the architectural
	 * `addu rd, rs, $zero` form established by device instrumentation. */
	if (rt(instr) != 0 || dest == 0 || dest == source)
		return;
	if (source == 0)
	{
		CPU_reg[dest] = PGXP_value_zero;
		SetValue(&CPU_reg[dest], rdVal);
		return;
	}

	Validate(&CPU_reg[source], rsVal);
	CPU_reg[dest] = CPU_reg[source];
	CPU_reg[dest].value = rdVal;
}

void PGXP_CPU_SUB(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Rd = Rs - Rt (signed) */
	PGXP_value ret;
	float of;

	if (rd(instr) == 0)
		return;

	Validate(&CPU_reg[rs(instr)], rsVal);
	Validate(&CPU_reg[rt(instr)], rtVal);

	if (rt(instr) == 0)
	{
		CPU_reg[rd(instr)] = CPU_reg[rs(instr)];
		CPU_reg[rd(instr)].value = rdVal;
		return;
	}

	/* iCB: Only require one valid input */
	if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
	{
		MakeValid(&CPU_reg[rs(instr)], rsVal);
		MakeValid(&CPU_reg[rt(instr)], rtVal);
	}

	ret = CPU_reg[rs(instr)];

	ret.x = f16Unsign(ret.x);
	ret.x -= f16Unsign(CPU_reg[rt(instr)].x);

	/* carry on over/underflow */
	of = (ret.x >= (float)USHRT_MAX + 1.f) ? 1.f : (ret.x < 0) ? -1.f : 0.f;
	ret.x = f16Sign(ret.x);
	ret.y -= CPU_reg[rt(instr)].y - of;

	/* truncate on overflow/underflow */
	ret.y += (ret.y >= (float)SHRT_MAX + 1.f) ? -(USHRT_MAX + 1) : (ret.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

	ret.halfFlags[0] &= CPU_reg[rt(instr)].halfFlags[0];
	ret.gFlags |= CPU_reg[rt(instr)].gFlags;
	ret.lFlags |= CPU_reg[rt(instr)].lFlags;
	ret.hFlags |= CPU_reg[rt(instr)].hFlags;

	ret.value = rdVal;

	ret.flags &= ~PGXP_COLOR;
	CPU_reg[rd(instr)] = ret;
}

void PGXP_CPU_SUBU(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Rd = Rs - Rt (signed) (unsafe?) */
	PGXP_CPU_SUB(instr, rdVal, rsVal, rtVal);
}

static void PGXP_CPU_Bitwise(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	psx_value vald, vals, valt;
	PGXP_value ret = {0};

	if (rd(instr) == 0)
		return;

	Validate(&CPU_reg[rs(instr)], rsVal);
	Validate(&CPU_reg[rt(instr)], rtVal);

	/* Command-byte masks can transport RGB without changing its payload.
	 * Use the input mask, not a coincidentally unchanged saturated result.
	 * Two distinct RGB sources do not establish an integer identity. */
	if ((CPU_reg[rs(instr)].flags | CPU_reg[rt(instr)].flags) & PGXP_COLOR)
	{
		const PGXP_value *color = NULL;
		unsigned function = instr & 0x3f;
		if (rs(instr) == rt(instr) && (function == 0x24 || function == 0x25))
			color = &CPU_reg[rs(instr)];
		else if (function != 0x27)
		{
			uint32_t neutral = function == 0x24 ? 0x00ffffff : 0;
			if ((CPU_reg[rs(instr)].flags & PGXP_COLOR) &&
				!(CPU_reg[rt(instr)].flags & PGXP_COLOR) && (rtVal & 0x00ffffff) == neutral)
				color = &CPU_reg[rs(instr)];
			else if ((CPU_reg[rt(instr)].flags & PGXP_COLOR) &&
				!(CPU_reg[rs(instr)].flags & PGXP_COLOR) && (rsVal & 0x00ffffff) == neutral)
				color = &CPU_reg[rt(instr)];
		}
		if (color)
		{
			CPU_reg[rd(instr)] = *color;
			CPU_reg[rd(instr)].value = rdVal;
			return;
		}
	}

	/* iCB: Only require one valid input */
	if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
	{
		MakeValid(&CPU_reg[rs(instr)], rsVal);
		MakeValid(&CPU_reg[rt(instr)], rtVal);
	}

	vald.d = rdVal;
	vals.d = rsVal;
	valt.d = rtVal;

	/*	CPU_reg[rd(instr)].valid = CPU_reg[rs(instr)].valid && CPU_reg[rt(instr)].valid; */
	ret.flags = VALID_01;

	if (vald.w.l == 0)
	{
		ret.x = 0.f;
		ret.lFlags = VALID_HALF;
	}
	else if (vald.w.l == vals.w.l)
	{
		ret.x = CPU_reg[rs(instr)].x;
		ret.lFlags = CPU_reg[rs(instr)].lFlags;
		ret.compFlags[0] = CPU_reg[rs(instr)].compFlags[0];
	}
	else if (vald.w.l == valt.w.l)
	{
		ret.x = CPU_reg[rt(instr)].x;
		ret.lFlags = CPU_reg[rt(instr)].lFlags;
		ret.compFlags[0] = CPU_reg[rt(instr)].compFlags[0];
	}
	else
	{
		ret.x = (float)vald.sw.l;
		ret.compFlags[0] = VALID;
		ret.lFlags = 0;
	}

	if (vald.w.h == 0)
	{
		ret.y = 0.f;
		ret.hFlags = VALID_HALF;
	}
	else if (vald.w.h == vals.w.h)
	{
		ret.y = CPU_reg[rs(instr)].y;
		ret.hFlags = CPU_reg[rs(instr)].hFlags;
		ret.compFlags[1] &= CPU_reg[rs(instr)].compFlags[1];
	}
	else if (vald.w.h == valt.w.h)
	{
		ret.y = CPU_reg[rt(instr)].y;
		ret.hFlags = CPU_reg[rt(instr)].hFlags;
		ret.compFlags[1] &= CPU_reg[rt(instr)].compFlags[1];
	}
	else
	{
		ret.y = (float)vald.sw.h;
		ret.compFlags[1] = VALID;
		ret.hFlags = 0;
	}

	/* iCB Hack: Force validity if even one half is valid
	 *if ((ret.hFlags & VALID_HALF) || (ret.lFlags & VALID_HALF))
	 *	ret.valid = 1;
	 * /iCB Hack */

	/* Get a valid W */
	if ((CPU_reg[rs(instr)].flags & VALID_2) == VALID_2)
	{
		ret.z = CPU_reg[rs(instr)].z;
		ret.compFlags[2] = CPU_reg[rs(instr)].compFlags[2];
	}
	else if((CPU_reg[rt(instr)].flags & VALID_2) == VALID_2)
	{
		ret.z = CPU_reg[rt(instr)].z;
		ret.compFlags[2] = CPU_reg[rt(instr)].compFlags[2];
	}

	ret.value = rdVal;
	CPU_reg[rd(instr)] = ret;
}

void PGXP_CPU_AND(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	PGXP_CPU_Bitwise((instr & ~0x3fu) | 0x24, rdVal, rsVal, rtVal);
}

void PGXP_CPU_OR(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Rd = Rs | Rt */
	if (rs(instr) == 0 || rt(instr) == 0)
	{
		PGXP_CPU_ADD(instr, rdVal, rsVal, rtVal);
		return;
	}
	PGXP_CPU_Bitwise((instr & ~0x3fu) | 0x25, rdVal, rsVal, rtVal);
}

void PGXP_CPU_XOR(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Rd = Rs ^ Rt */
	if (rs(instr) == 0 || rt(instr) == 0)
	{
		PGXP_CPU_ADD(instr, rdVal, rsVal, rtVal);
		return;
	}
	PGXP_CPU_Bitwise((instr & ~0x3fu) | 0x26, rdVal, rsVal, rtVal);
}

void PGXP_CPU_NOR(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Rd = Rs NOR Rt */
	PGXP_CPU_Bitwise((instr & ~0x3fu) | 0x27, rdVal, rsVal, rtVal);
}

void PGXP_CPU_SLT(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	PGXP_CPU_SetBoolean(rd(instr), rdVal);
}

void PGXP_CPU_SLTU(uint32_t instr, uint32_t rdVal, uint32_t rsVal, uint32_t rtVal)
{
	PGXP_CPU_SetBoolean(rd(instr), rdVal);
}

/* ============================================================
 * Register mult/div
 * ============================================================ */

void PGXP_CPU_MULT(uint32_t instr, uint32_t hiVal, uint32_t loVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Hi/Lo = Rs * Rt (signed) */
	double xx, xy, yx, yy;
	double lx = 0, ly = 0, hx = 0, hy = 0;

	Validate(&CPU_reg[rs(instr)], rsVal);
	Validate(&CPU_reg[rt(instr)], rtVal);

	/* iCB: Only require one valid input */
	if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
	{
		MakeValid(&CPU_reg[rs(instr)], rsVal);
		MakeValid(&CPU_reg[rt(instr)], rtVal);
	}

	CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];
	CPU_Lo.flags &= ~PGXP_COLOR;
	CPU_Hi.flags &= ~PGXP_COLOR;

	CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

	/* Multiply out components */
	xx = f16Unsign(CPU_reg[rs(instr)].x) * f16Unsign(CPU_reg[rt(instr)].x);
	xy = f16Unsign(CPU_reg[rs(instr)].x) * (CPU_reg[rt(instr)].y);
	yx = (CPU_reg[rs(instr)].y) * f16Unsign(CPU_reg[rt(instr)].x);
	yy = (CPU_reg[rs(instr)].y) * (CPU_reg[rt(instr)].y);

	/* Split values into outputs */
	lx = xx;

	ly = f16Overflow(xx);
	ly += xy + yx;

	hx = f16Overflow(ly);
	hx += yy;

	hy = f16Overflow(hx);

	CPU_Lo.x = f16Sign(lx);
	CPU_Lo.y = f16Sign(ly);
	CPU_Hi.x = f16Sign(hx);
	CPU_Hi.y = f16Sign(hy);

	CPU_Lo.value = loVal;
	CPU_Hi.value = hiVal;
}

void PGXP_CPU_MULTU(uint32_t instr, uint32_t hiVal, uint32_t loVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Hi/Lo = Rs * Rt (unsigned) */
	double xx, xy, yx, yy;
	double lx = 0, ly = 0, hx = 0, hy = 0;

	Validate(&CPU_reg[rs(instr)], rsVal);
	Validate(&CPU_reg[rt(instr)], rtVal);

	/* iCB: Only require one valid input */
	if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
	{
		MakeValid(&CPU_reg[rs(instr)], rsVal);
		MakeValid(&CPU_reg[rt(instr)], rtVal);
	}

	CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];
	CPU_Lo.flags &= ~PGXP_COLOR;
	CPU_Hi.flags &= ~PGXP_COLOR;

	CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

	/* Multiply out components */
	xx = f16Unsign(CPU_reg[rs(instr)].x) * f16Unsign(CPU_reg[rt(instr)].x);
	xy = f16Unsign(CPU_reg[rs(instr)].x) * f16Unsign(CPU_reg[rt(instr)].y);
	yx = f16Unsign(CPU_reg[rs(instr)].y) * f16Unsign(CPU_reg[rt(instr)].x);
	yy = f16Unsign(CPU_reg[rs(instr)].y) * f16Unsign(CPU_reg[rt(instr)].y);

	/* Split values into outputs */
	lx = xx;

	ly = f16Overflow(xx);
	ly += xy + yx;

	hx = f16Overflow(ly);
	hx += yy;

	hy = f16Overflow(hx);

	CPU_Lo.x = f16Sign(lx);
	CPU_Lo.y = f16Sign(ly);
	CPU_Hi.x = f16Sign(hx);
	CPU_Hi.y = f16Sign(hy);

	CPU_Lo.value = loVal;
	CPU_Hi.value = hiVal;
}

void PGXP_CPU_DIV(uint32_t instr, uint32_t hiVal, uint32_t loVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Lo = Rs / Rt (signed)
	 * Hi = Rs % Rt (signed) */
	double vs, vt, lo, hi;

	Validate(&CPU_reg[rs(instr)], rsVal);
	Validate(&CPU_reg[rt(instr)], rtVal);

	/* iCB: Only require one valid input */
	if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
	{
		MakeValid(&CPU_reg[rs(instr)], rsVal);
		MakeValid(&CPU_reg[rt(instr)], rtVal);
	}

	CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];
	CPU_Lo.flags &= ~PGXP_COLOR;
	CPU_Hi.flags &= ~PGXP_COLOR;

	CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

	vs = f16Unsign(CPU_reg[rs(instr)].x) + (CPU_reg[rs(instr)].y) * (double)(1 << 16);
	vt = f16Unsign(CPU_reg[rt(instr)].x) + (CPU_reg[rt(instr)].y) * (double)(1 << 16);
	if (rtVal == 0 || vt == 0.0 || !isfinite(vs) || !isfinite(vt))
	{
		SetValue(&CPU_Lo, loVal);
		SetValue(&CPU_Hi, hiVal);
		return;
	}

	lo = vs / vt;
	CPU_Lo.y = f16Sign(f16Overflow(lo));
	CPU_Lo.x = f16Sign(lo);

	hi = fmod(vs, vt);
	CPU_Hi.y = f16Sign(f16Overflow(hi));
	CPU_Hi.x = f16Sign(hi);

	CPU_Lo.value = loVal;
	CPU_Hi.value = hiVal;
}

void PGXP_CPU_DIVU(uint32_t instr, uint32_t hiVal, uint32_t loVal, uint32_t rsVal, uint32_t rtVal)
{
	/* Lo = Rs / Rt (unsigned)
	 * Hi = Rs % Rt (unsigned) */
	double vs, vt, lo, hi;

	Validate(&CPU_reg[rs(instr)], rsVal);
	Validate(&CPU_reg[rt(instr)], rtVal);

	/* iCB: Only require one valid input */
	if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
	{
		MakeValid(&CPU_reg[rs(instr)], rsVal);
		MakeValid(&CPU_reg[rt(instr)], rtVal);
	}

	CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];
	CPU_Lo.flags &= ~PGXP_COLOR;
	CPU_Hi.flags &= ~PGXP_COLOR;

	CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

	vs = f16Unsign(CPU_reg[rs(instr)].x) + f16Unsign(CPU_reg[rs(instr)].y) * (double)(1 << 16);
	vt = f16Unsign(CPU_reg[rt(instr)].x) + f16Unsign(CPU_reg[rt(instr)].y) * (double)(1 << 16);
	if (rtVal == 0 || vt == 0.0 || !isfinite(vs) || !isfinite(vt))
	{
		SetValue(&CPU_Lo, loVal);
		SetValue(&CPU_Hi, hiVal);
		return;
	}

	lo = vs / vt;
	CPU_Lo.y = f16Sign(f16Overflow(lo));
	CPU_Lo.x = f16Sign(lo);

	hi = fmod(vs, vt);
	CPU_Hi.y = f16Sign(f16Overflow(hi));
	CPU_Hi.x = f16Sign(hi);

	CPU_Lo.value = loVal;
	CPU_Hi.value = hiVal;
}

/* ============================================================
 * Shift operations (sa)
 * ============================================================ */
static void PGXP_CPU_ShiftLeft(uint32_t instr, uint32_t rdVal,
      uint32_t rtVal, uint32_t sh)
{
	PGXP_value ret;
	double x, y;

	if (rd(instr) == 0)
		return;

	Validate(&CPU_reg[rt(instr)], rtVal);
	ret = CPU_reg[rt(instr)];
	if (sh >= 16)
	{
		ret.y = f16Sign(f16Unsign(ret.x) * (1u << (sh - 16)));
		ret.x = 0.f;
		ret.compFlags[1] = ret.compFlags[0];
		ret.compFlags[0] = VALID;
		ret.hFlags = ret.lFlags;
		ret.lFlags = VALID_HALF;
	}
	else if (sh != 0)
	{
		x = f16Unsign(ret.x) * (1u << sh);
		y = f16Unsign(ret.y) * (1u << sh) + f16Overflow(x);
		ret.x = f16Sign(x);
		ret.y = f16Sign(y);
	}
	ret.value = rdVal;
	if (sh != 0)
		ret.flags &= ~PGXP_COLOR;
	CPU_reg[rd(instr)] = ret;
}

void PGXP_CPU_SLL(uint32_t instr, uint32_t rdVal, uint32_t rtVal)
{
	PGXP_CPU_ShiftLeft(instr, rdVal, rtVal, sa(instr));
}

/* Each half carries independent subpixel precision. Only the native high
 * half supplies the bits crossing into the low half during a short shift. */
static void PGXP_CPU_ShiftRight(uint32_t instr, uint32_t rdVal,
      uint32_t rtVal, uint32_t sh, int arithmetic)
{
	PGXP_value ret;
	double x, y, inverse_scale;
	uint32_t scale;

	if (rd(instr) == 0)
		return;

	Validate(&CPU_reg[rt(instr)], rtVal);
	ret = CPU_reg[rt(instr)];
	if (sh >= 16)
	{
		/* Exact powers of two avoid a variable floating-point division.
		 * Both branches only shift by 1..16, never the word width. */
		inverse_scale = (1u << (32 - sh)) * (1.0 / 65536.0);
		x = arithmetic ? ret.y : f16Unsign(ret.y);
		ret.x = f16Sign(x * inverse_scale);
		ret.y = (arithmetic && (rtVal & 0x80000000u)) ? -1.f : 0.f;
		ret.compFlags[0] = ret.compFlags[1];
		ret.compFlags[1] = VALID;
		ret.lFlags = ret.hFlags;
		ret.hFlags = VALID_HALF;
	}
	else if (sh != 0)
	{
		scale = 1u << sh;
		inverse_scale = (1u << (16 - sh)) * (1.0 / 65536.0);
		x = f16Unsign(ret.x) * inverse_scale;
		x += ((rtVal >> 16) & (scale - 1)) * (double)(1u << (16 - sh));
		y = arithmetic ? ret.y : f16Unsign(ret.y);
		ret.x = f16Sign(x);
		ret.y = f16Sign(y * inverse_scale);
	}
	ret.value = rdVal;
	if (sh != 0)
		ret.flags &= ~PGXP_COLOR;
	CPU_reg[rd(instr)] = ret;
}

void PGXP_CPU_SRL(uint32_t instr, uint32_t rdVal, uint32_t rtVal)
{
	PGXP_CPU_ShiftRight(instr, rdVal, rtVal, sa(instr), 0);
}

void PGXP_CPU_SRA(uint32_t instr, uint32_t rdVal, uint32_t rtVal)
{
	PGXP_CPU_ShiftRight(instr, rdVal, rtVal, sa(instr), 1);
}

/* ============================================================
 * Shift operations variable
 * ============================================================ */
void PGXP_CPU_SLLV(uint32_t instr, uint32_t rdVal, uint32_t rtVal, uint32_t rsVal)
{
	Validate(&CPU_reg[rs(instr)], rsVal);
	PGXP_CPU_ShiftLeft(instr, rdVal, rtVal, rsVal & 0x1f);
}

void PGXP_CPU_SRLV(uint32_t instr, uint32_t rdVal, uint32_t rtVal, uint32_t rsVal)
{
	Validate(&CPU_reg[rs(instr)], rsVal);
	PGXP_CPU_ShiftRight(instr, rdVal, rtVal, rsVal & 0x1f, 0);
}

void PGXP_CPU_SRAV(uint32_t instr, uint32_t rdVal, uint32_t rtVal, uint32_t rsVal)
{
	Validate(&CPU_reg[rs(instr)], rsVal);
	PGXP_CPU_ShiftRight(instr, rdVal, rtVal, rsVal & 0x1f, 1);
}

/* ============================================================
 * Move registers
 * ============================================================ */
void PGXP_CPU_MFHI(uint32_t instr, uint32_t rdVal, uint32_t hiVal)
{
	/* Rd = Hi */
	if (rd(instr) == 0)
		return;

	Validate(&CPU_Hi, hiVal);

	CPU_reg[rd(instr)] = CPU_Hi;
}

void PGXP_CPU_MTHI(uint32_t instr, uint32_t hiVal, uint32_t rsVal)
{
	/* Hi = Rs */
	Validate(&CPU_reg[rs(instr)], rsVal);

	CPU_Hi = CPU_reg[rs(instr)];
}

void PGXP_CPU_MFLO(uint32_t instr, uint32_t rdVal, uint32_t loVal)
{
	/* Rd = Lo */
	if (rd(instr) == 0)
		return;

	Validate(&CPU_Lo, loVal);

	CPU_reg[rd(instr)] = CPU_Lo;
}

void PGXP_CPU_MTLO(uint32_t instr, uint32_t loVal, uint32_t rsVal)
{
	/* Lo = Rs */
	Validate(&CPU_reg[rs(instr)], rsVal);

	CPU_Lo = CPU_reg[rs(instr)];
}

/* ============================================================
 * Memory Access
 * ============================================================ */

/* Load 32-bit word */
void PGXP_CPU_LWL(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
	if (rt(instr) != 0)
		LoadPartialWord(&CPU_reg[rt(instr)], rtVal, addr, (addr & 3) == 3);
}

void PGXP_CPU_LW(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
	/* Rt = Mem[Rs + Im] */
	if (rt(instr) == 0)
		return;

	ValidateAndCopyMem(&CPU_reg[rt(instr)], addr, rtVal);
	PGXP_CPU_ValidateColor(&CPU_reg[rt(instr)]);
}

void PGXP_CPU_LWR(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
	if (rt(instr) != 0)
		LoadPartialWord(&CPU_reg[rt(instr)], rtVal, addr, (addr & 3) == 0);
}

/* Load 16-bit */
void PGXP_CPU_LH(uint32_t instr, uint16_t rtVal, uint32_t addr)
{
	/* Rt = Mem[Rs + Im] (sign extended) */
	psx_value val;
	if (rt(instr) == 0)
		return;

	val.sd = (int32_t)(int16_t)rtVal;
	ValidateAndCopyMem16(&CPU_reg[rt(instr)], addr, val.d, 1);
}

void PGXP_CPU_LHU(uint32_t instr, uint16_t rtVal, uint32_t addr)
{
	/* Rt = Mem[Rs + Im] (zero extended) */
	psx_value val;
	if (rt(instr) == 0)
		return;

	val.d = rtVal;
	val.w.h = 0;
	ValidateAndCopyMem16(&CPU_reg[rt(instr)], addr, val.d, 0);
}

/* Load 8-bit */
void PGXP_CPU_LB(uint32_t instr, uint8_t rtVal, uint32_t addr)
{
	InvalidLoad(addr, instr, 116);
}

void PGXP_CPU_LBU(uint32_t instr, uint8_t rtVal, uint32_t addr)
{
	InvalidLoad(addr, instr, 116);
}

/* Store 32-bit word */
void PGXP_CPU_SWL(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
	if ((addr & 3) == 3)
		PGXP_CPU_SW(instr, rtVal, addr);
	else
		PGXP_InvalidateMem(addr);
}

void PGXP_CPU_SW(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
	/* Mem[Rs + Im] = Rt */
	Validate(&CPU_reg[rt(instr)], rtVal);
	WriteMem(&CPU_reg[rt(instr)], addr);
}

void PGXP_CPU_SWR(uint32_t instr, uint32_t rtVal, uint32_t addr)
{
	if ((addr & 3) == 0)
		PGXP_CPU_SW(instr, rtVal, addr);
	else
		PGXP_InvalidateMem(addr);
}

/* Store 16-bit */
void PGXP_CPU_SH(uint32_t instr, uint16_t rtVal, uint32_t addr)
{
	/* validate and copy half value */
	MaskValidate(&CPU_reg[rt(instr)], rtVal, 0xFFFF, VALID_0);
	WriteMem16(&CPU_reg[rt(instr)], addr);
}

/* Store 8-bit */
void PGXP_CPU_SB(uint32_t instr, uint8_t rtVal, uint32_t addr)
{
	InvalidStore(addr, instr, 208);
}

/* ============================================================
 * Data transfer tracking
 * ============================================================ */
void PGXP_CP0_MFC0(uint32_t instr, uint32_t rtVal, uint32_t rdVal)
{
	/* CPU[Rt] = CP0[Rd] */
	if (rt(instr) == 0)
		return;

	Validate(&CP0_reg[rd(instr)], rdVal);
	CPU_reg[rt(instr)] = CP0_reg[rd(instr)];
	CPU_reg[rt(instr)].value = rtVal;
	PGXP_CPU_ValidateColor(&CPU_reg[rt(instr)]);
}

void PGXP_CP0_MTC0(uint32_t instr, uint32_t rdVal, uint32_t rtVal)
{
	/* CP0[Rd] = CPU[Rt] */
	Validate(&CPU_reg[rt(instr)], rtVal);
	CP0_reg[rd(instr)] = CPU_reg[rt(instr)];
	CP0_reg[rd(instr)].value = rdVal;
}

void PGXP_CP0_CFC0(uint32_t instr, uint32_t rtVal, uint32_t rdVal)
{
	/* CPU[Rt] = CP0[Rd] */
	if (rt(instr) == 0)
		return;

	Validate(&CP0_reg[rd(instr)], rdVal);
	CPU_reg[rt(instr)] = CP0_reg[rd(instr)];
	CPU_reg[rt(instr)].value = rtVal;
}

void PGXP_CP0_CTC0(uint32_t instr, uint32_t rdVal, uint32_t rtVal)
{
	/* CP0[Rd] = CPU[Rt] */
	Validate(&CPU_reg[rt(instr)], rtVal);
	CP0_reg[rd(instr)] = CPU_reg[rt(instr)];
	CP0_reg[rd(instr)].value = rdVal;
}

void PGXP_CP0_RFE(uint32_t instr)
{}

// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef __LIGHTREC_PGXP_H__
#define __LIGHTREC_PGXP_H__

#include "disassembler.h"

#include <stdbool.h>

/* Non-memory instructions whose source and result registers must be passed
 * to the host PGXP callback. Memory operations use the I/O callback path. */
static inline bool lightrec_pgxp_cpu_tracked(union code c)
{
	switch (c.i.op) {
	case OP_SPECIAL:
		switch (c.r.op) {
		case OP_SPECIAL_SLL:  case OP_SPECIAL_SRL:  case OP_SPECIAL_SRA:
		case OP_SPECIAL_SLLV: case OP_SPECIAL_SRLV: case OP_SPECIAL_SRAV:
		case OP_SPECIAL_MFHI: case OP_SPECIAL_MTHI:
		case OP_SPECIAL_MFLO: case OP_SPECIAL_MTLO:
		case OP_SPECIAL_MULT: case OP_SPECIAL_MULTU:
		case OP_SPECIAL_DIV:  case OP_SPECIAL_DIVU:
		case OP_SPECIAL_ADD:  case OP_SPECIAL_ADDU:
		case OP_SPECIAL_SUB:  case OP_SPECIAL_SUBU:
		case OP_SPECIAL_AND:  case OP_SPECIAL_OR:
		case OP_SPECIAL_XOR:  case OP_SPECIAL_NOR:
		case OP_SPECIAL_SLT:  case OP_SPECIAL_SLTU:
			return true;
		default:
			return false;
		}
	case OP_ADDI: case OP_ADDIU:
	case OP_SLTI: case OP_SLTIU:
	case OP_ANDI: case OP_ORI:
	case OP_XORI: case OP_LUI:
		return true;
	default:
		return false;
	}
}

#endif /* __LIGHTREC_PGXP_H__ */

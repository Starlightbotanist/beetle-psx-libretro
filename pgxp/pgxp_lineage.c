#include <math.h>
#include <string.h>

#include "pgxp_cpu.h"
#include "pgxp_lineage.h"
#include "pgxp_main.h"
#include "pgxp_value.h"

#define PGXP_LINEAGE_RAM_WORDS       (UINT32_C(2048) * 1024 / 4)
#define PGXP_LINEAGE_SCRATCH_WORDS   (UINT32_C(1024) / 4)
#define PGXP_LINEAGE_MEMORY_WORDS    \
	(PGXP_LINEAGE_RAM_WORDS + PGXP_LINEAGE_SCRATCH_WORDS)
#define PGXP_LINEAGE_INVALID_INDEX   UINT32_MAX

#define PGXP_LINEAGE_VALID           (UINT32_C(1) << 0)
#define PGXP_LINEAGE_STAGE_SHIFT     1
#define PGXP_LINEAGE_STAGE_MASK      (UINT32_C(3) << PGXP_LINEAGE_STAGE_SHIFT)
#define PGXP_LINEAGE_VALID_W         (UINT32_C(1) << 3)

typedef struct
{
	float x;
	float y;
	float z;
	uint32_t value;
	uint32_t meta;
} PGXP_lineage;

typedef struct
{
	PGXP_lineage lineage;
	uint32_t word_addr;
} PGXP_gpu_lineage;

/* A direct record is retained for every address so display lists remain
 * valid until their architectural word is overwritten.  The compact bitmap
 * is checked first on every load/FIFO lookup; invalid addresses therefore do
 * not pull a 20-byte record into cache.  This replaces the diagnostic build's
 * roughly 54 MiB of RAM provenance with about 10 MiB plus a 64 KiB bitmap. */
static PGXP_lineage lineage_memory[PGXP_LINEAGE_MEMORY_WORDS];
static uint32_t lineage_memory_valid[(PGXP_LINEAGE_MEMORY_WORDS + 31) / 32];
static PGXP_lineage lineage_reg[32];
static PGXP_gpu_lineage lineage_fifo[32];
static PGXP_gpu_lineage lineage_cb[16];
static uint32_t lineage_reg_touched;

static unsigned lineage_stage(const PGXP_lineage* lineage)
{
	return (lineage->meta & PGXP_LINEAGE_STAGE_MASK) >>
		PGXP_LINEAGE_STAGE_SHIFT;
}

static void lineage_set_stage(PGXP_lineage* lineage, unsigned stage)
{
	lineage->meta = (lineage->meta & ~PGXP_LINEAGE_STAGE_MASK) |
		((uint32_t)stage << PGXP_LINEAGE_STAGE_SHIFT);
}

static int lineage_enabled(void)
{
	return PGXP_FeatureEnabled(PGXP_FEATURE_IDENTITY_MOVE) ||
		PGXP_FeatureEnabled(PGXP_FEATURE_EXACT_LINEAGE);
}

static uint32_t lineage_memory_index(uint32_t addr)
{
	switch (addr >> 24)
	{
		case 0x00:
		case 0x80:
		case 0xa0:
			return (addr & UINT32_C(0x001ffffc)) >> 2;
		default:
			if (addr >= UINT32_C(0x1f800000) &&
			    addr < UINT32_C(0x1f800400))
				return PGXP_LINEAGE_RAM_WORDS +
					((addr - UINT32_C(0x1f800000)) >> 2);
			return PGXP_LINEAGE_INVALID_INDEX;
	}
}

static uint32_t lineage_word_addr(uint32_t addr)
{
	uint32_t index = lineage_memory_index(addr);

	if (index == PGXP_LINEAGE_INVALID_INDEX)
		return UINT32_MAX;
	if (index < PGXP_LINEAGE_RAM_WORDS)
		return index << 2;
	return UINT32_C(0x1f800000) +
		((index - PGXP_LINEAGE_RAM_WORDS) << 2);
}

static int lineage_memory_is_valid(uint32_t index)
{
	return (lineage_memory_valid[index >> 5] >> (index & 31)) & 1;
}

static void lineage_memory_set_valid(uint32_t index)
{
	lineage_memory_valid[index >> 5] |= UINT32_C(1) << (index & 31);
}

static void lineage_memory_clear(uint32_t index)
{
	lineage_memory_valid[index >> 5] &= ~(UINT32_C(1) << (index & 31));
}

static PGXP_lineage* lineage_memory_read(uint32_t addr)
{
	uint32_t index = lineage_memory_index(addr);

	if (index == PGXP_LINEAGE_INVALID_INDEX ||
	    !lineage_memory_is_valid(index))
		return NULL;
	return &lineage_memory[index];
}

void PGXP_LineageInit(void)
{
	PGXP_LineageReset();
}

void PGXP_LineageReset(void)
{
	memset(lineage_memory_valid, 0, sizeof(lineage_memory_valid));
	memset(lineage_reg, 0, sizeof(lineage_reg));
	memset(lineage_fifo, 0, sizeof(lineage_fifo));
	memset(lineage_cb, 0, sizeof(lineage_cb));
	lineage_reg_touched = 0;
}

void PGXP_LineageMFC2(uint32_t instr, uint32_t value,
		const PGXP_value* precise)
{
	unsigned dest = (instr >> 16) & 31;
	PGXP_lineage* lineage;

	if (dest == 0)
		return;
	lineage_reg_touched |= UINT32_C(1) << dest;
	lineage = &lineage_reg[dest];
	memset(lineage, 0, sizeof(*lineage));
	if (!lineage_enabled() || !precise || precise->value != value ||
	    (precise->flags & VALID_01) != VALID_01 ||
	    !isfinite(precise->x) || !isfinite(precise->y) ||
	    !isfinite(precise->z))
		return;

	lineage->x = precise->x;
	lineage->y = precise->y;
	lineage->z = precise->z;
	lineage->value = value;
	lineage->meta = PGXP_LINEAGE_VALID |
		(UINT32_C(1) << PGXP_LINEAGE_STAGE_SHIFT);
	if ((precise->flags & VALID_2) == VALID_2)
		lineage->meta |= PGXP_LINEAGE_VALID_W;
}

void PGXP_LineageShift(uint32_t instr, uint32_t before,
		uint32_t after, int arithmetic)
{
	unsigned source = (instr >> 16) & 31;
	unsigned dest = (instr >> 11) & 31;
	unsigned shift = (instr >> 6) & 31;
	PGXP_lineage prior;

	if (dest == 0)
		return;
	prior = lineage_reg[source];
	lineage_reg_touched |= UINT32_C(1) << dest;
	memset(&lineage_reg[dest], 0, sizeof(lineage_reg[dest]));
	if (!PGXP_FeatureEnabled(PGXP_FEATURE_EXACT_LINEAGE) ||
	    !(prior.meta & PGXP_LINEAGE_VALID) || shift != 5)
		return;

	if (!arithmetic && lineage_stage(&prior) == 1 &&
	    prior.value == before)
	{
		lineage_reg[dest] = prior;
		lineage_reg[dest].value = after;
		lineage_set_stage(&lineage_reg[dest], 2);
	}
	else if (arithmetic && dest == source &&
	         lineage_stage(&prior) == 2 && prior.value == before)
	{
		lineage_reg[dest] = prior;
		lineage_reg[dest].value = after;
		lineage_set_stage(&lineage_reg[dest], 3);
	}
}

void PGXP_LineageIdentityMove(unsigned dest, unsigned source,
		uint32_t before, uint32_t after)
{
	PGXP_lineage prior;
	PGXP_value result;
	unsigned stage;

	if (dest == 0)
		return;
	prior = lineage_reg[source];
	lineage_reg_touched |= UINT32_C(1) << dest;
	memset(&lineage_reg[dest], 0, sizeof(lineage_reg[dest]));
	if (!PGXP_FeatureEnabled(PGXP_FEATURE_IDENTITY_MOVE) || source == 0 ||
	    before != after || !(prior.meta & PGXP_LINEAGE_VALID) ||
	    prior.value != before)
		return;

	stage = lineage_stage(&prior);
	if (stage == 2 || stage == 3)
	{
		if (PGXP_FeatureEnabled(PGXP_FEATURE_EXACT_LINEAGE))
			lineage_reg[dest] = prior;
		return;
	}
	if (stage != 1)
		return;

	/* Direct MFC2 aliases belong to I, not J.  Preserve the ordinary PGXP
	 * shadow so later legitimate CPU operations retain GTE precision. */
	Validate(&CPU_reg[source], before);
	result = CPU_reg[source];
	if ((result.flags & VALID_01) != VALID_01 || result.value != before)
		return;
	result.value = after;
	CPU_reg[dest] = result;
	lineage_reg[dest] = prior;
}

static int lineage_written_register(uint32_t instr, unsigned* dest)
{
	uint32_t primary = instr >> 26;

	if (primary == 0)
	{
		switch (instr & 63)
		{
			case 0x00: case 0x02: case 0x03:
			case 0x04: case 0x06: case 0x07:
			case 0x09: case 0x10: case 0x12:
			case 0x20: case 0x21: case 0x22: case 0x23:
			case 0x24: case 0x25: case 0x26: case 0x27:
			case 0x2a: case 0x2b:
				*dest = (instr >> 11) & 31;
				return 1;
		}
	}
	else if (primary == 0x01)
	{
		uint32_t subop = (instr >> 16) & 31;
		if (subop == 0x10 || subop == 0x11)
		{
			*dest = 31;
			return 1;
		}
	}
	else if (primary == 0x03)
	{
		*dest = 31;
		return 1;
	}
	else if ((primary >= 0x08 && primary <= 0x0f) ||
	         (primary >= 0x20 && primary <= 0x26))
	{
		*dest = (instr >> 16) & 31;
		return 1;
	}
	else if (primary == 0x10 || primary == 0x12)
	{
		uint32_t cop_op = (instr >> 21) & 31;
		if (cop_op == 0 || cop_op == 2)
		{
			*dest = (instr >> 16) & 31;
			return 1;
		}
	}
	return 0;
}

void PGXP_LineageObserveInstruction(uint32_t instr)
{
	unsigned dest;
	uint32_t bit;

	if (!lineage_written_register(instr, &dest) || dest == 0)
		return;
	bit = UINT32_C(1) << dest;
	if (!(lineage_reg_touched & bit))
		memset(&lineage_reg[dest], 0, sizeof(lineage_reg[dest]));
	lineage_reg_touched &= ~bit;
}

void PGXP_LineageLoad(uint32_t instr, uint32_t addr, uint32_t value)
{
	unsigned dest = (instr >> 16) & 31;
	PGXP_lineage* source;

	if (dest == 0)
		return;
	lineage_reg_touched |= UINT32_C(1) << dest;
	memset(&lineage_reg[dest], 0, sizeof(lineage_reg[dest]));
	/* LWL/LWR and sub-word loads are not exact full-word aliases. */
	if ((instr >> 26) != 0x23 || !lineage_enabled())
		return;
	source = lineage_memory_read(addr);
	if (source && (source->meta & PGXP_LINEAGE_VALID) &&
	    source->value == value)
		lineage_reg[dest] = *source;
}

void PGXP_LineageMemoryWrite(uint32_t addr)
{
	uint32_t index = lineage_memory_index(addr);

	if (index != PGXP_LINEAGE_INVALID_INDEX)
		lineage_memory_clear(index);
}

void PGXP_LineageStore(uint32_t instr, uint32_t value, uint32_t addr)
{
	unsigned source = (instr >> 16) & 31;
	uint32_t index = lineage_memory_index(addr);
	const PGXP_lineage* lineage = &lineage_reg[source];

	if (index == PGXP_LINEAGE_INVALID_INDEX)
		return;
	lineage_memory_clear(index);
	/* SWL/SWR share the ordinary PGXP store helper, but only an aligned SW
	 * is an exact architectural word alias. Partial stores retire lineage. */
	if ((instr >> 26) != 0x2b)
		return;
	if (!lineage_enabled() || !(lineage->meta & PGXP_LINEAGE_VALID) ||
	    lineage->value != value)
		return;
	lineage_memory[index] = *lineage;
	lineage_memory_set_valid(index);
}

void PGXP_LineageFIFOWrite(unsigned pos, uint32_t addr, uint32_t value)
{
	PGXP_lineage* source;

	if (pos >= 32)
		return;
	memset(&lineage_fifo[pos], 0, sizeof(lineage_fifo[pos]));
	if (!PGXP_FeatureEnabled(PGXP_FEATURE_EXACT_LINEAGE))
		return;
	source = lineage_memory_read(addr);
	if (!source || !(source->meta & PGXP_LINEAGE_VALID) ||
	    source->value != value)
		return;
	lineage_fifo[pos].lineage = *source;
	lineage_fifo[pos].word_addr = lineage_word_addr(addr);
}

void PGXP_LineageCBWrite(unsigned slot, unsigned fifo_pos)
{
	if (slot < 16 && fifo_pos < 32)
		lineage_cb[slot] = lineage_fifo[fifo_pos];
}

int PGXP_LineageRecoverVertex(unsigned slot, uint32_t value,
		float* x, float* y, float* z, int* valid_w)
{
	const PGXP_gpu_lineage* gpu;
	const PGXP_lineage* lineage;
	unsigned stage;

	if (!PGXP_FeatureEnabled(PGXP_FEATURE_EXACT_LINEAGE) || slot >= 16)
		return 0;
	gpu = &lineage_cb[slot];
	lineage = &gpu->lineage;
	stage = lineage_stage(lineage);
	if (!(lineage->meta & PGXP_LINEAGE_VALID) ||
	    (stage != 2 && stage != 3) || lineage->value != value ||
	    gpu->word_addr == UINT32_MAX || !isfinite(lineage->x) ||
	    !isfinite(lineage->y) || !isfinite(lineage->z))
		return 0;

	*x = lineage->x;
	*y = lineage->y;
	*z = lineage->z;
	*valid_w = (lineage->meta & PGXP_LINEAGE_VALID_W) != 0;
	return 1;
}

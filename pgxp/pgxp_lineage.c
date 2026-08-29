#include <math.h>
#include <string.h>

#include "pgxp_lineage.h"
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

enum PGXP_lineage_stage
{
	PGXP_LINEAGE_STAGE_NONE = 0,
	PGXP_LINEAGE_STAGE_PROJECTED = 1,
	PGXP_LINEAGE_STAGE_SHIFTED = 2,
	PGXP_LINEAGE_STAGE_RESTORED = 3
};

typedef struct
{
	float x;
	float y;
	float z;
	uint32_t value;
	uint32_t meta;
} PGXP_lineage;

/* Records are direct-addressed so provenance attached to display-list memory
 * remains valid until that architectural word is overwritten. The bitmap is
 * checked first, keeping invalid entries out of the hot memory path. */
static PGXP_lineage lineage_memory[PGXP_LINEAGE_MEMORY_WORDS];
static uint32_t lineage_memory_valid[(PGXP_LINEAGE_MEMORY_WORDS + 31) / 32];
static PGXP_lineage lineage_reg[32];
static PGXP_lineage lineage_fifo[32];
static PGXP_lineage lineage_cb[16];
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
	if (!precise || precise->value != value ||
	    (precise->flags & (VALID_01 | VALID_PROJECTION)) !=
	    (VALID_01 | VALID_PROJECTION) ||
	    !isfinite(precise->x) || !isfinite(precise->y) ||
	    !isfinite(precise->z))
		return;

	lineage->x = precise->x;
	lineage->y = precise->y;
	lineage->z = precise->z;
	lineage->value = value;
	lineage->meta = PGXP_LINEAGE_VALID |
		(PGXP_LINEAGE_STAGE_PROJECTED << PGXP_LINEAGE_STAGE_SHIFT);
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
	if (!(prior.meta & PGXP_LINEAGE_VALID) || shift != 5)
		return;

	if (!arithmetic &&
	    lineage_stage(&prior) == PGXP_LINEAGE_STAGE_PROJECTED &&
	    prior.value == before)
	{
		lineage_reg[dest] = prior;
		lineage_reg[dest].value = after;
		lineage_set_stage(&lineage_reg[dest], PGXP_LINEAGE_STAGE_SHIFTED);
	}
	else if (arithmetic && dest == source &&
	         lineage_stage(&prior) == PGXP_LINEAGE_STAGE_SHIFTED &&
	         prior.value == before)
	{
		lineage_reg[dest] = prior;
		lineage_reg[dest].value = after;
		lineage_set_stage(&lineage_reg[dest], PGXP_LINEAGE_STAGE_RESTORED);
	}
}

void PGXP_LineageIdentityMove(unsigned dest, unsigned source,
		uint32_t before, uint32_t after)
{
	PGXP_lineage prior;

	if (dest == 0 || dest >= 32 || source >= 32)
		return;
	prior = lineage_reg[source];
	lineage_reg_touched |= UINT32_C(1) << dest;
	memset(&lineage_reg[dest], 0, sizeof(lineage_reg[dest]));
	if (source == 0 || before != after ||
	    !(prior.meta & PGXP_LINEAGE_VALID) || prior.value != before)
		return;
	lineage_reg[dest] = prior;
	lineage_reg[dest].value = after;
}

void PGXP_LineageObserveRegisterWrite(unsigned dest)
{
	uint32_t bit;

	if (dest == 0 || dest >= 32)
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
	/* Partial and sub-word loads are not exact aliases of a stored word. */
	if ((instr >> 26) != 0x23)
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
	/* SWL and SWR share the ordinary store helper, but do not preserve an
	 * exact full-word relationship. */
	if ((instr >> 26) != 0x2b ||
	    !(lineage->meta & PGXP_LINEAGE_VALID) || lineage->value != value)
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
	source = lineage_memory_read(addr);
	if (!source || !(source->meta & PGXP_LINEAGE_VALID) ||
	    source->value != value)
		return;
	lineage_fifo[pos] = *source;
}

void PGXP_LineageCBWrite(unsigned slot, unsigned fifo_pos)
{
	if (slot < 16 && fifo_pos < 32)
		lineage_cb[slot] = lineage_fifo[fifo_pos];
}

int PGXP_LineageRecoverVertex(unsigned slot, uint32_t value,
		float* x, float* y, float* z, int* valid_w)
{
	const PGXP_lineage* lineage;
	unsigned stage;

	if (slot >= 16)
		return 0;
	lineage = &lineage_cb[slot];
	stage = lineage_stage(lineage);
	if (!(lineage->meta & PGXP_LINEAGE_VALID) ||
	    (stage != PGXP_LINEAGE_STAGE_SHIFTED &&
	     stage != PGXP_LINEAGE_STAGE_RESTORED) ||
	    lineage->value != value || !isfinite(lineage->x) ||
	    !isfinite(lineage->y) || !isfinite(lineage->z))
		return 0;

	*x = lineage->x;
	*y = lineage->y;
	*z = lineage->z;
	*valid_w = (lineage->meta & PGXP_LINEAGE_VALID_W) != 0;
	return 1;
}

#include <math.h>
#include <stdlib.h>
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
#define PGXP_LINEAGE_TAGGED          (UINT32_C(1) << 4)
#define PGXP_LINEAGE_TAG_MASK        UINT32_C(0x1f)

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
 * remains valid until that architectural word is overwritten. Allocate the
 * sidecar only while PGXP memory tracking is active; the bitmap is checked
 * first, keeping invalid entries out of the hot memory path. */
static PGXP_lineage* lineage_memory;
static uint32_t* lineage_memory_valid;
static PGXP_lineage lineage_reg[32];
static PGXP_lineage lineage_fifo[32];
static PGXP_lineage lineage_cb[16];
static uint32_t lineage_reg_touched;

/* Register lineage is replaced, never merged. Centralize the destination
 * bookkeeping so every new transport operation follows the same lifetime
 * rules. Callers which may write a source register must snapshot it first. */
static PGXP_lineage* lineage_begin_register_write(unsigned dest)
{
	PGXP_lineage* lineage;

	if (dest == 0 || dest >= 32)
		return NULL;
	lineage_reg_touched |= UINT32_C(1) << dest;
	lineage = &lineage_reg[dest];
	memset(lineage, 0, sizeof(*lineage));
	return lineage;
}

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

static uint32_t lineage_sra5(uint32_t value)
{
	uint32_t result = value >> 5;

	if (value & UINT32_C(0x80000000))
		result |= UINT32_C(0xf8000000);
	return result;
}

static uint32_t lineage_memory_index(uint32_t addr)
{
	uint32_t segment = addr >> 29;
	uint32_t physical;

	/* Only KUSEG, KSEG0, and KSEG1 have direct physical aliases. Do not
	 * fold KSEG2/3 addresses onto RAM merely because their low bits match. */
	if (segment != 0 && segment != 4 && segment != 5)
		return PGXP_LINEAGE_INVALID_INDEX;
	physical = addr & UINT32_C(0x1fffffff);

	/* The first 8 MiB are four mirrors of the 2 MiB main RAM. Addresses
	 * from 8 MiB through 16 MiB are unmapped and must not alias lineage. */
	if (physical < UINT32_C(0x00800000))
		return (physical & UINT32_C(0x001ffffc)) >> 2;
	if (physical >= UINT32_C(0x1f800000) &&
	    physical < UINT32_C(0x1f800400))
		return PGXP_LINEAGE_RAM_WORDS +
			((physical - UINT32_C(0x1f800000)) >> 2);
	return PGXP_LINEAGE_INVALID_INDEX;
}

static int lineage_memory_is_valid(uint32_t index)
{
	if (!lineage_memory_valid)
		return 0;
	return (lineage_memory_valid[index >> 5] >> (index & 31)) & 1;
}

static void lineage_memory_set_valid(uint32_t index)
{
	if (lineage_memory_valid)
		lineage_memory_valid[index >> 5] |=
			UINT32_C(1) << (index & 31);
}

static void lineage_memory_clear(uint32_t index)
{
	if (lineage_memory_valid)
		lineage_memory_valid[index >> 5] &=
			~(UINT32_C(1) << (index & 31));
}

static PGXP_lineage* lineage_memory_read(uint32_t addr)
{
	uint32_t index = lineage_memory_index(addr);

	if (!lineage_memory || index == PGXP_LINEAGE_INVALID_INDEX ||
	    !lineage_memory_is_valid(index))
		return NULL;
	return &lineage_memory[index];
}

static void lineage_copy_from_memory(PGXP_lineage* dest,
		uint32_t addr, uint32_t value)
{
	PGXP_lineage* source = lineage_memory_read(addr);

	if (!source || !(source->meta & PGXP_LINEAGE_VALID) ||
	    source->value != value)
		return;
	*dest = *source;
}

void PGXP_LineageReset(void)
{
	if (lineage_memory_valid)
		memset(lineage_memory_valid, 0,
			((PGXP_LINEAGE_MEMORY_WORDS + 31) / 32) *
			sizeof(*lineage_memory_valid));
	memset(lineage_reg, 0, sizeof(lineage_reg));
	memset(lineage_fifo, 0, sizeof(lineage_fifo));
	memset(lineage_cb, 0, sizeof(lineage_cb));
	lineage_reg_touched = 0;
}

int PGXP_LineageSetEnabled(int enabled)
{
	PGXP_lineage* memory;
	uint32_t* valid;

	if (!enabled)
	{
		free(lineage_memory);
		free(lineage_memory_valid);
		lineage_memory = NULL;
		lineage_memory_valid = NULL;
		PGXP_LineageReset();
		return 1;
	}
	if (lineage_memory && lineage_memory_valid)
		return 1;

	memory = (PGXP_lineage*)malloc(PGXP_LINEAGE_MEMORY_WORDS *
		sizeof(*memory));
	valid = (uint32_t*)calloc((PGXP_LINEAGE_MEMORY_WORDS + 31) / 32,
		sizeof(*valid));
	if (!memory || !valid)
	{
		free(memory);
		free(valid);
		lineage_memory = NULL;
		lineage_memory_valid = NULL;
		PGXP_LineageReset();
		return 0;
	}

	lineage_memory = memory;
	lineage_memory_valid = valid;
	PGXP_LineageReset();
	return 1;
}

void PGXP_LineageMFC2(uint32_t instr, uint32_t value,
		const PGXP_value* precise)
{
	unsigned dest = (instr >> 16) & 31;
	PGXP_lineage* lineage;

	lineage = lineage_begin_register_write(dest);
	if (!lineage)
		return;
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
	PGXP_lineage* lineage;

	if (dest == 0)
		return;
	prior = lineage_reg[source];
	lineage = lineage_begin_register_write(dest);
	if (!(prior.meta & PGXP_LINEAGE_VALID) || shift != 5)
		return;

	/* Keep only shifts that SRA 5 can reverse exactly. This proves that the
	 * packing step did not discard any coordinate bits. */
	if (!arithmetic &&
	    lineage_stage(&prior) == PGXP_LINEAGE_STAGE_PROJECTED &&
	    prior.value == before && lineage_sra5(after) == before)
	{
		*lineage = prior;
		lineage->value = after;
		lineage_set_stage(lineage, PGXP_LINEAGE_STAGE_SHIFTED);
	}
	else if (arithmetic && dest == source &&
	         lineage_stage(&prior) == PGXP_LINEAGE_STAGE_SHIFTED &&
	         prior.value == before && lineage_sra5(before) == after)
	{
		*lineage = prior;
		lineage->value = after;
		lineage_set_stage(lineage, PGXP_LINEAGE_STAGE_RESTORED);
	}
}

void PGXP_LineageTaggedAdd(uint32_t instr, uint32_t before,
		uint32_t after)
{
	unsigned source = (instr >> 21) & 31;
	unsigned dest = (instr >> 16) & 31;
	PGXP_lineage prior;
	PGXP_lineage* lineage;

	if (dest == 0)
		return;
	prior = lineage_reg[source];
	if (!(prior.meta & PGXP_LINEAGE_VALID) ||
	    lineage_stage(&prior) != PGXP_LINEAGE_STAGE_SHIFTED)
		return;

	lineage = lineage_begin_register_write(dest);
	if (prior.value != before)
		return;
	/* SLL 5 reserves bits 0..4. Changing only those bits cannot change the
	 * packed coordinate restored by SRA 5. Reject carries, borrows, and all
	 * other arithmetic rather than treating them as coordinate transport. */
	if ((before ^ after) & ~PGXP_LINEAGE_TAG_MASK)
		return;

	*lineage = prior;
	lineage->value = after;
	lineage->meta |= PGXP_LINEAGE_TAGGED;
}

void PGXP_LineageIdentityMove(unsigned dest, unsigned source,
		uint32_t before, uint32_t after)
{
	PGXP_lineage prior;
	PGXP_lineage* lineage;

	if (dest == 0 || dest >= 32 || source >= 32)
		return;
	prior = lineage_reg[source];
	lineage = lineage_begin_register_write(dest);
	if (source == 0 || before != after ||
	    !(prior.meta & PGXP_LINEAGE_VALID) || prior.value != before)
		return;
	*lineage = prior;
	lineage->value = after;
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
	PGXP_lineage* lineage;

	lineage = lineage_begin_register_write(dest);
	if (!lineage)
		return;
	/* Partial and sub-word loads are not exact aliases of a stored word. */
	if ((instr >> 26) != 0x23)
		return;
	lineage_copy_from_memory(lineage, addr, value);
}

void PGXP_LineageMemoryWrite(uint32_t addr)
{
	uint32_t index = lineage_memory_index(addr);

	if (index != PGXP_LINEAGE_INVALID_INDEX)
		lineage_memory_clear(index);
}

void PGXP_LineageMemoryWriteRange(uint32_t addr, uint32_t size)
{
	while (lineage_memory_valid && size)
	{
		uint32_t index = lineage_memory_index(addr);
		uint32_t step = 4 - (addr & 3);

		if (index != PGXP_LINEAGE_INVALID_INDEX)
			lineage_memory_clear(index);
		if (step > size)
			step = size;
		size -= step;
		if (addr > UINT32_MAX - step)
			break;
		addr += step;
	}
}

void PGXP_LineageScratchWrite(uint32_t offset, uint32_t size)
{
	PGXP_LineageMemoryWriteRange(
		UINT32_C(0x1f800000) | (offset & UINT32_C(0x3ff)), size);
}

void PGXP_LineageStore(uint32_t instr, uint32_t value, uint32_t addr)
{
	unsigned source = (instr >> 16) & 31;
	uint32_t index = lineage_memory_index(addr);
	const PGXP_lineage* lineage = &lineage_reg[source];

	if (!lineage_memory || !lineage_memory_valid ||
	    index == PGXP_LINEAGE_INVALID_INDEX)
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
	if (pos >= 32)
		return;
	memset(&lineage_fifo[pos], 0, sizeof(lineage_fifo[pos]));
	lineage_copy_from_memory(&lineage_fifo[pos], addr, value);
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
	if ((lineage->meta & PGXP_LINEAGE_TAGGED) &&
	    stage != PGXP_LINEAGE_STAGE_RESTORED)
		return 0;
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

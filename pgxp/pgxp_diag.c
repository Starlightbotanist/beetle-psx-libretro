#include "pgxp_diag.h"

#if PGXP_DIAG

#include <string.h>

#include <libretro.h>

#include "pgxp_cpu.h"
#include "pgxp_gpu.h"
#include "pgxp_gte.h"
#include "pgxp_main.h"
#include "pgxp_value.h"

extern retro_log_printf_t log_cb;
extern PGXP_value* GTE_data_reg;
extern PGXP_value* GTE_ctrl_reg;

#define PGXP_DIAG_WINDOW 60u
#define PGXP_DIAG_LOAD_OPS 7u
#define PGXP_DIAG_LOAD_SAMPLES 96u
#define PGXP_DIAG_ADDRESS_PAGES 16u
#define PGXP_DIAG_MEMORY_STATES 3u
#define PGXP_DIAG_STORE8_SLOTS (2048u * 1024u / 4u)

enum PGXP_diag_address_region
{
	PGXP_DIAG_ADDR_BIOS = 0,
	PGXP_DIAG_ADDR_PARALLEL,
	PGXP_DIAG_ADDR_CACHE_CONTROL,
	PGXP_DIAG_ADDR_OTHER,
	PGXP_DIAG_ADDR_REGIONS
};

typedef struct
{
	uint32_t page;
	uint32_t count;
} PGXP_diag_address_page;

typedef struct
{
	uint32_t word_addr;
	uint32_t byte_addr;
	uint32_t value;
	uint32_t before_value;
	uint32_t before_flags;
	uint32_t before_count;
	uint32_t invalid_count;
	uint32_t mode_frame;
	uint32_t valid;
} PGXP_diag_store8;

typedef struct
{
	uint32_t word_addr;
	uint32_t mfc2_value;
	uint32_t sll_value;
	uint32_t sra_value;
	uint32_t gte_reg;
	uint32_t stage;
	uint32_t valid;
	uint32_t transform_observed;
	uint32_t current_value;
	uint32_t depth;
	uint32_t chain_hash;
} PGXP_diag_lineage;

typedef struct
{
	uint32_t addr;
	uint32_t value;
	uint32_t shadow_value;
	uint32_t shadow_flags;
	uint32_t shadow_count;
	PGXP_diag_store8 store8;
	uint32_t store8_match;
	PGXP_diag_lineage lineage;
} PGXP_diag_gpu_provenance;

typedef struct
{
	uint64_t mem_reads;
	uint64_t mem_writes;
	uint64_t mem_invalid_reads;
	uint64_t mem_invalid_writes;
	uint64_t cpu_loads;
	uint64_t cpu_load_untracked;
	uint64_t cpu_load_invalid_result;
	uint64_t cpu_load_op[PGXP_DIAG_LOAD_OPS];
	uint64_t cpu_load_state[PGXP_DIAG_MEMORY_STATES];
	uint64_t cpu_load_invalid_state[PGXP_DIAG_MEMORY_STATES];
	uint64_t cpu_load_invalid_state_op[PGXP_DIAG_MEMORY_STATES]
		[PGXP_DIAG_LOAD_OPS];
	uint64_t cpu_load_region[PGXP_DIAG_ADDR_REGIONS];
	PGXP_diag_address_page cpu_load_page[PGXP_DIAG_ADDRESS_PAGES];
	uint64_t gte_vertices;
	uint64_t vertex_tracked;
	uint64_t vertex_cache;
	uint64_t vertex_native;
	uint64_t vertex_native_invalid_xy;
	uint64_t vertex_native_value_mismatch;
	uint64_t vertex_native_both;
	uint64_t vertex_valid_w;
	uint64_t nclip_compares;
	uint64_t nclip_sign_disagreements;
	uint64_t lineage_mfc2;
	uint64_t lineage_sll5_candidates;
	uint64_t lineage_sll5_matches;
	uint64_t lineage_sra5_candidates;
	uint64_t lineage_sra5_matches;
	uint64_t lineage_preserve_sll5;
	uint64_t lineage_preserve_sra5;
	uint64_t lineage_identity_candidates;
	uint64_t lineage_identity_matches;
	uint64_t lineage_identity_preserve;
	uint64_t lineage_drops;
	uint64_t lineage_transforms;
	uint64_t lineage_transform_state[4][4];
	uint64_t lineage_transform_semantic[8];
	uint64_t lineage_transform_propagated;
	uint64_t lineage_store2;
	uint64_t lineage_store3;
	uint64_t lineage_fifo;
	uint64_t event_hash;
} PGXP_diag_window;

static PGXP_diag_window window;
static uint64_t frame_number;
static uint32_t mode_frame;
static int last_backend = -1;
static unsigned last_mode = ~0u;
static uint32_t load_samples[PGXP_DIAG_MEMORY_STATES];
static uint32_t dispatch_samples;
static uint32_t vertex_samples;
static uint32_t cache_vertex_samples;
static uint32_t lineage_fifo_samples;
static uint32_t lineage_vertex_samples;
static uint32_t lineage_drop_samples;
static uint32_t lineage_transform_samples[128][16];
static uint32_t vertex_sample_addr[PGXP_DIAG_LOAD_SAMPLES];
static uint32_t vertex_sample_value[PGXP_DIAG_LOAD_SAMPLES];
static PGXP_diag_gpu_provenance fifo_provenance[32];
static PGXP_diag_store8 store8_provenance[PGXP_DIAG_STORE8_SLOTS];
static PGXP_diag_lineage lineage_reg[32];
static uint32_t lineage_reg_touched;
static uint32_t lineage_pre_instr;
static uint32_t lineage_pre_mask;
static uint32_t lineage_pre_native[32];
static PGXP_value lineage_pre_shadow[32];
static PGXP_diag_lineage lineage_pre_lineage[32];
static PGXP_diag_lineage lineage_mem[PGXP_DIAG_STORE8_SLOTS];
static PGXP_diag_gpu_provenance cb_provenance[16];

static int vertex_sample_seen(uint32_t addr, uint32_t value)
{
	uint32_t i;

	for (i = 0; i < vertex_samples; i++)
		if (vertex_sample_addr[i] == addr && vertex_sample_value[i] == value)
			return 1;
	return 0;
}

static uint64_t hash_bytes(uint64_t hash, const void* data, size_t size)
{
	const uint8_t* bytes = (const uint8_t*)data;
	size_t i;

	for (i = 0; i < size; i++)
	{
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static void hash_event(uint8_t type, uint32_t a, uint32_t b)
{
	window.event_hash = hash_bytes(window.event_hash, &type, sizeof(type));
	window.event_hash = hash_bytes(window.event_hash, &a, sizeof(a));
	window.event_hash = hash_bytes(window.event_hash, &b, sizeof(b));
}

static const char* backend_name(int backend)
{
	switch (backend)
	{
		case 1: return "Max Performance";
		case 2: return "Lightrec Interpreter";
		default: return "Disabled (Beetle Interpreter)";
	}
}

static uint32_t count_valid(const PGXP_value* values, uint32_t count,
		uint32_t mask)
{
	uint32_t valid = 0;
	uint32_t i;

	for (i = 0; i < count; i++)
		valid += (values[i].flags & mask) == mask;

	return valid;
}

static unsigned load_op_index(uint32_t instr)
{
	static const uint8_t index[8] = { 0, 1, 2, 3, 4, 5, 6, 0 };
	uint32_t opcode = instr >> 26;

	return (opcode >= 0x20 && opcode <= 0x26) ? index[opcode - 0x20] : 0;
}

static const char* load_op_name(uint32_t instr)
{
	static const char* names[PGXP_DIAG_LOAD_OPS] =
		{ "LB", "LH", "LWL", "LW", "LBU", "LHU", "LWR" };

	return names[load_op_index(instr)];
}

static unsigned address_region(uint32_t addr)
{
	uint32_t physical = addr & UINT32_C(0x1fffffff);

	if (physical >= UINT32_C(0x1fc00000) &&
	    physical <= UINT32_C(0x1fc7ffff))
		return PGXP_DIAG_ADDR_BIOS;
	if (physical >= UINT32_C(0x1f000000) &&
	    physical <= UINT32_C(0x1f00ffff))
		return PGXP_DIAG_ADDR_PARALLEL;
	if (addr == UINT32_C(0xfffe0130))
		return PGXP_DIAG_ADDR_CACHE_CONTROL;
	return PGXP_DIAG_ADDR_OTHER;
}

static void count_address_page(uint32_t addr)
{
	uint32_t page = addr >> 12;
	uint32_t free_slot = PGXP_DIAG_ADDRESS_PAGES;
	uint32_t i;

	for (i = 0; i < PGXP_DIAG_ADDRESS_PAGES; i++)
	{
		if (window.cpu_load_page[i].count &&
		    window.cpu_load_page[i].page == page)
		{
			window.cpu_load_page[i].count++;
			return;
		}
		if (!window.cpu_load_page[i].count &&
		    free_slot == PGXP_DIAG_ADDRESS_PAGES)
			free_slot = i;
	}

	if (free_slot < PGXP_DIAG_ADDRESS_PAGES)
	{
		window.cpu_load_page[free_slot].page = page;
		window.cpu_load_page[free_slot].count = 1;
	}
}

static uint32_t invalid_address_register_mask(void)
{
	uint32_t mask = 0;
	uint32_t i;

	for (i = 1; i < 32; i++)
		if (CPU_reg[i].gFlags == INVALID_ADDRESS)
			mask |= UINT32_C(1) << i;
	return mask;
}

static uint32_t invalid_register_mask(void)
{
	uint32_t mask = 0;
	uint32_t i;

	for (i = 1; i < 32; i++)
		if ((CPU_reg[i].flags & VALID_01) != VALID_01)
			mask |= UINT32_C(1) << i;
	return mask;
}

void PGXP_DiagInit(void)
{
	memset(&window, 0, sizeof(window));
	window.event_hash = UINT64_C(1469598103934665603);
	frame_number = 0;
	mode_frame = 0;
	last_backend = -1;
	last_mode = ~0u;
	memset(load_samples, 0, sizeof(load_samples));
	memset(fifo_provenance, 0, sizeof(fifo_provenance));
	memset(store8_provenance, 0, sizeof(store8_provenance));
	memset(lineage_reg, 0, sizeof(lineage_reg));
	lineage_reg_touched = 0;
	lineage_pre_instr = 0;
	lineage_pre_mask = 0;
	memset(lineage_pre_native, 0, sizeof(lineage_pre_native));
	memset(lineage_pre_shadow, 0, sizeof(lineage_pre_shadow));
	memset(lineage_pre_lineage, 0, sizeof(lineage_pre_lineage));
	memset(lineage_mem, 0, sizeof(lineage_mem));
	memset(cb_provenance, 0, sizeof(cb_provenance));
	dispatch_samples = 0;
	vertex_samples = 0;
	cache_vertex_samples = 0;
	lineage_fifo_samples = 0;
	lineage_vertex_samples = 0;
	lineage_drop_samples = 0;
	memset(lineage_transform_samples, 0,
		sizeof(lineage_transform_samples));
}

uint32_t PGXP_DiagCPUInvalidMask(void)
{
	return invalid_register_mask();
}

void PGXP_DiagCPUDispatch(uint32_t instr, uint32_t addr, unsigned dest,
		uint32_t before_mask, uint32_t before_flags,
		uint16_t before_gflags)
{
	uint32_t after_mask = invalid_register_mask();
	uint32_t after_flags = dest < 32 ? CPU_reg[dest].flags : 0;
	uint16_t after_gflags = dest < 32 ? CPU_reg[dest].gFlags : 0;

	if (dest < 32)
	{
		uint32_t bit = UINT32_C(1) << dest;
		if (!(lineage_reg_touched & bit))
		{
			if (lineage_reg[dest].valid)
			{
				uint32_t expected = lineage_reg[dest].current_value;
				window.lineage_drops++;
				if (lineage_drop_samples < PGXP_DIAG_LOAD_SAMPLES &&
				    log_cb)
				{
					log_cb(RETRO_LOG_INFO,
						"[pgxp_lineage_drop] n=%u mf=%u instr=%08x "
						"op=%02x func=%02x rs=%u rt=%u rd=%u dest=%u "
						"stage=%u gte=%u old=%08x new=%08x flags=%08x\n",
						lineage_drop_samples + 1, mode_frame, instr,
						instr >> 26, instr & 0x3f,
						(instr >> 21) & 31, (instr >> 16) & 31,
						(instr >> 11) & 31, dest,
						lineage_reg[dest].stage,
						lineage_reg[dest].gte_reg, expected,
						CPU_reg[dest].value, CPU_reg[dest].flags);
					lineage_drop_samples++;
				}
			}
			memset(&lineage_reg[dest], 0, sizeof(lineage_reg[dest]));
		}
		lineage_reg_touched &= ~bit;
	}

	if (dispatch_samples >= 32 || !log_cb)
		return;
	if (!((before_mask ^ after_mask) & (UINT32_C(1) << 7)))
		return;

	log_cb(RETRO_LOG_INFO,
		"[pgxp_cpu_change] n=%u mf=%u instr=%08x op=%02x "
		"rs=%u rt=%u rd=%u dest=%u addr=%08x mask=%08x/%08x "
		"flags=%08x/%08x gflags=%u/%u\n",
		dispatch_samples + 1, mode_frame, instr, instr >> 26,
		(instr >> 21) & 0x1f, (instr >> 16) & 0x1f,
		(instr >> 11) & 0x1f, dest, addr, before_mask, after_mask,
		before_flags, after_flags, before_gflags, after_gflags);
	dispatch_samples++;
}

void PGXP_DiagCPULoad(uint32_t instr, uint32_t addr, uint32_t value,
		const PGXP_value* result, int memory_state)
{
	unsigned op_index = load_op_index(instr);
	unsigned reg = (instr >> 16) & 0x1f;
	int invalid_result = (result->flags & VALID_01) != VALID_01;

	if (memory_state < 0 ||
	    (unsigned)memory_state >= PGXP_DIAG_MEMORY_STATES)
		memory_state = 0;

	window.cpu_loads++;
	window.cpu_load_op[op_index]++;
	window.cpu_load_state[memory_state]++;
	if (memory_state == 0)
	{
		window.cpu_load_untracked++;
		window.cpu_load_region[address_region(addr)]++;
		count_address_page(addr);
	}
	if (invalid_result)
	{
		window.cpu_load_invalid_result++;
		window.cpu_load_invalid_state[memory_state]++;
		window.cpu_load_invalid_state_op[memory_state][op_index]++;
	}

	/* Keep separate bounded samples for unsupported, never-written, and
	 * previously-written shadow memory. Otherwise early BIOS traffic would
	 * consume the sample budget before gameplay reaches stale shadow data. */
	if (invalid_result && load_samples[memory_state] <
	    PGXP_DIAG_LOAD_SAMPLES / PGXP_DIAG_MEMORY_STATES && log_cb)
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_load] state=%d n=%u mf=%u op=%s rt=%u "
			"addr=%08x value=%08x "
			"flags=%08x gflags=%u\n",
			memory_state, load_samples[memory_state] + 1, mode_frame,
			load_op_name(instr), reg,
			addr, value, result->flags, result->gFlags);
		load_samples[memory_state]++;
	}
}

void PGXP_DiagMemoryRead(uint32_t addr, uint32_t value, int valid_address)
{
	window.mem_reads++;
	if (!valid_address)
		window.mem_invalid_reads++;
	hash_event(1, addr, value);
}

void PGXP_DiagMemoryWrite(uint32_t addr, uint32_t value, int valid_address)
{
	window.mem_writes++;
	if (!valid_address)
		window.mem_invalid_writes++;
	hash_event(2, addr, value);
}

static uint32_t gpu_source_word_addr(uint32_t addr)
{
	switch (addr >> 24)
	{
		case 0x00:
		case 0x80:
		case 0xa0:
			return addr & UINT32_C(0x001ffffc);
		default:
			return addr & ~UINT32_C(3);
	}
}

void PGXP_DiagMFC2(uint32_t instr, uint32_t value)
{
	uint32_t dest = (instr >> 16) & 31;
	PGXP_diag_lineage* lineage = &lineage_reg[dest];

	memset(lineage, 0, sizeof(*lineage));
	window.lineage_mfc2++;
	lineage->mfc2_value = value;
	lineage->gte_reg = (instr >> 11) & 31;
	lineage->stage = 1;
	lineage->valid = 1;
	lineage->current_value = value;
	lineage->chain_hash = UINT32_C(2166136261) ^ instr ^ value;
	lineage_reg_touched |= UINT32_C(1) << dest;
}

void PGXP_DiagShift(uint32_t instr, uint32_t before, uint32_t after,
		int arithmetic)
{
	uint32_t source = (instr >> 16) & 31;
	uint32_t dest = (instr >> 11) & 31;
	uint32_t shift = (instr >> 6) & 31;
	PGXP_diag_lineage prior = lineage_reg[source];

	lineage_reg_touched |= UINT32_C(1) << dest;
	memset(&lineage_reg[dest], 0, sizeof(lineage_reg[dest]));
	if (!arithmetic && shift == 5)
		window.lineage_sll5_candidates++;
	else if (arithmetic && shift == 5 && dest == source)
		window.lineage_sra5_candidates++;
	if (!arithmetic && shift == 5 && prior.valid && prior.stage == 1 &&
	    prior.mfc2_value == before)
	{
		lineage_reg[dest] = prior;
		lineage_reg[dest].sll_value = after;
		lineage_reg[dest].stage = 2;
		lineage_reg[dest].current_value = after;
		lineage_reg[dest].depth++;
		window.lineage_sll5_matches++;
	}
	else if (arithmetic && shift == 5 && dest == source &&
	         prior.valid && prior.stage == 2 && prior.sll_value == before)
	{
		lineage_reg[dest] = prior;
		lineage_reg[dest].sra_value = after;
		lineage_reg[dest].stage = 3;
		lineage_reg[dest].current_value = after;
		lineage_reg[dest].depth++;
		window.lineage_sra5_matches++;
	}
}

int PGXP_DiagPreserveShift(uint32_t instr, uint32_t before,
		uint32_t after, int arithmetic)
{
	uint32_t source = (instr >> 16) & 31;
	uint32_t dest = (instr >> 11) & 31;
	PGXP_value result;

	Validate(&CPU_reg[source], before);
	result = CPU_reg[source];
	PGXP_DiagShift(instr, before, after, arithmetic);
	if ((result.flags & VALID_01) != VALID_01 || result.value != before ||
	    !lineage_reg[dest].valid ||
	    lineage_reg[dest].stage != (arithmetic ? 3u : 2u))
		return 0;

	result.value = after;
	CPU_reg[dest] = result;
	if (arithmetic)
		window.lineage_preserve_sra5++;
	else
		window.lineage_preserve_sll5++;
	return 1;
}

void PGXP_DiagIdentityMove(unsigned dest, unsigned source,
		uint32_t before, uint32_t after)
{
	PGXP_diag_lineage prior = lineage_reg[source];
	PGXP_value result;
	uint32_t expected;

	window.lineage_identity_candidates++;
	lineage_reg_touched |= UINT32_C(1) << dest;
	memset(&lineage_reg[dest], 0, sizeof(lineage_reg[dest]));
	if (source == 0 || before != after || !prior.valid)
		return;
	expected = prior.current_value;
	if (expected != before)
		return;
	window.lineage_identity_matches++;

	Validate(&CPU_reg[source], before);
	result = CPU_reg[source];
	if ((result.flags & VALID_01) != VALID_01 || result.value != before)
		return;
	result.value = after;
	CPU_reg[dest] = result;
	lineage_reg[dest] = prior;
	window.lineage_identity_preserve++;
	return;
}

void PGXP_DiagBeginInstruction(uint32_t instr, const uint32_t* gpr)
{
	uint32_t rs = (instr >> 21) & 31;
	uint32_t rt = (instr >> 16) & 31;

	lineage_pre_instr = instr;
	lineage_pre_mask = (UINT32_C(1) << rs) | (UINT32_C(1) << rt);
	lineage_pre_shadow[rs] = CPU_reg[rs];
	lineage_pre_shadow[rt] = CPU_reg[rt];
	lineage_pre_native[rs] = gpr[rs];
	lineage_pre_native[rt] = gpr[rt];
	lineage_pre_lineage[rs] = lineage_reg[rs];
	lineage_pre_lineage[rt] = lineage_reg[rt];
}

void PGXP_DiagObserveInstruction(uint32_t instr, const uint32_t* gpr)
{
	uint32_t primary = instr >> 26;
	uint32_t special = instr & 63;
	uint32_t dest = 0, source = 0, other = 0;
	uint32_t expected, transform_class, semantic = 0;
	uint32_t source_state, result_state;
	uint32_t* transition_samples;
	PGXP_diag_lineage prior;
	const PGXP_value *source_shadow, *other_shadow, *result_shadow;
	int identity = 0;

	if (primary == 0)
	{
		switch (special)
		{
			case 0x00: case 0x02: case 0x03:
				dest = (instr >> 11) & 31; source = (instr >> 16) & 31; break;
			case 0x04: case 0x06: case 0x07:
				dest = (instr >> 11) & 31; source = (instr >> 16) & 31;
				other = (instr >> 21) & 31; break;
			case 0x20: case 0x21: case 0x22: case 0x23:
			case 0x24: case 0x25: case 0x26: case 0x27:
			case 0x2a: case 0x2b:
				dest = (instr >> 11) & 31; source = (instr >> 21) & 31;
				other = (instr >> 16) & 31;
				identity = (special == 0x20 || special == 0x21 ||
					special == 0x25 || special == 0x26) &&
					(source == 0 || other == 0);
				break;
			default: return;
		}
	}
	else
	{
		switch (primary)
		{
			case 0x08: case 0x09: case 0x0a: case 0x0b:
			case 0x0c: case 0x0d: case 0x0e:
				dest = (instr >> 16) & 31; source = (instr >> 21) & 31;
				identity = (primary == 0x08 || primary == 0x09 ||
					primary == 0x0d || primary == 0x0e) &&
					(instr & 0xffff) == 0;
				break;
			default: return;
		}
	}
	if (dest == 0 || identity)
		return;

	/* PGXP_DiagPreserveShift runs before the architectural GPR write and
	 * has already advanced a recognized pair to stage 2 or 3.  Replacing
	 * it below with the BEGIN_OPF snapshot would roll the lineage back to
	 * stage 1, making every following SRA5 miss. */
	if (primary == 0 &&
	    ((special == 0x00 && lineage_reg[dest].valid &&
	      lineage_reg[dest].stage == 2) ||
	     (special == 0x03 && lineage_reg[dest].valid &&
	      lineage_reg[dest].stage == 3)))
		return;

	if (lineage_pre_instr != instr)
		return;
	if (!lineage_pre_lineage[source].valid &&
	    lineage_pre_lineage[other].valid)
		source = other;
	if (!(lineage_pre_mask & (UINT32_C(1) << source)) ||
	    !lineage_pre_lineage[source].valid)
		return;

	prior = lineage_pre_lineage[source];
	expected = prior.current_value;
	source_shadow = &lineage_pre_shadow[source];
	other_shadow = &lineage_pre_shadow[other];
	result_shadow = &CPU_reg[dest];
	source_state = (((source_shadow->flags & VALID_01) == VALID_01) ? 2u : 0u) |
		(source_shadow->value == expected);
	result_state = (((result_shadow->flags & VALID_01) == VALID_01) ? 2u : 0u) |
		(result_shadow->value == gpr[dest]);

	if (gpr[dest] == expected)
		semantic = 1;
	else if (other && gpr[dest] == lineage_pre_native[other])
		semantic = 2;
	else if (other && (gpr[dest] ==
		((expected & 0xffff) | (lineage_pre_native[other] & 0xffff0000)) ||
		gpr[dest] == ((lineage_pre_native[other] & 0xffff) | (expected & 0xffff0000))))
		semantic = 3;
	else if ((gpr[dest] & 0xffff) == (expected & 0xffff) ||
		(gpr[dest] & 0xffff0000) == (expected & 0xffff0000))
		semantic = 4;
	else if (primary == 0 && special <= 7)
		semantic = 5;
	else if ((primary == 0 && (special == 0x2a || special == 0x2b)) ||
		primary == 0x0a || primary == 0x0b)
		semantic = 6;
	else
		semantic = 7;

	window.lineage_transforms++;
	window.lineage_transform_state[source_state][result_state]++;
	window.lineage_transform_semantic[semantic]++;
	if (result_state == 3)
		window.lineage_transform_propagated++;

	transform_class = primary == 0 ? special : 0x40 + primary;
	transition_samples = &lineage_transform_samples[transform_class]
		[source_state * 4 + result_state];
	if (*transition_samples < 4 && log_cb)
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_lineage_transform] class=%u n=%u mf=%u instr=%08x "
			"op=%02x func=%02x rs=%u rt=%u rd=%u src=%u dest=%u "
			"stage=%u depth=%u chain=%08x gte=%u value=%08x other=%08x "
			"result=%08x state=%u/%u semantic=%u "
			"src_shadow=%08x/%08x/%.3f/%.3f "
			"other_shadow=%08x/%08x/%.3f/%.3f "
			"result_shadow=%08x/%08x/%.3f/%.3f\n",
			transform_class, *transition_samples + 1, mode_frame, instr,
			primary, special, (instr >> 21) & 31, (instr >> 16) & 31,
			(instr >> 11) & 31, source, dest, prior.stage, prior.depth,
			prior.chain_hash, prior.gte_reg, expected, lineage_pre_native[other], gpr[dest],
			source_state, result_state, semantic,
			source_shadow->value, source_shadow->flags,
			source_shadow->x, source_shadow->y,
			other_shadow->value, other_shadow->flags,
			other_shadow->x, other_shadow->y,
			result_shadow->value, result_shadow->flags,
			result_shadow->x, result_shadow->y);
		(*transition_samples)++;
	}

	lineage_reg[dest] = prior;
	lineage_reg[dest].current_value = gpr[dest];
	lineage_reg[dest].depth++;
	lineage_reg[dest].transform_observed = 1;
	lineage_reg[dest].chain_hash =
		(prior.chain_hash ^ instr ^ gpr[dest]) * UINT32_C(16777619);
	lineage_reg_touched |= UINT32_C(1) << dest;
}

void PGXP_DiagLineageStore(uint32_t instr, uint32_t value, uint32_t addr)
{
	uint32_t source = (instr >> 16) & 31;
	uint32_t word_addr = gpu_source_word_addr(addr);
	PGXP_diag_lineage* dest;
	const PGXP_diag_lineage* prior = &lineage_reg[source];

	if (word_addr >= UINT32_C(0x00200000))
		return;
	dest = &lineage_mem[word_addr >> 2];
	uint32_t expected = prior->current_value;

	memset(dest, 0, sizeof(*dest));
	if (prior->valid && expected == value)
	{
		*dest = *prior;
		dest->word_addr = word_addr;
		if (prior->stage == 3)
			window.lineage_store3++;
		else
			window.lineage_store2++;
	}
}

void PGXP_DiagStore8(uint32_t addr, uint8_t value,
		uint32_t invalid_count, const PGXP_value* shadow)
{
	uint32_t word_addr = gpu_source_word_addr(addr);
	PGXP_diag_store8* provenance = &store8_provenance
		[(word_addr >> 2) & (PGXP_DIAG_STORE8_SLOTS - 1)];

	provenance->word_addr = word_addr;
	provenance->byte_addr = addr;
	provenance->value = value;
	provenance->before_value = shadow ? shadow->value : 0;
	provenance->before_flags = shadow ? shadow->flags : 0;
	provenance->before_count = shadow ? shadow->count : 0;
	provenance->invalid_count = invalid_count;
	provenance->mode_frame = mode_frame;
	provenance->valid = 1;
}

void PGXP_DiagFIFOWrite(unsigned pos, uint32_t addr, uint32_t value,
		const PGXP_value* shadow)
{
	PGXP_diag_gpu_provenance* provenance;

	if (pos >= 32)
		return;
	provenance = &fifo_provenance[pos];
	provenance->addr = addr;
	provenance->value = value;
	provenance->shadow_value = shadow ? shadow->value : 0;
	provenance->shadow_flags = shadow ? shadow->flags : 0;
	provenance->shadow_count = shadow ? shadow->count : 0;

	{
		uint32_t word_addr = gpu_source_word_addr(addr);
		const PGXP_diag_store8* store8 = &store8_provenance
			[(word_addr >> 2) & (PGXP_DIAG_STORE8_SLOTS - 1)];

		memset(&provenance->store8, 0, sizeof(provenance->store8));
		memset(&provenance->lineage, 0, sizeof(provenance->lineage));
		if (word_addr < UINT32_C(0x00200000))
		{
			provenance->lineage = lineage_mem[word_addr >> 2];
			if (!provenance->lineage.valid ||
			    provenance->lineage.word_addr != word_addr ||
			    provenance->lineage.current_value != value)
				memset(&provenance->lineage, 0,
					sizeof(provenance->lineage));
			if (provenance->lineage.valid)
			{
				window.lineage_fifo++;
				if (lineage_fifo_samples < PGXP_DIAG_LOAD_SAMPLES && log_cb)
				{
					log_cb(RETRO_LOG_INFO,
						"[pgxp_lineage_fifo] n=%u mf=%u pos=%u "
						"addr=%08x value=%08x stage=%u gte=%u "
						"mfc2=%08x sll=%08x sra=%08x\n",
						lineage_fifo_samples + 1, mode_frame, pos,
						addr, value, provenance->lineage.stage,
						provenance->lineage.gte_reg,
						provenance->lineage.mfc2_value,
						provenance->lineage.sll_value,
						provenance->lineage.sra_value);
					lineage_fifo_samples++;
				}
			}
		}
		provenance->store8_match = 0;
		if (store8->valid && store8->word_addr == word_addr)
		{
			provenance->store8 = *store8;
			provenance->store8_match =
				shadow && shadow->count == store8->invalid_count;
		}
	}
}

void PGXP_DiagCBWrite(unsigned slot, unsigned fifo_pos)
{
	if (slot < 16 && fifo_pos < 32)
		cb_provenance[slot] = fifo_provenance[fifo_pos];
}

void PGXP_DiagGTEVertex(float x, float y, float z, uint32_t value)
{
	window.gte_vertices++;
	hash_event(3, value, 0);
	window.event_hash = hash_bytes(window.event_hash, &x, sizeof(x));
	window.event_hash = hash_bytes(window.event_hash, &y, sizeof(y));
	window.event_hash = hash_bytes(window.event_hash, &z, sizeof(z));
}

void PGXP_DiagVertex(enum PGXP_diag_vertex_source source,
		unsigned slot, uint32_t value, const PGXP_value* shadow,
		float x, float y, float w, int valid_w,
		int valid_xy, int value_match)
{
	switch (source)
	{
		case PGXP_DIAG_VERTEX_TRACKED: window.vertex_tracked++; break;
		case PGXP_DIAG_VERTEX_CACHE:   window.vertex_cache++;   break;
		default:                       window.vertex_native++;  break;
	}
	if (valid_w)
		window.vertex_valid_w++;
	if (cb_provenance[slot].lineage.valid &&
	    lineage_vertex_samples < PGXP_DIAG_LOAD_SAMPLES && log_cb)
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_lineage_vertex] n=%u mf=%u slot=%u source=%u "
			"psx=%08x shadow=%08x flags=%08x valid=%d/%d "
			"stage=%u gte=%u mfc2=%08x sll=%08x sra=%08x\n",
			lineage_vertex_samples + 1, mode_frame, slot, source,
			value, shadow->value, shadow->flags, valid_xy, value_match,
			cb_provenance[slot].lineage.stage,
			cb_provenance[slot].lineage.gte_reg,
			cb_provenance[slot].lineage.mfc2_value,
			cb_provenance[slot].lineage.sll_value,
			cb_provenance[slot].lineage.sra_value);
		lineage_vertex_samples++;
	}
	if (source == PGXP_DIAG_VERTEX_NATIVE)
	{
		if (!valid_xy)
			window.vertex_native_invalid_xy++;
		if (!value_match)
			window.vertex_native_value_mismatch++;
		if (!valid_xy && !value_match)
			window.vertex_native_both++;

		if (vertex_samples < PGXP_DIAG_LOAD_SAMPLES && log_cb &&
		    !vertex_sample_seen(cb_provenance[slot].addr,
			    cb_provenance[slot].value))
		{
			vertex_sample_addr[vertex_samples] = cb_provenance[slot].addr;
			vertex_sample_value[vertex_samples] = cb_provenance[slot].value;
			log_cb(RETRO_LOG_INFO,
				"[pgxp_vertex_native] n=%u mf=%u slot=%u "
				"psx=%08x shadow=%08x flags=%08x gflags=%u "
				"lflags=%u hflags=%u count=%u valid=%d/%d\n",
				vertex_samples + 1, mode_frame, slot, value,
				shadow->value, shadow->flags, shadow->gFlags,
				shadow->lFlags, shadow->hFlags, shadow->count,
				valid_xy, value_match);
			log_cb(RETRO_LOG_INFO,
				"[pgxp_vertex_source] n=%u mf=%u slot=%u "
				"src_addr=%08x src_psx=%08x "
				"src_shadow=%08x src_flags=%08x src_count=%u\n",
				vertex_samples + 1, mode_frame, slot,
				cb_provenance[slot].addr,
				cb_provenance[slot].value,
				cb_provenance[slot].shadow_value,
				cb_provenance[slot].shadow_flags,
				cb_provenance[slot].shadow_count);
			log_cb(RETRO_LOG_INFO,
				"[pgxp_vertex_lineage] n=%u mf=%u slot=%u seen=%u "
				"stage=%u gte=%u mfc2=%08x sll=%08x sra=%08x\n",
				vertex_samples + 1, mode_frame, slot,
				cb_provenance[slot].lineage.valid,
				cb_provenance[slot].lineage.stage,
				cb_provenance[slot].lineage.gte_reg,
				cb_provenance[slot].lineage.mfc2_value,
				cb_provenance[slot].lineage.sll_value,
				cb_provenance[slot].lineage.sra_value);
			log_cb(RETRO_LOG_INFO,
				"[pgxp_vertex_store8] n=%u mf=%u slot=%u seen=%u match=%u "
				"store_mf=%u addr=%08x byte=%02x before=%08x flags=%08x "
				"count=%u\n",
				vertex_samples + 1, mode_frame, slot,
				cb_provenance[slot].store8.valid,
				cb_provenance[slot].store8_match,
				cb_provenance[slot].store8.mode_frame,
				cb_provenance[slot].store8.byte_addr,
				cb_provenance[slot].store8.value,
				cb_provenance[slot].store8.before_value,
				cb_provenance[slot].store8.before_flags,
				cb_provenance[slot].store8.before_count);
			vertex_samples++;
		}
	}
	else if (source == PGXP_DIAG_VERTEX_CACHE &&
	         cache_vertex_samples < PGXP_DIAG_LOAD_SAMPLES && log_cb)
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_vertex_cache] n=%u mf=%u slot=%u psx=%08x "
			"ram_shadow=%08x flags=%08x valid=%d/%d "
			"cache_xyz=%.3f/%.3f/%.3f src_addr=%08x src_psx=%08x "
			"lineage=%u/%u/%u/%08x\n",
			cache_vertex_samples + 1, mode_frame, slot, value,
			shadow->value, shadow->flags, valid_xy, value_match,
			x, y, w, cb_provenance[slot].addr,
			cb_provenance[slot].value,
			cb_provenance[slot].lineage.valid,
			cb_provenance[slot].lineage.stage,
			cb_provenance[slot].lineage.depth,
			cb_provenance[slot].lineage.chain_hash);
		cache_vertex_samples++;
	}
	hash_event((uint8_t)(4 + source), value, (uint32_t)valid_w);
	window.event_hash = hash_bytes(window.event_hash, &x, sizeof(x));
	window.event_hash = hash_bytes(window.event_hash, &y, sizeof(y));
	window.event_hash = hash_bytes(window.event_hash, &w, sizeof(w));
}

void PGXP_DiagNCLIP(int32_t native_value, int32_t precise_value)
{
	window.nclip_compares++;
	if ((native_value < 0) != (precise_value < 0) ||
	    (native_value == 0) != (precise_value == 0))
		window.nclip_sign_disagreements++;
	hash_event(7, (uint32_t)native_value, (uint32_t)precise_value);
}

void PGXP_DiagFrame(int backend)
{
	uint64_t state_hash = UINT64_C(1469598103934665603);
	uint64_t cpu_hash = UINT64_C(1469598103934665603);
	uint64_t cp0_hash = UINT64_C(1469598103934665603);
	uint64_t gte_data_hash = UINT64_C(1469598103934665603);
	uint64_t gte_ctrl_hash = UINT64_C(1469598103934665603);
	unsigned mode = PGXP_GetModes();
	uint32_t vc[7];

	frame_number++;
	if (backend != last_backend || mode != last_mode)
	{
		mode_frame = 0;
		if (log_cb)
			log_cb(RETRO_LOG_INFO,
				"[pgxp_frame] backend=\"%s\" mode=0x%02x window=%u\n",
				backend_name(backend), mode, PGXP_DIAG_WINDOW);
		last_backend = backend;
		last_mode = mode;
	}
	mode_frame++;

	cpu_hash = hash_bytes(cpu_hash, CPU_reg, 34 * sizeof(*CPU_reg));
	cp0_hash = hash_bytes(cp0_hash, CP0_reg, 32 * sizeof(*CP0_reg));
	gte_data_hash = hash_bytes(gte_data_hash, GTE_data_reg,
		32 * sizeof(*GTE_data_reg));
	gte_ctrl_hash = hash_bytes(gte_ctrl_hash, GTE_ctrl_reg,
		32 * sizeof(*GTE_ctrl_reg));
	state_hash = hash_bytes(state_hash, &cpu_hash, sizeof(cpu_hash));
	state_hash = hash_bytes(state_hash, &cp0_hash, sizeof(cp0_hash));
	state_hash = hash_bytes(state_hash, &gte_data_hash, sizeof(gte_data_hash));
	state_hash = hash_bytes(state_hash, &gte_ctrl_hash, sizeof(gte_ctrl_hash));

	/* The boot-logo failure is deterministic and occurs before gameplay.
	 * While CPU tracking is enabled, retain a compact per-frame account of
	 * the first six seconds after each mode/backend transition.  This avoids
	 * an instruction-by-instruction log while still locating the first frame
	 * where the PGXP shadow state diverges between CPU backends. */
	if ((mode & PGXP_MODE_CPU) && mode_frame <= 360 && log_cb)
		log_cb(RETRO_LOG_INFO,
			"[pgxp_cpu_boot] mf=%u backend=\"%s\" mode=0x%02x "
			"cpu=%016llx cp0=%016llx gte-d=%016llx gte-c=%016llx "
			"valid=%u/%u/%u/%u invalid-mask=%08x addr-poison=%08x\n",
			mode_frame, backend_name(backend), mode,
			(unsigned long long)cpu_hash,
			(unsigned long long)cp0_hash,
			(unsigned long long)gte_data_hash,
			(unsigned long long)gte_ctrl_hash,
			count_valid(CPU_reg, 34, VALID_01),
			count_valid(CP0_reg, 32, VALID_01),
			count_valid(GTE_data_reg, 32, VALID_01),
			count_valid(GTE_ctrl_reg, 32, VALID_01),
			invalid_register_mask(),
			invalid_address_register_mask());

	if ((frame_number % PGXP_DIAG_WINDOW) != 0 || !log_cb)
		return;

	PGXP_GetVertexCacheStats(vc);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_frame] f=%llu backend=%s mode=0x%02x state=%016llx events=%016llx "
		"mem-r=%llu/%llu mem-w=%llu/%llu gte-v=%llu "
		"vertex=%llu/%llu/%llu w=%llu nclip=%llu/%llu "
		"reject=%llu/%llu/%llu cache=%u/%u/%u/%u\n",
		(unsigned long long)frame_number,
		backend_name(backend), mode,
		(unsigned long long)state_hash,
		(unsigned long long)window.event_hash,
		(unsigned long long)window.mem_reads,
		(unsigned long long)window.mem_invalid_reads,
		(unsigned long long)window.mem_writes,
		(unsigned long long)window.mem_invalid_writes,
		(unsigned long long)window.gte_vertices,
		(unsigned long long)window.vertex_tracked,
		(unsigned long long)window.vertex_cache,
		(unsigned long long)window.vertex_native,
		(unsigned long long)window.vertex_valid_w,
		(unsigned long long)window.nclip_compares,
		(unsigned long long)window.nclip_sign_disagreements,
		(unsigned long long)window.vertex_native_invalid_xy,
		(unsigned long long)window.vertex_native_value_mismatch,
		(unsigned long long)window.vertex_native_both,
		vc[3], vc[4], vc[5], vc[6]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_lineage_summary] f=%llu mfc2=%llu "
		"sll5=%llu/%llu sra5=%llu/%llu preserve=%llu/%llu "
		"identity=%llu/%llu/%llu drops=%llu transforms=%llu "
		"store=%llu/%llu fifo=%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.lineage_mfc2,
		(unsigned long long)window.lineage_sll5_matches,
		(unsigned long long)window.lineage_sll5_candidates,
		(unsigned long long)window.lineage_sra5_matches,
		(unsigned long long)window.lineage_sra5_candidates,
		(unsigned long long)window.lineage_preserve_sll5,
		(unsigned long long)window.lineage_preserve_sra5,
		(unsigned long long)window.lineage_identity_matches,
		(unsigned long long)window.lineage_identity_candidates,
		(unsigned long long)window.lineage_identity_preserve,
		(unsigned long long)window.lineage_drops,
		(unsigned long long)window.lineage_transforms,
		(unsigned long long)window.lineage_store2,
		(unsigned long long)window.lineage_store3,
		(unsigned long long)window.lineage_fifo);
	{
		uint32_t state;
		for (state = 0; state < 4; state++)
			log_cb(RETRO_LOG_INFO,
				"[pgxp_lineage_state] f=%llu src=%u result=%llu/%llu/%llu/%llu\n",
				(unsigned long long)frame_number, state,
				(unsigned long long)window.lineage_transform_state[state][0],
				(unsigned long long)window.lineage_transform_state[state][1],
				(unsigned long long)window.lineage_transform_state[state][2],
				(unsigned long long)window.lineage_transform_state[state][3]);
	}
	log_cb(RETRO_LOG_INFO,
		"[pgxp_lineage_semantic] f=%llu propagated=%llu kind=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.lineage_transform_propagated,
		(unsigned long long)window.lineage_transform_semantic[0],
		(unsigned long long)window.lineage_transform_semantic[1],
		(unsigned long long)window.lineage_transform_semantic[2],
		(unsigned long long)window.lineage_transform_semantic[3],
		(unsigned long long)window.lineage_transform_semantic[4],
		(unsigned long long)window.lineage_transform_semantic[5],
		(unsigned long long)window.lineage_transform_semantic[6],
		(unsigned long long)window.lineage_transform_semantic[7]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_load_summary] f=%llu loads=%llu untracked=%llu "
		"invalid-result=%llu op=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"region=%llu/%llu/%llu/%llu invalid-mask=%08x poison=%08x\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.cpu_loads,
		(unsigned long long)window.cpu_load_untracked,
		(unsigned long long)window.cpu_load_invalid_result,
		(unsigned long long)window.cpu_load_op[0],
		(unsigned long long)window.cpu_load_op[1],
		(unsigned long long)window.cpu_load_op[2],
		(unsigned long long)window.cpu_load_op[3],
		(unsigned long long)window.cpu_load_op[4],
		(unsigned long long)window.cpu_load_op[5],
		(unsigned long long)window.cpu_load_op[6],
		(unsigned long long)window.cpu_load_region[0],
		(unsigned long long)window.cpu_load_region[1],
		(unsigned long long)window.cpu_load_region[2],
		(unsigned long long)window.cpu_load_region[3],
		invalid_register_mask(),
		invalid_address_register_mask());
	log_cb(RETRO_LOG_INFO,
		"[pgxp_load_state] f=%llu total=%llu/%llu/%llu "
		"invalid=%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.cpu_load_state[0],
		(unsigned long long)window.cpu_load_state[1],
		(unsigned long long)window.cpu_load_state[2],
		(unsigned long long)window.cpu_load_invalid_state[0],
		(unsigned long long)window.cpu_load_invalid_state[1],
		(unsigned long long)window.cpu_load_invalid_state[2]);
	{
		uint32_t state;
		for (state = 0; state < PGXP_DIAG_MEMORY_STATES; state++)
			log_cb(RETRO_LOG_INFO,
				"[pgxp_load_state_op] f=%llu state=%u "
				"invalid=%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
				(unsigned long long)frame_number, state,
				(unsigned long long)window.cpu_load_invalid_state_op[state][0],
				(unsigned long long)window.cpu_load_invalid_state_op[state][1],
				(unsigned long long)window.cpu_load_invalid_state_op[state][2],
				(unsigned long long)window.cpu_load_invalid_state_op[state][3],
				(unsigned long long)window.cpu_load_invalid_state_op[state][4],
				(unsigned long long)window.cpu_load_invalid_state_op[state][5],
				(unsigned long long)window.cpu_load_invalid_state_op[state][6]);
	}
	{
		uint32_t i;
		for (i = 0; i < PGXP_DIAG_ADDRESS_PAGES; i++)
			if (window.cpu_load_page[i].count)
				log_cb(RETRO_LOG_INFO,
					"[pgxp_load_page] f=%llu page=%05x count=%u\n",
					(unsigned long long)frame_number,
					window.cpu_load_page[i].page,
					window.cpu_load_page[i].count);
	}

	memset(&window, 0, sizeof(window));
	window.event_hash = UINT64_C(1469598103934665603);
	dispatch_samples = 0;
}

#endif /* PGXP_DIAG */

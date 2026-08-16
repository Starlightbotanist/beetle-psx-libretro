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
#define PGXP_DIAG_STORE8_SLOTS 4096u

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
	uint32_t addr;
	uint32_t value;
	uint32_t shadow_value;
	uint32_t shadow_flags;
	uint32_t shadow_count;
	PGXP_diag_store8 store8;
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
static PGXP_diag_gpu_provenance fifo_provenance[32];
static PGXP_diag_store8 store8_provenance[PGXP_DIAG_STORE8_SLOTS];
static PGXP_diag_gpu_provenance cb_provenance[16];

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
	memset(cb_provenance, 0, sizeof(cb_provenance));
	dispatch_samples = 0;
	vertex_samples = 0;
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

		if (store8->valid && store8->word_addr == word_addr && shadow &&
		    shadow->count == store8->invalid_count)
			provenance->store8 = *store8;
		else
			memset(&provenance->store8, 0, sizeof(provenance->store8));
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
	if (source == PGXP_DIAG_VERTEX_NATIVE)
	{
		if (!valid_xy)
			window.vertex_native_invalid_xy++;
		if (!value_match)
			window.vertex_native_value_mismatch++;
		if (!valid_xy && !value_match)
			window.vertex_native_both++;

		if (vertex_samples < PGXP_DIAG_LOAD_SAMPLES && log_cb)
		{
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
				"[pgxp_vertex_store8] n=%u mf=%u slot=%u seen=%u "
				"store_mf=%u addr=%08x byte=%02x before=%08x flags=%08x "
				"count=%u\n",
				vertex_samples + 1, mode_frame, slot,
				cb_provenance[slot].store8.valid,
				cb_provenance[slot].store8.mode_frame,
				cb_provenance[slot].store8.byte_addr,
				cb_provenance[slot].store8.value,
				cb_provenance[slot].store8.before_value,
				cb_provenance[slot].store8.before_flags,
				cb_provenance[slot].store8.before_count);
			vertex_samples++;
		}
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

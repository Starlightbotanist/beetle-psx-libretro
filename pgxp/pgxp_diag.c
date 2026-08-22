#include "pgxp_diag.h"

#if PGXP_DIAG

#include <math.h>
#include <string.h>

#include <libretro.h>

#include "pgxp_cpu.h"
#include "pgxp_gpu.h"
#include "pgxp_gte.h"
#include "pgxp_main.h"
#include "pgxp_mem.h"
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
#define PGXP_TRACE_RING_SIZE 262144u
#define PGXP_TRACE_REASON_COUNT 8u
#define PGXP_DIAG_PRIMITIVE_BUCKETS 32u
#define PGXP_DIAG_PRIMITIVE_COMPOSITIONS 4u
#define PGXP_DIAG_TRACE_STAGES 9u
#define PGXP_DIAG_RECOVERY_BUCKETS 65536u
#define PGXP_DIAG_RECOVERY_WAYS 4u
#define PGXP_DIAG_RAM_WORDS (2048u * 1024u / 4u)
#define PGXP_DIAG_WRITER_WIDTHS 4u
#define PGXP_DIAG_COHERENCE_BUCKETS 16384u
#define PGXP_DIAG_COHERENCE_WAYS 4u
#define PGXP_DIAG_EDGE_SLOTS 16384u
#define PGXP_DIAG_EDGE_KINDS 3u
#define PGXP_DIAG_EDGE_DELTA_BINS 7u
#define PGXP_DIAG_EDGE_PACKET_BINS 6u
#define PGXP_DIAG_EDGE_SAMPLES 96u
#define PGXP_DIAG_SEAM_ACCEPT_SAMPLES 256u
#define PGXP_DIAG_SEAM_ACCEPT_WINDOW_SAMPLES 8u

enum PGXP_trace_event_type
{
	PGXP_TRACE_EVENT_GTE = 1,
	PGXP_TRACE_EVENT_MFC2,
	PGXP_TRACE_EVENT_SLL5,
	PGXP_TRACE_EVENT_SRA5,
	PGXP_TRACE_EVENT_STORE,
	PGXP_TRACE_EVENT_LOAD,
	PGXP_TRACE_EVENT_FIFO,
	PGXP_TRACE_EVENT_CB,
	PGXP_TRACE_EVENT_VERTEX,
	PGXP_TRACE_EVENT_CPU
};

typedef struct
{
	uint64_t id;
	uint64_t sequence;
	uint32_t frame;
	uint32_t instr_addr;
	uint32_t before;
	uint32_t after;
	uint32_t shadow_value;
	uint32_t shadow_flags;
	float x;
	float y;
	uint8_t type;
	uint8_t reason;
	uint8_t stage;
	uint16_t source_dest;
} PGXP_trace_event;

typedef struct
{
	uint32_t value;
	uint32_t frame;
	float x;
	float y;
	float z;
	uint8_t ambiguous;
	uint8_t valid;
} PGXP_diag_recovery_vertex;

typedef struct
{
	uint64_t trace_id;
	uint32_t value;
	uint32_t flags;
	uint32_t frame;
	uint8_t width; /* 1 = full word, 2 = halfword, 3 = byte */
	uint8_t stage;
	uint8_t valid;
} PGXP_diag_mem_writer;

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
	PGXP_diag_mem_writer writer;
} PGXP_diag_gpu_provenance;

typedef struct
{
	uint32_t key;
	uint32_t frame;
	uint64_t packet;
	float x;
	float y;
	float w;
	uint8_t stage;
	uint8_t valid;
} PGXP_diag_coherence_vertex;

/* One frame-local record for a native PSX edge.  Keeping the most recent
 * textured and untextured observation lets us ask two distinct questions:
 * whether adjacent primitives in the same material class recover different
 * PGXP endpoints, and whether the textured surface and the untextured layer
 * exposed by the R4 coverage probe disagree at an otherwise identical native
 * edge. */
typedef struct
{
	int32_t native_x[2];
	int32_t native_y[2];
	float precise_x[2][2];
	float precise_y[2][2];
	uint64_t packet[2];
	uint8_t opcode[2];
	uint8_t gouraud[2];
	uint8_t invalid_w[2];
	uint8_t valid[2];
} PGXP_diag_edge;

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
	uint64_t projection_z_band[7];
	double projection_z_max;
	uint64_t projection_z_delta[6];
	double projection_z_delta_sum;
	double projection_z_delta_max;
	uint64_t rendered_z_linked[3];
	uint64_t rendered_z_far[3];
	uint64_t rendered_z_band[3][4];
	uint64_t rendered_z_delta[3][6];
	uint64_t vertex_delta_class[3][6];
	uint64_t vertex_axis_exact[3][2];
	uint64_t vertex_axis_subpixel[3][2];
	uint64_t vertex_axis_ge_one[3][2];
	uint64_t vertex_axis_sign[3][2][3];
	double vertex_delta_abs_sum[3][2];
	double vertex_delta_sum[3][2];
	float vertex_delta_max[3][2];
	uint64_t vertex_stage_delta[PGXP_DIAG_TRACE_STAGES][6];
	uint64_t vertex_writer_delta[PGXP_DIAG_WRITER_WIDTHS][6];
	uint64_t vertex_tracked;
	uint64_t vertex_cache;
	uint64_t vertex_native;
	uint64_t vertex_native_invalid_xy;
	uint64_t vertex_native_value_mismatch;
	uint64_t vertex_native_both;
	uint64_t vertex_valid_w;
	uint64_t vertex_w_gate_kept;
	uint64_t vertex_w_gate_rejected;
	uint64_t vertex_w_gate_rejected_stage[PGXP_DIAG_TRACE_STAGES];
	uint64_t vertex_w_gate_rejected_width[PGXP_DIAG_WRITER_WIDTHS];
	/* Cross-tab the command-buffer snapshot decision against the live
	 * source-memory decision SwanStation makes when consuming a vertex.
	 * Columns are eligible/no-address/invalid-xy/value-mismatch. */
	uint64_t vertex_live_state[3][4];
	uint64_t vertex_coherence_delta[4][6];
	uint64_t vertex_coherence_cross_stage[4];
	double vertex_coherence_w_delta[4];
	uint64_t line_hack_candidates[4];
	uint64_t line_hack_w_rejects[4];
	uint64_t nclip_compares;
	uint64_t nclip_sign_disagreements;
	uint64_t nclip_applied_sign_changes;
	uint64_t nclip_precise_zero_fallbacks;
	uint64_t nclip_reference_sign_disagreements;
	uint64_t nclip_reference_native_disagreements;
	uint64_t nclip_double_zero;
	uint64_t nclip_reference_zero;
	uint64_t nclip_validity_attempts;
	uint64_t nclip_validity_invalid;
	uint64_t nclip_invalid_mask[8];
	uint64_t nclip_mismatch_mask[8];
	uint64_t nclip_z_invalid_mask[8];
	uint64_t nclip_xy_only_attempts;
	uint64_t nclip_xy_only_compares;
	uint64_t nclip_xy_only_sign_changes;
	uint64_t nclip_xy_only_y_band[4];
	uint64_t gpu_triangles[3];
	uint64_t gpu_area_native_sign[3];
	uint64_t gpu_area_precise_sign[3];
	uint64_t gpu_area_sign_disagreements;
	uint64_t gpu_area_precise_zero;
	uint64_t gpu_area_near_native[5];
	uint64_t gpu_area_near_precise[5];
	uint64_t gpu_area_anomaly_y[4];
	uint64_t gpu_quad_pairs;
	uint64_t gpu_quad_native_fold;
	uint64_t gpu_quad_precise_fold;
	uint64_t gpu_quad_fold_introduced;
	uint64_t gpu_quad_fold_removed;
	uint64_t gpu_quad_fold_introduced_y[4];
	uint64_t gpu_quad_fold_introduced_opcode[32];
	uint64_t gpu_triangle_invalid_w;
	uint64_t gpu_oversize_x;
	uint64_t gpu_oversize_y;
	uint64_t gpu_oversize_sign_disagreements;
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
	uint64_t trace_events[11];
	uint64_t trace_reasons[PGXP_TRACE_REASON_COUNT];
	uint64_t trace_terminal[3][PGXP_TRACE_REASON_COUNT];
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
static uint32_t vertex_live_samples;
static uint32_t vertex_coherence_samples;
static uint32_t line_hack_samples;
static uint8_t pending_projection_z_band;
static uint8_t pending_projection_z_delta;
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
static PGXP_diag_coherence_vertex coherence_vertices
	[PGXP_DIAG_COHERENCE_BUCKETS][PGXP_DIAG_COHERENCE_WAYS];
static PGXP_trace_event trace_ring[PGXP_TRACE_RING_SIZE];
static uint64_t trace_next_id = 1;
static uint64_t trace_sequence;
static uint32_t trace_write;
static uint32_t trace_chain_samples;
static uint32_t trace_tracked_samples;
static uint64_t trace_tracked_ids[64];
static PGXP_diag_recovery_vertex recovery_vertices
	[PGXP_DIAG_RECOVERY_BUCKETS][PGXP_DIAG_RECOVERY_WAYS];
static uint64_t recovery_attempts;
static uint64_t recovery_hits;
static uint64_t recovery_ambiguous;
static uint64_t recovery_ambiguous_used;
static uint64_t recovery_misses;
static uint64_t recovery_age_hits[5];
static uint64_t recovery_old_age[4];
static uint64_t recovery_too_old;
static uint64_t recovery_stage_attempts[PGXP_DIAG_TRACE_STAGES];
static uint64_t recovery_stage_hits[PGXP_DIAG_TRACE_STAGES];
static uint64_t recovery_way_hits[PGXP_DIAG_RECOVERY_WAYS];
static uint64_t recovery_evictions;
static PGXP_diag_mem_writer mem_writers[PGXP_DIAG_RAM_WORDS];
static uint64_t writer_writes[PGXP_DIAG_WRITER_WIDTHS];
static uint64_t writer_native[PGXP_DIAG_WRITER_WIDTHS];
static uint64_t writer_native_reason[PGXP_DIAG_WRITER_WIDTHS]
	[PGXP_TRACE_REASON_COUNT];
static uint64_t writer_native_stage[PGXP_DIAG_WRITER_WIDTHS]
	[PGXP_DIAG_TRACE_STAGES];
static uint64_t writer_tracked[PGXP_DIAG_WRITER_WIDTHS];
static uint64_t writer_tracked_source_w[PGXP_DIAG_WRITER_WIDTHS];
static uint64_t writer_tracked_retained_w[PGXP_DIAG_WRITER_WIDTHS];
static uint64_t writer_tracked_invalid_w;
static uint32_t writer_samples;
static uint32_t writer_tracked_samples;
static uint64_t packet_ordinal;
static uint64_t current_packet;
static uint8_t current_opcode;
static uint8_t current_packet_words;
static uint8_t current_abr;
static uint8_t current_tex_mode;
static uint8_t current_mask_eval;
typedef struct PGXP_diag_packet_vertex_Tag
{
	uint64_t trace_id;
	uint32_t addr;
	uint32_t value;
	uint8_t source;
	uint8_t stage;
	uint8_t reason;
} PGXP_diag_packet_vertex;
static PGXP_diag_packet_vertex packet_vertices[3];
static uint8_t packet_vertex_count;
static uint32_t primitive_bucket_samples[PGXP_DIAG_PRIMITIVE_BUCKETS];
static uint64_t primitive_total;
static uint64_t primitive_class[2][2][2];
static uint64_t primitive_y_band[4];
static uint64_t primitive_sra_vertices;
static uint64_t primitive_tolerance_reverts;
static uint64_t primitive_composition[PGXP_DIAG_PRIMITIVE_BUCKETS]
	[PGXP_DIAG_PRIMITIVE_COMPOSITIONS];
static uint64_t primitive_source_stage[PGXP_DIAG_PRIMITIVE_BUCKETS][3]
	[PGXP_DIAG_TRACE_STAGES];
static uint64_t primitive_native_reason[PGXP_DIAG_PRIMITIVE_BUCKETS]
	[PGXP_TRACE_REASON_COUNT];
static uint64_t primitive_native_sra5_reason[PGXP_DIAG_PRIMITIVE_BUCKETS]
	[PGXP_TRACE_REASON_COUNT];
static PGXP_diag_edge edge_table[PGXP_DIAG_EDGE_SLOTS];
static uint32_t edge_table_frame = ~UINT32_C(0);
static uint64_t edge_observations[2];
static uint64_t edge_compares[PGXP_DIAG_EDGE_KINDS];
static uint64_t edge_delta_bins[PGXP_DIAG_EDGE_KINDS]
	[PGXP_DIAG_EDGE_DELTA_BINS];
static uint64_t edge_near_delta_bins[PGXP_DIAG_EDGE_KINDS]
	[PGXP_DIAG_EDGE_DELTA_BINS];
static uint64_t edge_packet_bins[PGXP_DIAG_EDGE_KINDS]
	[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t edge_mismatch_y[PGXP_DIAG_EDGE_KINDS][4];
static double edge_delta_sum[PGXP_DIAG_EDGE_KINDS];
static float edge_delta_max[PGXP_DIAG_EDGE_KINDS];
static uint64_t edge_table_overflow;
static uint32_t edge_samples;
static uint64_t seam_results[PGXP_DIAG_SEAM_RESULTS];
static uint64_t seam_delta_bins[PGXP_DIAG_EDGE_DELTA_BINS];
static uint64_t seam_age_bins[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t seam_primitives;
static uint64_t seam_observed_edges;
static uint64_t seam_moved_vertices;
static uint64_t seam_conflicts;
static uint32_t seam_accept_samples;
static uint32_t seam_accept_window_samples;
static int gpu_quad_native_sign;
static int gpu_quad_precise_sign;
static int gpu_quad_invalid_w;
static int gpu_quad_pending;
static uint32_t gpu_area_samples;
static uint32_t gpu_area_window_samples;
static uint32_t gpu_fold_samples;
static uint32_t gpu_fold_window_samples;
static uint32_t nclip_invalid_samples;
static uint32_t nclip_invalid_window_samples;
static uint32_t nclip_reference_samples;
static uint32_t nclip_reference_window_samples;
static uint32_t nclip_xy_only_samples;
static uint32_t nclip_xy_only_window_samples;
static unsigned pending_nclip_z_mask;

static int trace_metadata_valid(const PGXP_value* value)
{
	return value && value->trace_id > 0 &&
		value->trace_id < trace_next_id &&
		value->trace_stage >= PGXP_TRACE_GTE &&
		value->trace_stage <= PGXP_TRACE_VERTEX;
}

static void trace_record(uint8_t type, uint8_t reason, uint64_t id,
		uint8_t stage, uint16_t source_dest, uint32_t instr_addr,
		uint32_t before, uint32_t after, const PGXP_value* shadow)
{
	PGXP_trace_event* event;

	if (!id || id >= trace_next_id || stage < PGXP_TRACE_GTE ||
	    stage > PGXP_TRACE_VERTEX)
		return;
	event = &trace_ring[trace_write++ & (PGXP_TRACE_RING_SIZE - 1)];
	event->id = id;
	event->sequence = ++trace_sequence;
	event->frame = mode_frame;
	event->instr_addr = instr_addr;
	event->before = before;
	event->after = after;
	event->shadow_value = shadow ? shadow->value : 0;
	event->shadow_flags = shadow ? shadow->flags : 0;
	event->x = shadow ? shadow->x : 0.f;
	event->y = shadow ? shadow->y : 0.f;
	event->type = type;
	event->reason = reason;
	event->stage = stage;
	event->source_dest = source_dest;
	if (type < 11)
		window.trace_events[type]++;
	if (reason < PGXP_TRACE_REASON_COUNT)
		window.trace_reasons[reason]++;
}

static void trace_dump_chain(uint64_t id, uint8_t terminal_reason)
{
	uint64_t first;
	uint64_t sequence;

	if (!id || !log_cb || trace_chain_samples >= 48)
		return;
	first = trace_sequence > PGXP_TRACE_RING_SIZE ?
		trace_sequence - PGXP_TRACE_RING_SIZE + 1 : 1;
	log_cb(RETRO_LOG_INFO,
		"[pgxp_trace_chain] sample=%u id=%llu terminal=%u begin=%llu end=%llu\n",
		trace_chain_samples + 1, (unsigned long long)id, terminal_reason,
		(unsigned long long)first, (unsigned long long)trace_sequence);
	for (sequence = first; sequence <= trace_sequence; sequence++)
	{
		const PGXP_trace_event* event =
			&trace_ring[(uint32_t)(sequence - 1) & (PGXP_TRACE_RING_SIZE - 1)];
		if (event->sequence != sequence || event->id != id)
			continue;
		log_cb(RETRO_LOG_INFO,
			"[pgxp_trace_event] id=%llu seq=%llu mf=%u type=%u reason=%u "
			"stage=%u sd=%03x ia=%08x native=%08x/%08x "
			"shadow=%08x flags=%08x xy=%.3f/%.3f\n",
			(unsigned long long)id, (unsigned long long)event->sequence,
			event->frame, event->type, event->reason, event->stage,
			event->source_dest, event->instr_addr, event->before,
			event->after, event->shadow_value, event->shadow_flags,
			event->x, event->y);
	}
	trace_chain_samples++;
}


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
	memset(coherence_vertices, 0, sizeof(coherence_vertices));
	dispatch_samples = 0;
	vertex_samples = 0;
	cache_vertex_samples = 0;
	vertex_live_samples = 0;
	vertex_coherence_samples = 0;
	line_hack_samples = 0;
	pending_projection_z_band = 0;
	pending_projection_z_delta = 0;
	lineage_fifo_samples = 0;
	lineage_vertex_samples = 0;
	lineage_drop_samples = 0;
	memset(lineage_transform_samples, 0,
		sizeof(lineage_transform_samples));
	trace_next_id = 1;
	trace_sequence = 0;
	trace_write = 0;
	trace_chain_samples = 0;
	trace_tracked_samples = 0;
	memset(recovery_vertices, 0, sizeof(recovery_vertices));
	memset(mem_writers, 0, sizeof(mem_writers));
	recovery_attempts = recovery_hits = recovery_ambiguous = recovery_misses = 0;
	recovery_ambiguous_used = 0;
	memset(recovery_age_hits, 0, sizeof(recovery_age_hits));
	memset(recovery_old_age, 0, sizeof(recovery_old_age));
	recovery_too_old = 0;
	memset(recovery_stage_attempts, 0, sizeof(recovery_stage_attempts));
	memset(recovery_stage_hits, 0, sizeof(recovery_stage_hits));
	memset(recovery_way_hits, 0, sizeof(recovery_way_hits));
	recovery_evictions = 0;
	memset(writer_writes, 0, sizeof(writer_writes));
	memset(writer_native, 0, sizeof(writer_native));
	memset(writer_native_reason, 0, sizeof(writer_native_reason));
	memset(writer_native_stage, 0, sizeof(writer_native_stage));
	memset(writer_tracked, 0, sizeof(writer_tracked));
	memset(writer_tracked_source_w, 0, sizeof(writer_tracked_source_w));
	memset(writer_tracked_retained_w, 0, sizeof(writer_tracked_retained_w));
	writer_tracked_invalid_w = 0;
	writer_samples = 0;
	writer_tracked_samples = 0;
	memset(trace_tracked_ids, 0, sizeof(trace_tracked_ids));
	packet_ordinal = 0;
	current_packet = 0;
	current_opcode = 0;
	current_packet_words = 0;
	current_abr = 0;
	current_tex_mode = 0;
	current_mask_eval = 0;
	memset(packet_vertices, 0, sizeof(packet_vertices));
	packet_vertex_count = 0;
	memset(primitive_bucket_samples, 0, sizeof(primitive_bucket_samples));
	primitive_total = 0;
	memset(primitive_class, 0, sizeof(primitive_class));
	memset(primitive_y_band, 0, sizeof(primitive_y_band));
	primitive_sra_vertices = 0;
	primitive_tolerance_reverts = 0;
	memset(primitive_composition, 0, sizeof(primitive_composition));
	memset(primitive_source_stage, 0, sizeof(primitive_source_stage));
	memset(primitive_native_reason, 0, sizeof(primitive_native_reason));
	memset(primitive_native_sra5_reason, 0,
		sizeof(primitive_native_sra5_reason));
	memset(edge_table, 0, sizeof(edge_table));
	edge_table_frame = ~UINT32_C(0);
	memset(edge_observations, 0, sizeof(edge_observations));
	memset(edge_compares, 0, sizeof(edge_compares));
	memset(edge_delta_bins, 0, sizeof(edge_delta_bins));
	memset(edge_near_delta_bins, 0, sizeof(edge_near_delta_bins));
	memset(edge_packet_bins, 0, sizeof(edge_packet_bins));
	memset(edge_mismatch_y, 0, sizeof(edge_mismatch_y));
	memset(edge_delta_sum, 0, sizeof(edge_delta_sum));
	memset(edge_delta_max, 0, sizeof(edge_delta_max));
	edge_table_overflow = 0;
	edge_samples = 0;
	memset(seam_results, 0, sizeof(seam_results));
	memset(seam_delta_bins, 0, sizeof(seam_delta_bins));
	memset(seam_age_bins, 0, sizeof(seam_age_bins));
	seam_primitives = 0;
	seam_observed_edges = 0;
	seam_moved_vertices = 0;
	seam_conflicts = 0;
	seam_accept_samples = 0;
	seam_accept_window_samples = 0;
	gpu_quad_native_sign = 0;
	gpu_quad_precise_sign = 0;
	gpu_quad_invalid_w = 0;
	gpu_quad_pending = 0;
	gpu_area_samples = 0;
	gpu_area_window_samples = 0;
	gpu_fold_samples = 0;
	gpu_fold_window_samples = 0;
	nclip_invalid_samples = 0;
	nclip_invalid_window_samples = 0;
	nclip_reference_samples = 0;
	nclip_reference_window_samples = 0;
	nclip_xy_only_samples = 0;
	nclip_xy_only_window_samples = 0;
	pending_nclip_z_mask = 0;
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
	trace_record(PGXP_TRACE_EVENT_LOAD,
		!result->trace_id ? 1 : (invalid_result ? 2 :
		(result->value != value ? 3 : 0)), result->trace_id,
		result->trace_stage, (uint8_t)reg, addr, value, result->value, result);

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

void PGXP_DiagMemoryWrite(uint32_t addr, const PGXP_value* value,
		int valid_address, int full_word)
{
	uint32_t segment = addr >> 24;
	unsigned width = full_word ? 1u : 2u;

	window.mem_writes++;
	if (!valid_address)
		window.mem_invalid_writes++;
	hash_event(2, addr, value ? value->value : 0);
	if (!value || (segment != 0x00 && segment != 0x80 && segment != 0xa0))
		return;
	{
		PGXP_diag_mem_writer* writer = &mem_writers
			[(addr & UINT32_C(0x001ffffc)) >> 2];
		writer->trace_id = trace_metadata_valid(value) ? value->trace_id : 0;
		writer->value = value->value;
		writer->flags = value->flags;
		writer->frame = mode_frame;
		writer->width = (uint8_t)width;
		writer->stage = trace_metadata_valid(value) ? value->trace_stage :
			PGXP_TRACE_NONE;
		writer->valid = 1;
		writer_writes[width]++;
	}
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

void PGXP_DiagTraceGTE(PGXP_value* value)
{
	PGXP_diag_recovery_vertex* recovery;
	uint32_t index;
	unsigned way;
	unsigned replace = PGXP_DIAG_RECOVERY_WAYS;
	if (!value)
		return;
	/* A GTE result starts a fresh provenance chain. */
	memset(value->trace_reserved, 0, sizeof(value->trace_reserved));
	/* Byte 6 is diagnostic-only projection provenance. The low nibble is the
	 * raw-Z band and the high nibble is the exact-vs-applied delta class;
	 * bytes 0..5 retain their existing lineage and original-MFC2 roles. */
	value->trace_reserved[6] = (uint8_t)(pending_projection_z_band |
		(pending_projection_z_delta << 4));
	value->trace_id = trace_next_id++;
	value->trace_stage = PGXP_TRACE_GTE;
	trace_record(PGXP_TRACE_EVENT_GTE, 0, value->trace_id,
		value->trace_stage, 0, 0, value->value, value->value, value);
	index = (value->value * UINT32_C(2654435761)) >> 16;
	recovery = NULL;
	for (way = 0; way < PGXP_DIAG_RECOVERY_WAYS; way++)
	{
		PGXP_diag_recovery_vertex* candidate = &recovery_vertices[index][way];
		if (candidate->valid && candidate->value == value->value)
		{
			recovery = candidate;
			break;
		}
		if (!candidate->valid)
		{
			if (replace == PGXP_DIAG_RECOVERY_WAYS ||
			    recovery_vertices[index][replace].valid)
				replace = way;
			continue;
		}
		if (replace == PGXP_DIAG_RECOVERY_WAYS ||
		    (recovery_vertices[index][replace].valid &&
		     candidate->frame < recovery_vertices[index][replace].frame))
			replace = way;
	}
	if (!recovery)
	{
		recovery = &recovery_vertices[index][replace];
		if (recovery->valid)
			recovery_evictions++;
	}
	if (recovery->valid && recovery->frame == mode_frame &&
	    recovery->value == value->value)
	{
		if (recovery->x != value->x || recovery->y != value->y ||
		    recovery->z != value->z)
			recovery->ambiguous = 1;
	}
	else
	{
		recovery->value = value->value;
		recovery->frame = mode_frame;
		recovery->x = value->x;
		recovery->y = value->y;
		recovery->z = value->z;
		recovery->ambiguous = 0;
		recovery->valid = 1;
	}
}

int PGXP_DiagRecoverVertex(uint32_t value, const PGXP_value* stale,
		unsigned slot, float* x, float* y, float* z)
{
	PGXP_diag_recovery_vertex* recovery;
	uint32_t index;
	uint32_t age;
	unsigned way;
	unsigned stage = trace_metadata_valid(stale) ? stale->trace_stage :
		PGXP_TRACE_NONE;

	recovery_attempts++;
	if (stage >= PGXP_DIAG_TRACE_STAGES)
		stage = PGXP_TRACE_NONE;
	recovery_stage_attempts[stage]++;
	index = (value * UINT32_C(2654435761)) >> 16;
	recovery = NULL;
	for (way = 0; way < PGXP_DIAG_RECOVERY_WAYS; way++)
	{
		PGXP_diag_recovery_vertex* candidate = &recovery_vertices[index][way];
		if (candidate->valid && candidate->value == value)
		{
			recovery = candidate;
			break;
		}
	}
	if (!recovery || recovery->frame > mode_frame)
	{
		recovery_misses++;
		return 0;
	}
	age = mode_frame - recovery->frame;
	if (age > 4)
	{
		recovery_too_old++;
		recovery_old_age[age < 8 ? age - 5 : 3]++;
		return 0;
	}
	if (recovery->ambiguous)
	{
		recovery_ambiguous++;
		/* Keep the first exact-word result as a bounded experiment.  Multiple
		 * precise GTE results may quantize to the same native pixel; the first
		 * remains spatially compatible even though its subpixel/depth identity
		 * is not proven. */
		recovery_ambiguous_used++;
	}
	*x = recovery->x;
	*y = recovery->y;
	*z = recovery->z;
	recovery_hits++;
	recovery_age_hits[age]++;
	recovery_stage_hits[stage]++;
	recovery_way_hits[way]++;
	return 1;
	return 0;
}

void PGXP_DiagTraceMFC2(uint32_t instr, PGXP_value* value)
{
	if (!trace_metadata_valid(value))
		return;
	/* Preserve the root MFC2 native word for the SLL5/SRA5 round-trip. */
	memcpy(value->trace_reserved + 1, &value->value, sizeof(value->value));
	value->trace_reserved[5] = 1;
	value->trace_stage = PGXP_TRACE_MFC2;
	trace_record(PGXP_TRACE_EVENT_MFC2, 0, value->trace_id,
		value->trace_stage,
		(uint16_t)((((instr >> 11) & 31) << 5) | ((instr >> 16) & 31)),
		instr, value->value, value->value, value);
}

void PGXP_DiagTraceShift(uint32_t instr, uint32_t before, uint32_t after,
		int arithmetic, unsigned reason, PGXP_value* value)
{
	uint64_t id = trace_metadata_valid(value) ? value->trace_id : 0;
	uint8_t stage = value ? value->trace_stage : 0;

	if (reason == 0 && value && id)
	{
		stage = arithmetic ? PGXP_TRACE_SRA5 : PGXP_TRACE_SLL5;
		value->trace_stage = stage;
	}
	trace_record(arithmetic ? PGXP_TRACE_EVENT_SRA5 : PGXP_TRACE_EVENT_SLL5,
		(uint8_t)reason, id, stage,
		(uint16_t)((((instr >> 16) & 31) << 5) | ((instr >> 11) & 31)),
		instr, before, after, value);
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
	if ((result.flags & VALID_01) != VALID_01)
	{
		PGXP_DiagTraceShift(instr, before, after, arithmetic, 2, &result);
		return 0;
	}
	if (result.value != before)
	{
		PGXP_DiagTraceShift(instr, before, after, arithmetic, 3, &result);
		return 0;
	}
	/* In diagnostic mode, a traced SLL5 can prove the SRA5 handoff even
	 * when the legacy lineage table was not updated for this register. */
	if (arithmetic && (instr >> 6 & 31) == 5 &&
	    result.trace_id != 0 && result.trace_stage == PGXP_TRACE_SLL5 &&
	    result.trace_reserved[0] != 0)
	{
		PGXP_DiagTraceShift(instr, before, after, arithmetic, 5, &result);
		return 0;
	}
    if (arithmetic && (instr >> 6 & 31) == 5 &&
        result.trace_id != 0 && result.trace_stage == PGXP_TRACE_SLL5 &&
        result.trace_reserved[0] == 0 && result.trace_reserved[5] != 0)
    {
        uint32_t original_mfc2;
        memcpy(&original_mfc2, result.trace_reserved + 1,
            sizeof(original_mfc2));
        if (after != original_mfc2)
        {
            PGXP_DiagTraceShift(instr, before, after, arithmetic, 7, &result);
            return 0;
        }
    }
	if ((!lineage_reg[dest].valid ||
	     lineage_reg[dest].stage != (arithmetic ? 3u : 2u)) &&
	    !(arithmetic && (instr >> 6 & 31) == 5 &&
	      result.trace_id != 0 && result.trace_stage == PGXP_TRACE_SLL5 &&
	      result.trace_reserved[0] == 0 && result.trace_reserved[5] != 0))
	{
		PGXP_DiagTraceShift(instr, before, after, arithmetic, 4, &result);
		return 0;
	}

	result.value = after;
	PGXP_DiagTraceShift(instr, before, after, arithmetic, 0, &result);
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
	trace_record(PGXP_TRACE_EVENT_CPU, 0, result.trace_id,
		result.trace_stage, (uint16_t)((source << 5) | dest), 0,
		before, after, &result);
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

	/* In Memory Only mode, a non-identity CPU write leaves CPU_reg[dest]
	 * unchanged. Mark any traced shadow there as stale while retaining its
	 * trace metadata for the diagnostic ledger. */
	if (trace_metadata_valid(&CPU_reg[dest]))
		CPU_reg[dest].trace_reserved[0] = 1;

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
	if (trace_metadata_valid(source_shadow))
	{
		int result_trace_valid = trace_metadata_valid(result_shadow);
		uint8_t trace_reason = result_trace_valid &&
			result_shadow->trace_id == source_shadow->trace_id ? 0 :
			(!result_trace_valid ? 1 : 5);
		if ((result_shadow->flags & VALID_01) != VALID_01)
			trace_reason = 2;
		else if (result_shadow->value != gpr[dest])
			trace_reason = 3;
		trace_record(PGXP_TRACE_EVENT_CPU, trace_reason,
			source_shadow->trace_id, source_shadow->trace_stage,
			(uint16_t)((source << 5) | dest), instr, expected,
			gpr[dest], result_shadow);
	}

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
	const PGXP_value* shadow = &CPU_reg[source];

	trace_record(PGXP_TRACE_EVENT_STORE,
		!shadow->trace_id ? 1 : ((shadow->flags & VALID_01) != VALID_01 ? 2 :
		(shadow->value != value ? 3 : 0)), shadow->trace_id,
		shadow->trace_stage, (uint8_t)source, addr, value, shadow->value, shadow);

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
	PGXP_diag_mem_writer* writer = NULL;

	provenance->word_addr = word_addr;
	provenance->byte_addr = addr;
	provenance->value = value;
	provenance->before_value = shadow ? shadow->value : 0;
	provenance->before_flags = shadow ? shadow->flags : 0;
	provenance->before_count = shadow ? shadow->count : 0;
	provenance->invalid_count = invalid_count;
	provenance->mode_frame = mode_frame;
	provenance->valid = 1;
	if (word_addr < UINT32_C(0x00200000))
	{
		writer = &mem_writers[word_addr >> 2];
		writer->trace_id = shadow && trace_metadata_valid(shadow) ?
			shadow->trace_id : 0;
		writer->value = value;
		writer->flags = shadow ? shadow->flags : 0;
		writer->frame = mode_frame;
		writer->width = 3;
		writer->stage = shadow && trace_metadata_valid(shadow) ?
			shadow->trace_stage : PGXP_TRACE_NONE;
		writer->valid = 1;
		writer_writes[3]++;
	}
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
	if (shadow)
		trace_record(PGXP_TRACE_EVENT_FIFO,
			!shadow->trace_id ? 1 : ((shadow->flags & VALID_01) != VALID_01 ? 2 :
			(shadow->value != value ? 3 : 0)), shadow->trace_id,
			shadow->trace_stage, (uint8_t)pos, addr, value,
			shadow->value, shadow);

	{
		uint32_t word_addr = gpu_source_word_addr(addr);
		const PGXP_diag_store8* store8 = &store8_provenance
			[(word_addr >> 2) & (PGXP_DIAG_STORE8_SLOTS - 1)];

		memset(&provenance->store8, 0, sizeof(provenance->store8));
		memset(&provenance->lineage, 0, sizeof(provenance->lineage));
		memset(&provenance->writer, 0, sizeof(provenance->writer));
		if (word_addr < UINT32_C(0x00200000))
		{
			provenance->writer = mem_writers[word_addr >> 2];
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
	{
		const PGXP_value* shadow;
		cb_provenance[slot] = fifo_provenance[fifo_pos];
		shadow = PGXP_ReadFIFO(fifo_pos);
		trace_record(PGXP_TRACE_EVENT_CB, !shadow->trace_id ? 1 : 0,
			shadow->trace_id, shadow->trace_stage, (uint8_t)slot, fifo_pos,
			shadow->value, shadow->value, shadow);
	}
}

void PGXP_DiagPacket(uint8_t opcode, unsigned words, unsigned abr,
		unsigned tex_mode, int mask_eval)
{
	current_packet = ++packet_ordinal;
	current_opcode = opcode;
	current_packet_words = (uint8_t)words;
	current_abr = (uint8_t)abr;
	current_tex_mode = (uint8_t)tex_mode;
	current_mask_eval = mask_eval != 0;
	memset(packet_vertices, 0, sizeof(packet_vertices));
	packet_vertex_count = 0;
}

static unsigned pgxp_diag_edge_delta_bin(float delta)
{
	if (delta <= 1.0e-6f)
		return 0;
	if (delta <= (1.0f / 64.0f))
		return 1;
	if (delta <= (1.0f / 8.0f))
		return 2;
	if (delta <= 0.25f)
		return 3;
	if (delta <= 0.5f)
		return 4;
	if (delta <= 1.0f)
		return 5;
	return 6;
}

static unsigned pgxp_diag_edge_packet_bin(uint64_t gap)
{
	if (gap <= 1)
		return 0;
	if (gap <= 4)
		return 1;
	if (gap <= 16)
		return 2;
	if (gap <= 64)
		return 3;
	if (gap <= 256)
		return 4;
	return 5;
}

void PGXP_DiagSeamEdge(enum PGXP_diag_seam_result result,
		float delta, uint64_t age)
{
	if ((unsigned)result >= PGXP_DIAG_SEAM_RESULTS)
		return;
	seam_results[result]++;
	seam_delta_bins[pgxp_diag_edge_delta_bin(delta)]++;
	seam_age_bins[pgxp_diag_edge_packet_bin(age)]++;
}

void PGXP_DiagSeamAccept(int32_t x0, int32_t y0,
		int32_t x1, int32_t y1, float delta0, float delta1,
		uint64_t age)
{
	if (!log_cb || seam_accept_samples >= PGXP_DIAG_SEAM_ACCEPT_SAMPLES ||
	    seam_accept_window_samples >= PGXP_DIAG_SEAM_ACCEPT_WINDOW_SAMPLES)
		return;
	seam_accept_samples++;
	seam_accept_window_samples++;
	log_cb(RETRO_LOG_INFO,
		"[pgxp_seam_accept] n=%u mf=%u packet=%llu op=%02x "
		"native=%d/%d-%d/%d endpoint_delta=%.6f/%.6f age=%llu\n",
		seam_accept_samples, mode_frame,
		(unsigned long long)current_packet, current_opcode,
		x0, y0, x1, y1, delta0, delta1,
		(unsigned long long)age);
}

void PGXP_DiagSeamPrimitive(unsigned observed_edges,
		unsigned moved_vertices, unsigned conflicts)
{
	seam_primitives++;
	seam_observed_edges += observed_edges;
	seam_moved_vertices += moved_vertices;
	seam_conflicts += conflicts;
}

static PGXP_diag_edge* pgxp_diag_find_edge(int32_t x0, int32_t y0,
		int32_t x1, int32_t y1)
{
	uint32_t hash = UINT32_C(2166136261);
	unsigned probe;

#define PGXP_EDGE_HASH_VALUE(value) do { \
	hash = (hash ^ (uint32_t)(value)) * UINT32_C(16777619); \
} while (0)
	PGXP_EDGE_HASH_VALUE(x0);
	PGXP_EDGE_HASH_VALUE(y0);
	PGXP_EDGE_HASH_VALUE(x1);
	PGXP_EDGE_HASH_VALUE(y1);
#undef PGXP_EDGE_HASH_VALUE

	for (probe = 0; probe < PGXP_DIAG_EDGE_SLOTS; probe++)
	{
		PGXP_diag_edge* edge = &edge_table[
			(hash + probe) & (PGXP_DIAG_EDGE_SLOTS - 1)];
		if (!edge->valid[0] && !edge->valid[1])
		{
			edge->native_x[0] = x0;
			edge->native_y[0] = y0;
			edge->native_x[1] = x1;
			edge->native_y[1] = y1;
			return edge;
		}
		if (edge->native_x[0] == x0 && edge->native_y[0] == y0 &&
		    edge->native_x[1] == x1 && edge->native_y[1] == y1)
			return edge;
	}

	edge_table_overflow++;
	return NULL;
}

static void pgxp_diag_compare_edge(const PGXP_diag_edge* edge,
		unsigned previous_class, unsigned current_class,
		float current_x0, float current_y0,
		float current_x1, float current_y1,
		uint64_t current_packet, uint8_t current_opcode,
		int current_gouraud, int current_invalid_w,
		unsigned upscale_shift)
{
	static const char* const kind_name[PGXP_DIAG_EDGE_KINDS] = {
		"uu", "ut", "tt"
	};
	unsigned kind;
	unsigned delta_bin;
	unsigned packet_bin;
	unsigned y_band;
	float scale = (float)(1u << upscale_shift);
	float inv_scale = 1.0f / scale;
	float delta[4];
	float max_delta;
	float average_y;
	uint64_t gap;

	if (edge->packet[previous_class] == current_packet)
		return;

	kind = previous_class == current_class ?
		(current_class ? 2u : 0u) : 1u;
	delta[0] = fabsf(edge->precise_x[previous_class][0] - current_x0) *
		inv_scale;
	delta[1] = fabsf(edge->precise_y[previous_class][0] - current_y0) *
		inv_scale;
	delta[2] = fabsf(edge->precise_x[previous_class][1] - current_x1) *
		inv_scale;
	delta[3] = fabsf(edge->precise_y[previous_class][1] - current_y1) *
		inv_scale;
	max_delta = delta[0];
	if (delta[1] > max_delta) max_delta = delta[1];
	if (delta[2] > max_delta) max_delta = delta[2];
	if (delta[3] > max_delta) max_delta = delta[3];
	delta_bin = pgxp_diag_edge_delta_bin(max_delta);
	gap = edge->packet[previous_class] > current_packet ?
		edge->packet[previous_class] - current_packet :
		current_packet - edge->packet[previous_class];
	packet_bin = pgxp_diag_edge_packet_bin(gap);
	average_y = ((float)edge->native_y[0] +
		(float)edge->native_y[1]) * 0.5f * inv_scale;
	y_band = average_y < 64.0f ? 0u : average_y < 128.0f ? 1u :
		average_y < 192.0f ? 2u : 3u;

	edge_compares[kind]++;
	edge_delta_bins[kind][delta_bin]++;
	if (gap <= 64)
		edge_near_delta_bins[kind][delta_bin]++;
	edge_packet_bins[kind][packet_bin]++;
	edge_delta_sum[kind] += max_delta;
	if (max_delta > edge_delta_max[kind])
		edge_delta_max[kind] = max_delta;
	if (delta_bin >= 2)
		edge_mismatch_y[kind][y_band]++;

	if (delta_bin < 2 || !log_cb || edge_samples >= PGXP_DIAG_EDGE_SAMPLES)
		return;
	edge_samples++;
	log_cb(RETRO_LOG_INFO,
		"[pgxp_edge_mismatch] n=%u mf=%u kind=%s "
		"prev=%llu/%02x curr=%llu/%02x native=%d/%d-%d/%d scale=%u "
		"prev_xy=%.4f/%.4f-%.4f/%.4f "
		"curr_xy=%.4f/%.4f-%.4f/%.4f delta=%.6f gap=%llu "
		"g=%u/%u invalid_w=%u/%u\n",
		edge_samples, mode_frame, kind_name[kind],
		(unsigned long long)edge->packet[previous_class],
		edge->opcode[previous_class],
		(unsigned long long)current_packet, current_opcode,
		edge->native_x[0], edge->native_y[0],
		edge->native_x[1], edge->native_y[1], 1u << upscale_shift,
		edge->precise_x[previous_class][0] * inv_scale,
		edge->precise_y[previous_class][0] * inv_scale,
		edge->precise_x[previous_class][1] * inv_scale,
		edge->precise_y[previous_class][1] * inv_scale,
		current_x0 * inv_scale, current_y0 * inv_scale,
		current_x1 * inv_scale, current_y1 * inv_scale,
		max_delta, (unsigned long long)gap,
		edge->gouraud[previous_class], current_gouraud != 0,
		edge->invalid_w[previous_class], current_invalid_w != 0);
}

static void pgxp_diag_observe_edges(
		const PGXP_diag_primitive_vertex vertices[3],
		int textured, int gouraud, int invalid_w, unsigned upscale_shift)
{
	unsigned i;
	unsigned current_class = textured != 0;

	if (edge_table_frame != mode_frame)
	{
		memset(edge_table, 0, sizeof(edge_table));
		edge_table_frame = mode_frame;
	}

	for (i = 0; i < 3; i++)
	{
		unsigned j = (i + 1) % 3;
		int32_t x0 = vertices[i].native_x;
		int32_t y0 = vertices[i].native_y;
		int32_t x1 = vertices[j].native_x;
		int32_t y1 = vertices[j].native_y;
		float px0 = vertices[i].precise_after_x;
		float py0 = vertices[i].precise_after_y;
		float px1 = vertices[j].precise_after_x;
		float py1 = vertices[j].precise_after_y;
		PGXP_diag_edge* edge;

		if (x0 == x1 && y0 == y1)
			continue;
		if (x1 < x0 || (x1 == x0 && y1 < y0))
		{
			int32_t temp_i;
			float temp_f;
			temp_i = x0; x0 = x1; x1 = temp_i;
			temp_i = y0; y0 = y1; y1 = temp_i;
			temp_f = px0; px0 = px1; px1 = temp_f;
			temp_f = py0; py0 = py1; py1 = temp_f;
		}

		edge_observations[current_class]++;
		edge = pgxp_diag_find_edge(x0, y0, x1, y1);
		if (!edge)
			continue;
		if (edge->valid[current_class])
			pgxp_diag_compare_edge(edge, current_class, current_class,
				px0, py0, px1, py1, current_packet, current_opcode,
				gouraud, invalid_w, upscale_shift);
		if (edge->valid[!current_class])
			pgxp_diag_compare_edge(edge, !current_class, current_class,
				px0, py0, px1, py1, current_packet, current_opcode,
				gouraud, invalid_w, upscale_shift);

		edge->precise_x[current_class][0] = px0;
		edge->precise_y[current_class][0] = py0;
		edge->precise_x[current_class][1] = px1;
		edge->precise_y[current_class][1] = py1;
		edge->packet[current_class] = current_packet;
		edge->opcode[current_class] = current_opcode;
		edge->gouraud[current_class] = gouraud != 0;
		edge->invalid_w[current_class] = invalid_w != 0;
		edge->valid[current_class] = 1;
	}
}

void PGXP_DiagPrimitive(const PGXP_diag_primitive_vertex vertices[3],
		int invalid_w, int tolerance, unsigned upscale_shift)
{
	int textured = !!(current_opcode & 0x04);
	int gouraud = !!(current_opcode & 0x10);
	int reverted = 0;
	int32_t average_y = (vertices[0].native_y + vertices[1].native_y +
		vertices[2].native_y) / 3;
	unsigned y_band = average_y < 64 ? 0 : average_y < 128 ? 1 :
		average_y < 192 ? 2 : 3;
	unsigned bucket = (textured << 4) | (gouraud << 3) |
		((invalid_w != 0) << 2) | y_band;
	unsigned source_mask = 0;
	unsigned composition;
	unsigned i;

	primitive_total++;
	primitive_class[textured][gouraud][invalid_w != 0]++;
	primitive_y_band[y_band]++;
	pgxp_diag_observe_edges(vertices, textured, gouraud, invalid_w,
		upscale_shift);
	for (i = 0; i < 3; i++)
	{
		if (i < packet_vertex_count &&
		    packet_vertices[i].stage == PGXP_TRACE_SRA5)
			primitive_sra_vertices++;
		if (vertices[i].precise_before_x != vertices[i].precise_after_x ||
		    vertices[i].precise_before_y != vertices[i].precise_after_y)
			reverted = 1;
	}
	for (i = 0; i < packet_vertex_count; i++)
	{
		unsigned source = packet_vertices[i].source;
		unsigned stage = packet_vertices[i].stage;
		if (source < 3)
		{
			source_mask |= 1u << source;
			if (stage < PGXP_DIAG_TRACE_STAGES)
				primitive_source_stage[bucket][source][stage]++;
			if (source == PGXP_DIAG_VERTEX_NATIVE &&
			    packet_vertices[i].reason < PGXP_TRACE_REASON_COUNT)
			{
				primitive_native_reason[bucket][packet_vertices[i].reason]++;
				if (stage == PGXP_TRACE_SRA5)
					primitive_native_sra5_reason[bucket]
						[packet_vertices[i].reason]++;
			}
		}
	}
	if (source_mask == (1u << PGXP_DIAG_VERTEX_TRACKED))
		composition = 0;
	else if (source_mask == (1u << PGXP_DIAG_VERTEX_NATIVE))
		composition = 2;
	else if (source_mask & (1u << PGXP_DIAG_VERTEX_CACHE) || !source_mask)
		composition = 3;
	else
		composition = 1;
	primitive_composition[bucket][composition]++;
	if (reverted)
		primitive_tolerance_reverts++;

	if (!log_cb || primitive_bucket_samples[bucket] >= 8)
		return;
	primitive_bucket_samples[bucket]++;
	log_cb(RETRO_LOG_INFO,
		"[pgxp_primitive] bucket=%u sample=%u mf=%u packet=%llu op=%02x "
		"words=%u textured=%d gouraud=%d quad=%u abr=%u texmode=%u "
		"mask=%u invalid_w=%d tol=%d reverted=%d yband=%u vertices=%u\n",
		bucket, primitive_bucket_samples[bucket], mode_frame,
		(unsigned long long)current_packet, current_opcode,
		current_packet_words, textured, gouraud, !!(current_opcode & 0x08),
		current_abr, current_tex_mode, current_mask_eval, invalid_w,
		tolerance, reverted, y_band, packet_vertex_count);
	for (i = 0; i < 3; i++)
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_primitive_vertex] bucket=%u sample=%u v=%u native=%d/%d "
			"before=%.3f/%.3f/%.6f after=%.3f/%.3f/%.6f uv=%u/%u\n",
			bucket, primitive_bucket_samples[bucket], i, vertices[i].native_x,
			vertices[i].native_y, vertices[i].precise_before_x,
			vertices[i].precise_before_y, vertices[i].precise_before_w,
			vertices[i].precise_after_x, vertices[i].precise_after_y,
			vertices[i].precise_after_w, vertices[i].u, vertices[i].v);
	}
	for (i = 0; i < packet_vertex_count; i++)
	{
		const PGXP_diag_packet_vertex* tracked = &packet_vertices[i];
		log_cb(RETRO_LOG_INFO,
			"[pgxp_primitive_trace] bucket=%u sample=%u observed=%u "
			"source=%u id=%llu stage=%u reason=%u addr=%08x value=%08x\n",
			bucket, primitive_bucket_samples[bucket], i, tracked->source,
			(unsigned long long)tracked->trace_id, tracked->stage,
			tracked->reason,
			tracked->addr, tracked->value);
	}
}

static int pgxp_diag_area_sign(double area)
{
	return area < 0.0 ? -1 : area > 0.0 ? 1 : 0;
}

static unsigned pgxp_diag_area_band(double area)
{
	double magnitude = area < 0.0 ? -area : area;
	if (magnitude == 0.0) return 0;
	if (magnitude < 1.0) return 1;
	if (magnitude < 16.0) return 2;
	if (magnitude < 256.0) return 3;
	return 4;
}

void PGXP_DiagGPUPrimitive(const PGXP_diag_primitive_vertex vertices[3],
		unsigned quad_part, int invalid_w, unsigned upscale_shift)
{
	double scale = (double)(UINT32_C(1) << upscale_shift);
	double scale2 = scale * scale;
	double native_area =
		((double)vertices[1].native_x - vertices[0].native_x) *
		((double)vertices[2].native_y - vertices[0].native_y) -
		((double)vertices[2].native_x - vertices[0].native_x) *
		((double)vertices[1].native_y - vertices[0].native_y);
	double precise_area =
		((double)vertices[1].precise_after_x - vertices[0].precise_after_x) *
		((double)vertices[2].precise_after_y - vertices[0].precise_after_y) -
		((double)vertices[2].precise_after_x - vertices[0].precise_after_x) *
		((double)vertices[1].precise_after_y - vertices[0].precise_after_y);
	int native_sign;
	int precise_sign;
	int sign_disagreement;
	int oversize_x;
	int oversize_y;
	int pair_native_fold = 0;
	int pair_precise_fold = 0;
	int average_y;
	unsigned y_band;
	unsigned part = quad_part <= 2 ? quad_part : 0;
	unsigned i;

	native_area /= scale2;
	precise_area /= scale2;
	native_sign = pgxp_diag_area_sign(native_area);
	precise_sign = pgxp_diag_area_sign(precise_area);
	sign_disagreement = native_sign != precise_sign;
	oversize_x = 0;
	oversize_y = 0;
	for (i = 0; i < 3; i++)
	{
		unsigned j = (i + 1) % 3;
		int64_t dx = (int64_t)vertices[i].native_x - vertices[j].native_x;
		int64_t dy = (int64_t)vertices[i].native_y - vertices[j].native_y;
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		if (dx >= ((int64_t)1024 << upscale_shift)) oversize_x = 1;
		if (dy >= ((int64_t)512 << upscale_shift)) oversize_y = 1;
	}
	average_y = (vertices[0].native_y + vertices[1].native_y +
		vertices[2].native_y) / 3;
	average_y >>= upscale_shift;
	y_band = average_y < 64 ? 0 : average_y < 128 ? 1 :
		average_y < 192 ? 2 : 3;

	window.gpu_triangles[part]++;
	window.gpu_area_native_sign[native_sign + 1]++;
	window.gpu_area_precise_sign[precise_sign + 1]++;
	window.gpu_area_near_native[pgxp_diag_area_band(native_area)]++;
	window.gpu_area_near_precise[pgxp_diag_area_band(precise_area)]++;
	if (sign_disagreement)
	{
		window.gpu_area_sign_disagreements++;
		window.gpu_area_anomaly_y[y_band]++;
	}
	if (!precise_sign)
		window.gpu_area_precise_zero++;
	if (invalid_w)
		window.gpu_triangle_invalid_w++;
	if (oversize_x)
		window.gpu_oversize_x++;
	if (oversize_y)
		window.gpu_oversize_y++;
	if ((oversize_x || oversize_y) && sign_disagreement)
		window.gpu_oversize_sign_disagreements++;

	if (part == 1)
	{
		gpu_quad_native_sign = native_sign;
		gpu_quad_precise_sign = precise_sign;
		gpu_quad_invalid_w = invalid_w;
		gpu_quad_pending = 1;
	}
	else if (part == 2 && gpu_quad_pending)
	{
		/* The decoder presents the second half as [1,2,3], while the hardware
		 * renderer submits that half as [3,2,1].  Equal decoder-space signs
		 * therefore mean opposite renderer-space winding: a folded quad. */
		int native_fold = gpu_quad_native_sign != 0 && native_sign != 0 &&
			gpu_quad_native_sign == native_sign;
		int precise_fold = gpu_quad_precise_sign != 0 && precise_sign != 0 &&
			gpu_quad_precise_sign == precise_sign;
		window.gpu_quad_pairs++;
		if (native_fold) window.gpu_quad_native_fold++;
		if (precise_fold) window.gpu_quad_precise_fold++;
		if (!native_fold && precise_fold) window.gpu_quad_fold_introduced++;
		if (native_fold && !precise_fold) window.gpu_quad_fold_removed++;
		if (!native_fold && precise_fold)
		{
			window.gpu_quad_fold_introduced_y[y_band]++;
			window.gpu_quad_fold_introduced_opcode[current_opcode & 0x1f]++;
		}
		if (precise_fold) window.gpu_area_anomaly_y[y_band]++;
		pair_native_fold = native_fold;
		pair_precise_fold = precise_fold;
		invalid_w |= gpu_quad_invalid_w;
		gpu_quad_pending = 0;
	}
	else if (part == 0)
		gpu_quad_pending = 0;

	if (log_cb && gpu_area_window_samples < 12 &&
	    (sign_disagreement || oversize_x || oversize_y ||
	     pair_precise_fold))
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_gpu_area] n=%u mf=%u packet=%llu op=%02x part=%u "
			"native=%.6f/%d precise=%.6f/%d invalid_w=%d "
			"oversize=%d/%d yband=%u vertices="
			"%d/%d:%.3f/%.3f/%.6f,%d/%d:%.3f/%.3f/%.6f,"
			"%d/%d:%.3f/%.3f/%.6f\n",
			gpu_area_samples + 1, mode_frame,
			(unsigned long long)current_packet, current_opcode, part,
			native_area, native_sign, precise_area, precise_sign, invalid_w,
			oversize_x, oversize_y, y_band,
			vertices[0].native_x, vertices[0].native_y,
			vertices[0].precise_after_x, vertices[0].precise_after_y,
			vertices[0].precise_after_w,
			vertices[1].native_x, vertices[1].native_y,
			vertices[1].precise_after_x, vertices[1].precise_after_y,
			vertices[1].precise_after_w,
			vertices[2].native_x, vertices[2].native_y,
			vertices[2].precise_after_x, vertices[2].precise_after_y,
			vertices[2].precise_after_w);
		gpu_area_samples++;
		gpu_area_window_samples++;
	}
	if (log_cb && gpu_fold_window_samples < 8 &&
	    !pair_native_fold && pair_precise_fold)
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_gpu_fold] n=%u mf=%u packet=%llu op=%02x yband=%u "
			"invalid_w=%d native_sign=%d/%d precise_sign=%d/%d vertices="
			"%d/%d:%.3f/%.3f/%.6f,%d/%d:%.3f/%.3f/%.6f,"
			"%d/%d:%.3f/%.3f/%.6f\n",
			gpu_fold_samples + 1, mode_frame,
			(unsigned long long)current_packet, current_opcode, y_band,
			invalid_w, gpu_quad_native_sign, native_sign,
			gpu_quad_precise_sign, precise_sign,
			vertices[0].native_x, vertices[0].native_y,
			vertices[0].precise_after_x, vertices[0].precise_after_y,
			vertices[0].precise_after_w,
			vertices[1].native_x, vertices[1].native_y,
			vertices[1].precise_after_x, vertices[1].precise_after_y,
			vertices[1].precise_after_w,
			vertices[2].native_x, vertices[2].native_y,
			vertices[2].precise_after_x, vertices[2].precise_after_y,
			vertices[2].precise_after_w);
		gpu_fold_samples++;
		gpu_fold_window_samples++;
	}
}

void PGXP_DiagLineHack(int32_t average_y, int rejected_w,
		float w0, float w1, float w2)
{
	unsigned y_band = average_y < 64 ? 0 : average_y < 128 ? 1 :
		average_y < 256 ? 2 : 3;
	window.line_hack_candidates[y_band]++;
	if (!rejected_w)
		return;
	window.line_hack_w_rejects[y_band]++;
	if (y_band == 3 && log_cb && line_hack_samples < 64)
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_line_hack_reject] n=%u mf=%u packet=%llu op=%02x "
			"y=%d w=%.6f/%.6f/%.6f\n",
			line_hack_samples + 1, mode_frame,
			(unsigned long long)current_packet, current_opcode,
			average_y, w0, w1, w2);
		line_hack_samples++;
	}
}

static void trace_sample_tracked(unsigned slot, uint32_t value,
		const PGXP_value* shadow, float x, float y, float w, int valid_w)
{
	uint32_t i;
	int16_t native_x;
	int16_t native_y;

	if (!shadow || !shadow->trace_id || !log_cb ||
	    trace_tracked_samples >= 64 || shadow->trace_stage != PGXP_TRACE_SRA5)
		return;
	for (i = 0; i < trace_tracked_samples; i++)
		if (trace_tracked_ids[i] == shadow->trace_id)
			return;

	trace_tracked_ids[trace_tracked_samples++] = shadow->trace_id;
	native_x = (int16_t)(value & 0xffff);
	native_y = (int16_t)(value >> 16);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_trace_tracked] sample=%u id=%llu mf=%u packet=%llu "
		"op=%02x words=%u slot=%u textured=%u gouraud=%u quad=%u "
		"abr=%u texmode=%u mask=%u addr=%08x native=%d/%d "
		"precise=%.3f/%.3f delta=%.3f/%.3f w=%.6f valid_w=%d\n",
		trace_tracked_samples, (unsigned long long)shadow->trace_id,
		mode_frame, (unsigned long long)current_packet, current_opcode,
		current_packet_words, slot, !!(current_opcode & 0x04),
		!!(current_opcode & 0x10), !!(current_opcode & 0x08),
		current_abr, current_tex_mode, current_mask_eval,
		cb_provenance[slot].addr, native_x, native_y, x, y,
		x - native_x, y - native_y, w, valid_w);
}

static void vertex_coherence_observe(int32_t native_x, int32_t native_y,
		float x, float y, float w, unsigned stage)
{
	uint32_t key = ((uint32_t)(uint16_t)native_y << 16) |
		(uint16_t)native_x;
	uint32_t bucket = (key * UINT32_C(2654435761)) >> 18;
	PGXP_diag_coherence_vertex* ways = coherence_vertices[bucket];
	PGXP_diag_coherence_vertex* previous = NULL;
	PGXP_diag_coherence_vertex* replacement = &ways[0];
	unsigned y_band = native_y < 64 ? 0 : native_y < 128 ? 1 :
		native_y < 256 ? 2 : 3;
	unsigned i;

	for (i = 0; i < PGXP_DIAG_COHERENCE_WAYS; i++)
	{
		if (ways[i].valid && ways[i].key == key &&
		    ways[i].frame == mode_frame && ways[i].packet < current_packet &&
		    (!previous || ways[i].packet > previous->packet))
			previous = &ways[i];
		if (!ways[i].valid || ways[i].frame != mode_frame ||
		    ways[i].packet < replacement->packet)
			replacement = &ways[i];
	}

	if (previous)
	{
		float dx = fabsf(x - previous->x);
		float dy = fabsf(y - previous->y);
		float delta = dx > dy ? dx : dy;
		float w_delta = w > 0.0f && previous->w > 0.0f ?
			fabsf(w - previous->w) : 0.0f;
		unsigned delta_class = delta == 0.0f ? 0 : delta <= 0.25f ? 1 :
			delta <= 0.5f ? 2 : delta < 1.0f ? 3 :
			delta <= 4.0f ? 4 : 5;
		window.vertex_coherence_delta[y_band][delta_class]++;
		window.vertex_coherence_w_delta[y_band] += w_delta;
		if (stage != previous->stage)
			window.vertex_coherence_cross_stage[y_band]++;
		if (y_band == 3 && delta > 0.25f && log_cb &&
		    vertex_coherence_samples < 64)
		{
			log_cb(RETRO_LOG_INFO,
				"[pgxp_vertex_coherence] n=%u mf=%u packet=%llu prev=%llu "
				"native=%d/%d precise=%.3f/%.3f prev_xy=%.3f/%.3f "
				"delta=%.3f w=%.3f/%.3f dw=%.3f stage=%u/%u\n",
				vertex_coherence_samples + 1, mode_frame,
				(unsigned long long)current_packet,
				(unsigned long long)previous->packet, native_x, native_y,
				x, y, previous->x, previous->y, delta, w, previous->w,
				w_delta, stage, previous->stage);
			vertex_coherence_samples++;
		}
	}

	replacement->key = key;
	replacement->frame = mode_frame;
	replacement->packet = current_packet;
	replacement->x = x;
	replacement->y = y;
	replacement->w = w;
	replacement->stage = (uint8_t)stage;
	replacement->valid = 1;
}

void PGXP_DiagGTEVertex(float x, float y, float z, uint32_t value)
{
	window.gte_vertices++;
	hash_event(3, value, 0);
	window.event_hash = hash_bytes(window.event_hash, &x, sizeof(x));
	window.event_hash = hash_bytes(window.event_hash, &y, sizeof(y));
	window.event_hash = hash_bytes(window.event_hash, &z, sizeof(z));
}

void PGXP_DiagProjectionZ(double raw_z, float precise_z,
		uint16_t architectural_z, uint16_t h)
{
	unsigned band;
	unsigned delta_class;
	double floor_z = (double)h * 0.5;
	float reference_z = (float)architectural_z;
	double delta;

	if (reference_z < (float)floor_z)
		reference_z = (float)floor_z;
	delta = (double)precise_z - (double)reference_z;
	if (delta < 0.0)
		delta = -delta;
	if (delta == 0.0)
		delta_class = 0;
	else if (delta <= 0.125)
		delta_class = 1;
	else if (delta <= 0.25)
		delta_class = 2;
	else if (delta <= 0.5)
		delta_class = 3;
	else if (delta < 1.0)
		delta_class = 4;
	else
		delta_class = 5;
	window.projection_z_delta[delta_class]++;
	window.projection_z_delta_sum += delta;
	if (delta > window.projection_z_delta_max)
		window.projection_z_delta_max = delta;
	pending_projection_z_delta = (uint8_t)(delta_class + 1);

	if (raw_z <= 0.0)
		band = 0;
	else if (raw_z <= floor_z)
		band = 1;
	else if (raw_z <= 65535.0)
		band = 2;
	else if (raw_z <= 73727.0)
		band = 3;
	else if (raw_z <= 98302.5)
		band = 4;
	else if (raw_z <= 131070.0)
		band = 5;
	else
		band = 6;
	window.projection_z_band[band]++;
	pending_projection_z_band = (uint8_t)(band + 1);
	if (raw_z > window.projection_z_max)
		window.projection_z_max = raw_z;
	hash_event(8, (uint32_t)band | ((uint32_t)delta_class << 8),
		(uint32_t)precise_z);
}

void PGXP_DiagVertex(enum PGXP_diag_vertex_source source,
		unsigned slot, uint32_t value, const PGXP_value* shadow,
		float x, float y, float w, int32_t native_x, int32_t native_y,
		int valid_w,
		int valid_xy, int value_match)
{
	const PGXP_value* live_shadow = NULL;
	uint32_t source_addr = slot < 16 ? cb_provenance[slot].addr : 0;
	uint32_t physical_addr = source_addr & UINT32_C(0x1fffffff);
	unsigned live_state;
	uint8_t terminal_reason;
	unsigned writer_width;
	unsigned writer_stage;
	unsigned delta_class;
	float dx = x - (float)native_x;
	float dy = y - (float)native_y;
	float abs_dx = dx < 0.0f ? -dx : dx;
	float abs_dy = dy < 0.0f ? -dy : dy;
	float max_delta = abs_dx > abs_dy ? abs_dx : abs_dy;
	if (source <= PGXP_DIAG_VERTEX_NATIVE && trace_metadata_valid(shadow) &&
	    (shadow->trace_reserved[6] & 0x0f) >= 1 &&
	    (shadow->trace_reserved[6] & 0x0f) <= 7)
	{
		unsigned source_band = (shadow->trace_reserved[6] & 0x0f) - 1;
		window.rendered_z_linked[source]++;
		if (source_band >= 3)
		{
			window.rendered_z_far[source]++;
			window.rendered_z_band[source][source_band - 3]++;
		}
	}
	if (source <= PGXP_DIAG_VERTEX_NATIVE && trace_metadata_valid(shadow) &&
	    (shadow->trace_reserved[6] >> 4) >= 1 &&
	    (shadow->trace_reserved[6] >> 4) <= 6)
		window.rendered_z_delta[source]
			[(shadow->trace_reserved[6] >> 4) - 1]++;
	if (source == PGXP_DIAG_VERTEX_TRACKED)
		terminal_reason = 0;
	else if (source == PGXP_DIAG_VERTEX_CACHE)
		terminal_reason = 6;
	else if (!trace_metadata_valid(shadow))
		terminal_reason = 1;
	else if (!valid_xy && !value_match)
		terminal_reason = 4;
	else if (!valid_xy)
		terminal_reason = 2;
	else
		terminal_reason = 3;
	/* SwanStation keeps the DMA source address beside each FIFO word and
	 * rereads its PGXP shadow here.  Beetle instead carries the shadow
	 * snapshot taken at FIFO insertion.  Measure that semantic gap without
	 * changing rendering.  GPU MMIO addresses are not source memory in
	 * SwanStation; RAM mirrors and scratchpad are. */
	if (physical_addr < UINT32_C(0x00800000) ||
	    (physical_addr >= UINT32_C(0x1f800000) &&
	     physical_addr < UINT32_C(0x1f800400)))
		live_shadow = ReadMem(source_addr);
	if (!live_shadow)
		live_state = 1;
	else if ((live_shadow->flags & VALID_01) != VALID_01)
		live_state = 2;
	else if (live_shadow->value != value)
		live_state = 3;
	else
		live_state = 0;
	if (source <= PGXP_DIAG_VERTEX_NATIVE)
		window.vertex_live_state[source][live_state]++;
	if (source == PGXP_DIAG_VERTEX_TRACKED && live_state != 0 &&
	    vertex_live_samples < 64 && log_cb)
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_vertex_live_reject] n=%u mf=%u slot=%u addr=%08x "
			"psx=%08x snapshot=%08x/%08x live=%08x/%08x state=%u\n",
			vertex_live_samples + 1, mode_frame, slot, source_addr, value,
			shadow ? shadow->value : 0, shadow ? shadow->flags : 0,
			live_shadow ? live_shadow->value : 0,
			live_shadow ? live_shadow->flags : 0, live_state);
		vertex_live_samples++;
	}

	writer_width = cb_provenance[slot].writer.valid ?
		cb_provenance[slot].writer.width : 0;
	if (writer_width >= PGXP_DIAG_WRITER_WIDTHS)
		writer_width = 0;
	writer_stage = cb_provenance[slot].writer.stage;
	if (writer_stage >= PGXP_DIAG_TRACE_STAGES)
		writer_stage = PGXP_TRACE_NONE;
	if (max_delta == 0.0f)
		delta_class = 0;
	else if (max_delta <= 0.25f)
		delta_class = 1;
	else if (max_delta <= 0.5f)
		delta_class = 2;
	else if (max_delta < 1.0f)
		delta_class = 3;
	else if (max_delta <= 4.0f)
		delta_class = 4;
	else
		delta_class = 5;
	if (source <= PGXP_DIAG_VERTEX_NATIVE)
	{
		float delta[2];
		float abs_delta[2];
		unsigned axis;
		delta[0] = dx;
		delta[1] = dy;
		abs_delta[0] = abs_dx;
		abs_delta[1] = abs_dy;
		window.vertex_delta_class[source][delta_class]++;
		for (axis = 0; axis < 2; axis++)
		{
			unsigned sign = delta[axis] < 0.0f ? 0 :
				delta[axis] > 0.0f ? 2 : 1;
			window.vertex_axis_sign[source][axis][sign]++;
			window.vertex_delta_abs_sum[source][axis] += abs_delta[axis];
			window.vertex_delta_sum[source][axis] += delta[axis];
			if (abs_delta[axis] > window.vertex_delta_max[source][axis])
				window.vertex_delta_max[source][axis] = abs_delta[axis];
			if (abs_delta[axis] == 0.0f)
				window.vertex_axis_exact[source][axis]++;
			else if (abs_delta[axis] < 1.0f)
				window.vertex_axis_subpixel[source][axis]++;
			else
				window.vertex_axis_ge_one[source][axis]++;
		}
		if (source == PGXP_DIAG_VERTEX_TRACKED)
		{
			unsigned stage = shadow &&
				shadow->trace_stage < PGXP_DIAG_TRACE_STAGES ?
				shadow->trace_stage : PGXP_TRACE_NONE;
			window.vertex_stage_delta[stage][delta_class]++;
			window.vertex_writer_delta[writer_width][delta_class]++;
		}
	}
	if (packet_vertex_count < 3)
	{
		PGXP_diag_packet_vertex* packet_vertex =
			&packet_vertices[packet_vertex_count++];
		packet_vertex->trace_id = shadow ? shadow->trace_id : 0;
		packet_vertex->addr = cb_provenance[slot].addr;
		packet_vertex->value = value;
		packet_vertex->source = (uint8_t)source;
		packet_vertex->stage = shadow ? shadow->trace_stage : 0;
		packet_vertex->reason = terminal_reason;
	}
	window.trace_terminal[source][terminal_reason]++;
	if (shadow)
		trace_record(PGXP_TRACE_EVENT_VERTEX, terminal_reason,
			shadow->trace_id, PGXP_TRACE_VERTEX, (uint8_t)slot,
			cb_provenance[slot].addr, value, shadow->value, shadow);
	if (source == PGXP_DIAG_VERTEX_NATIVE &&
	    trace_metadata_valid(shadow))
		trace_dump_chain(shadow->trace_id, terminal_reason);
	else if (source == PGXP_DIAG_VERTEX_TRACKED &&
	         trace_metadata_valid(shadow))
		trace_sample_tracked(slot, value, shadow, x, y, w, valid_w);

	switch (source)
	{
		case PGXP_DIAG_VERTEX_TRACKED: window.vertex_tracked++; break;
		case PGXP_DIAG_VERTEX_CACHE:   window.vertex_cache++;   break;
		default:                       window.vertex_native++;  break;
	}
	if (valid_w)
		window.vertex_valid_w++;
	else if (source == PGXP_DIAG_VERTEX_TRACKED)
		writer_tracked_invalid_w++;
	if (source == PGXP_DIAG_VERTEX_TRACKED)
	{
		unsigned stage = shadow && shadow->trace_stage < PGXP_DIAG_TRACE_STAGES ?
			shadow->trace_stage : PGXP_TRACE_NONE;
		vertex_coherence_observe(native_x, native_y, x, y,
			valid_w ? w : 0.0f, stage);
		writer_tracked[writer_width]++;
		if ((cb_provenance[slot].writer.flags & VALID_2) == VALID_2)
			writer_tracked_source_w[writer_width]++;
		else if (valid_w)
		{
			writer_tracked_retained_w[writer_width]++;
			if (writer_width == 2 && writer_tracked_samples < 48 && log_cb)
			{
				log_cb(RETRO_LOG_INFO,
					"[pgxp_writer_retained_w] n=%u mf=%u slot=%u "
					"addr=%08x gpu=%08x shadow=%08x shadow_flags=%08x "
					"writer_mf=%u writer_value=%08x writer_flags=%08x "
					"writer_stage=%u trace=%llu age=%u w=%.6f\n",
					writer_tracked_samples + 1, mode_frame, slot,
					cb_provenance[slot].addr, value,
					shadow ? shadow->value : 0,
					shadow ? shadow->flags : 0,
					cb_provenance[slot].writer.frame,
					cb_provenance[slot].writer.value,
					cb_provenance[slot].writer.flags, writer_stage,
					(unsigned long long)cb_provenance[slot].writer.trace_id,
					cb_provenance[slot].writer.frame <= mode_frame ?
					mode_frame - cb_provenance[slot].writer.frame : 0, w);
				writer_tracked_samples++;
			}
		}
	}
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
		writer_native[writer_width]++;
		writer_native_reason[writer_width][terminal_reason]++;
		writer_native_stage[writer_width][writer_stage]++;
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
				"[pgxp_vertex_writer] n=%u mf=%u slot=%u seen=%u "
				"width=%u writer_mf=%u value=%08x flags=%08x "
				"stage=%u trace=%llu age=%u reason=%u\n",
				vertex_samples + 1, mode_frame, slot,
				cb_provenance[slot].writer.valid,
				writer_width, cb_provenance[slot].writer.frame,
				cb_provenance[slot].writer.value,
				cb_provenance[slot].writer.flags, writer_stage,
				(unsigned long long)cb_provenance[slot].writer.trace_id,
				cb_provenance[slot].writer.valid &&
				cb_provenance[slot].writer.frame <= mode_frame ?
				mode_frame - cb_provenance[slot].writer.frame : 0,
				terminal_reason);
			writer_samples++;
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

int PGXP_DiagVertexWEligible(unsigned slot, const PGXP_value* shadow)
{
	unsigned stage = trace_metadata_valid(shadow) ? shadow->trace_stage :
		PGXP_TRACE_NONE;
	unsigned width = slot < 16 && cb_provenance[slot].writer.valid ?
		cb_provenance[slot].writer.width : 0;
	int eligible = stage != PGXP_TRACE_NONE;
	if (width >= PGXP_DIAG_WRITER_WIDTHS)
		width = 0;
	if (eligible)
		window.vertex_w_gate_kept++;
	else
	{
		window.vertex_w_gate_rejected++;
		window.vertex_w_gate_rejected_stage[stage]++;
		window.vertex_w_gate_rejected_width[width]++;
	}
	return eligible;
}

void PGXP_DiagNCLIP(int32_t native_value, int32_t precise_value,
		int32_t reference_value, int32_t applied_value)
{
	int reference_disagreement =
		((precise_value < 0) != (reference_value < 0)) ||
		((precise_value == 0) != (reference_value == 0));
	window.nclip_compares++;
	if (pending_nclip_z_mask)
	{
		window.nclip_xy_only_compares++;
		if ((native_value < 0) != (applied_value < 0) ||
		    (native_value == 0) != (applied_value == 0))
			window.nclip_xy_only_sign_changes++;
	}
	if ((native_value < 0) != (precise_value < 0) ||
	    (native_value == 0) != (precise_value == 0))
		window.nclip_sign_disagreements++;
	if ((native_value < 0) != (applied_value < 0) ||
	    (native_value == 0) != (applied_value == 0))
		window.nclip_applied_sign_changes++;
	if (precise_value == 0 && native_value != 0)
		window.nclip_precise_zero_fallbacks++;
	if (reference_disagreement)
		window.nclip_reference_sign_disagreements++;
	if ((native_value < 0) != (reference_value < 0) ||
	    (native_value == 0) != (reference_value == 0))
		window.nclip_reference_native_disagreements++;
	if (precise_value == 0) window.nclip_double_zero++;
	if (reference_value == 0) window.nclip_reference_zero++;
	if (reference_disagreement && log_cb && nclip_reference_window_samples < 8)
	{
		log_cb(RETRO_LOG_INFO,
			"[pgxp_nclip_reference] n=%u mf=%u native=%d double=%d "
			"reference=%d applied=%d\n",
			nclip_reference_samples + 1, mode_frame, native_value,
			precise_value, reference_value, applied_value);
		nclip_reference_samples++;
		nclip_reference_window_samples++;
	}
	hash_event(7, (uint32_t)native_value, (uint32_t)precise_value);
}

void PGXP_DiagNCLIPValidity(unsigned invalid_mask, unsigned mismatch_mask,
		uint32_t sxy0, uint32_t sxy1, uint32_t sxy2,
		const PGXP_value* shadow0, const PGXP_value* shadow1,
		const PGXP_value* shadow2)
{
	const PGXP_value* shadow[3] = { shadow0, shadow1, shadow2 };
	unsigned i;
	unsigned z_invalid_mask;
	int32_t average_y;
	unsigned y_band;
	invalid_mask &= 7u;
	mismatch_mask &= 7u;
	z_invalid_mask = ((shadow0->flags & VALID_2) != VALID_2 ? 1u : 0u) |
		((shadow1->flags & VALID_2) != VALID_2 ? 2u : 0u) |
		((shadow2->flags & VALID_2) != VALID_2 ? 4u : 0u);
	pending_nclip_z_mask = !invalid_mask ? z_invalid_mask : 0;
	window.nclip_validity_attempts++;
	window.nclip_invalid_mask[invalid_mask]++;
	window.nclip_mismatch_mask[mismatch_mask]++;
	window.nclip_z_invalid_mask[z_invalid_mask]++;
	if (!invalid_mask && z_invalid_mask)
	{
		window.nclip_xy_only_attempts++;
		average_y = ((int16_t)(sxy0 >> 16) + (int16_t)(sxy1 >> 16) +
			(int16_t)(sxy2 >> 16)) / 3;
		y_band = average_y < 64 ? 0 : average_y < 128 ? 1 :
			average_y < 256 ? 2 : 3;
		window.nclip_xy_only_y_band[y_band]++;
		if (log_cb && nclip_xy_only_window_samples < 8 &&
		    nclip_xy_only_samples < 64)
		{
			log_cb(RETRO_LOG_INFO,
				"[pgxp_nclip_xy_only] n=%u mf=%u zmask=%u yband=%u "
				"native=%08x/%08x/%08x flags=%08x/%08x/%08x\n",
				nclip_xy_only_samples + 1, mode_frame, z_invalid_mask,
				y_band, sxy0, sxy1, sxy2, shadow0->flags,
				shadow1->flags, shadow2->flags);
			nclip_xy_only_samples++;
			nclip_xy_only_window_samples++;
		}
	}
	if (!invalid_mask)
		return;
	window.nclip_validity_invalid++;
	if (!log_cb || nclip_invalid_window_samples >= 8)
		return;
	log_cb(RETRO_LOG_INFO,
		"[pgxp_nclip_invalid] n=%u mf=%u invalid=%u mismatch=%u "
		"native=%08x/%08x/%08x\n",
		nclip_invalid_samples + 1, mode_frame, invalid_mask, mismatch_mask,
		sxy0, sxy1, sxy2);
	for (i = 0; i < 3; i++)
		log_cb(RETRO_LOG_INFO,
			"[pgxp_nclip_shadow] n=%u v=%u value=%08x flags=%08x "
			"xy=%.6f/%.6f count=%u stage=%u trace=%llu\n",
			nclip_invalid_samples + 1, i, shadow[i]->value,
			shadow[i]->flags, shadow[i]->x, shadow[i]->y,
			shadow[i]->count, shadow[i]->trace_stage,
			(unsigned long long)shadow[i]->trace_id);
	nclip_invalid_samples++;
	nclip_invalid_window_samples++;
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
		"vertex=%llu/%llu/%llu w=%llu nclip=%llu/%llu/%llu/%llu "
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
		(unsigned long long)window.nclip_applied_sign_changes,
		(unsigned long long)window.nclip_precise_zero_fallbacks,
		(unsigned long long)window.vertex_native_invalid_xy,
		(unsigned long long)window.vertex_native_value_mismatch,
		(unsigned long long)window.vertex_native_both,
		vc[3], vc[4], vc[5], vc[6]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_vertex_live] f=%llu "
		"tracked=%llu/%llu/%llu/%llu cache=%llu/%llu/%llu/%llu "
		"native=%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.vertex_live_state[0][0],
		(unsigned long long)window.vertex_live_state[0][1],
		(unsigned long long)window.vertex_live_state[0][2],
		(unsigned long long)window.vertex_live_state[0][3],
		(unsigned long long)window.vertex_live_state[1][0],
		(unsigned long long)window.vertex_live_state[1][1],
		(unsigned long long)window.vertex_live_state[1][2],
		(unsigned long long)window.vertex_live_state[1][3],
		(unsigned long long)window.vertex_live_state[2][0],
		(unsigned long long)window.vertex_live_state[2][1],
		(unsigned long long)window.vertex_live_state[2][2],
		(unsigned long long)window.vertex_live_state[2][3]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_vertex_coherence_summary] f=%llu "
		"y0=%llu/%llu/%llu/%llu/%llu/%llu "
		"y1=%llu/%llu/%llu/%llu/%llu/%llu "
		"y2=%llu/%llu/%llu/%llu/%llu/%llu "
		"y3=%llu/%llu/%llu/%llu/%llu/%llu "
		"cross-stage=%llu/%llu/%llu/%llu w-delta=%.3f/%.3f/%.3f/%.3f "
		"samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.vertex_coherence_delta[0][0],
		(unsigned long long)window.vertex_coherence_delta[0][1],
		(unsigned long long)window.vertex_coherence_delta[0][2],
		(unsigned long long)window.vertex_coherence_delta[0][3],
		(unsigned long long)window.vertex_coherence_delta[0][4],
		(unsigned long long)window.vertex_coherence_delta[0][5],
		(unsigned long long)window.vertex_coherence_delta[1][0],
		(unsigned long long)window.vertex_coherence_delta[1][1],
		(unsigned long long)window.vertex_coherence_delta[1][2],
		(unsigned long long)window.vertex_coherence_delta[1][3],
		(unsigned long long)window.vertex_coherence_delta[1][4],
		(unsigned long long)window.vertex_coherence_delta[1][5],
		(unsigned long long)window.vertex_coherence_delta[2][0],
		(unsigned long long)window.vertex_coherence_delta[2][1],
		(unsigned long long)window.vertex_coherence_delta[2][2],
		(unsigned long long)window.vertex_coherence_delta[2][3],
		(unsigned long long)window.vertex_coherence_delta[2][4],
		(unsigned long long)window.vertex_coherence_delta[2][5],
		(unsigned long long)window.vertex_coherence_delta[3][0],
		(unsigned long long)window.vertex_coherence_delta[3][1],
		(unsigned long long)window.vertex_coherence_delta[3][2],
		(unsigned long long)window.vertex_coherence_delta[3][3],
		(unsigned long long)window.vertex_coherence_delta[3][4],
		(unsigned long long)window.vertex_coherence_delta[3][5],
		(unsigned long long)window.vertex_coherence_cross_stage[0],
		(unsigned long long)window.vertex_coherence_cross_stage[1],
		(unsigned long long)window.vertex_coherence_cross_stage[2],
		(unsigned long long)window.vertex_coherence_cross_stage[3],
		window.vertex_coherence_w_delta[0],
		window.vertex_coherence_w_delta[1],
		window.vertex_coherence_w_delta[2],
		window.vertex_coherence_w_delta[3], vertex_coherence_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_line_hack] f=%llu candidates=%llu/%llu/%llu/%llu "
		"w-reject=%llu/%llu/%llu/%llu samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.line_hack_candidates[0],
		(unsigned long long)window.line_hack_candidates[1],
		(unsigned long long)window.line_hack_candidates[2],
		(unsigned long long)window.line_hack_candidates[3],
		(unsigned long long)window.line_hack_w_rejects[0],
		(unsigned long long)window.line_hack_w_rejects[1],
		(unsigned long long)window.line_hack_w_rejects[2],
		(unsigned long long)window.line_hack_w_rejects[3],
		line_hack_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_projection_z] f=%llu raw=nonpos:%llu floor:%llu normal:%llu "
		"far:%llu/%llu/%llu/%llu max=%.3f rendered-linked=%llu/%llu/%llu "
		"rendered-far=%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.projection_z_band[0],
		(unsigned long long)window.projection_z_band[1],
		(unsigned long long)window.projection_z_band[2],
		(unsigned long long)window.projection_z_band[3],
		(unsigned long long)window.projection_z_band[4],
		(unsigned long long)window.projection_z_band[5],
		(unsigned long long)window.projection_z_band[6],
		window.projection_z_max,
		(unsigned long long)window.rendered_z_linked[0],
		(unsigned long long)window.rendered_z_linked[1],
		(unsigned long long)window.rendered_z_linked[2],
		(unsigned long long)window.rendered_z_far[0],
		(unsigned long long)window.rendered_z_far[1],
		(unsigned long long)window.rendered_z_far[2]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_projection_delta] f=%llu applied=fractional-raw-z reference=architectural-sz3 "
		"generated=exact:%llu le.125:%llu le.25:%llu le.5:%llu lt1:%llu ge1:%llu "
		"sum=%.3f max=%.3f\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.projection_z_delta[0],
		(unsigned long long)window.projection_z_delta[1],
		(unsigned long long)window.projection_z_delta[2],
		(unsigned long long)window.projection_z_delta[3],
		(unsigned long long)window.projection_z_delta[4],
		(unsigned long long)window.projection_z_delta[5],
		window.projection_z_delta_sum,
		window.projection_z_delta_max);
	{
		unsigned source;
		for (source = 0; source < 3; source++)
		{
			uint64_t total = 0;
			unsigned c;
			for (c = 0; c < 6; c++)
				total += window.rendered_z_delta[source][c];
			if (total)
				log_cb(RETRO_LOG_INFO,
					"[pgxp_rendered_projection_delta] f=%llu source=%u total=%llu "
					"delta=%llu/%llu/%llu/%llu/%llu/%llu\n",
					(unsigned long long)frame_number, source,
					(unsigned long long)total,
					(unsigned long long)window.rendered_z_delta[source][0],
					(unsigned long long)window.rendered_z_delta[source][1],
					(unsigned long long)window.rendered_z_delta[source][2],
					(unsigned long long)window.rendered_z_delta[source][3],
					(unsigned long long)window.rendered_z_delta[source][4],
					(unsigned long long)window.rendered_z_delta[source][5]);
		}
	}
	log_cb(RETRO_LOG_INFO,
		"[pgxp_w_gate] f=%llu kept=%llu rejected=%llu "
		"stage=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"width=%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.vertex_w_gate_kept,
		(unsigned long long)window.vertex_w_gate_rejected,
		(unsigned long long)window.vertex_w_gate_rejected_stage[0],
		(unsigned long long)window.vertex_w_gate_rejected_stage[1],
		(unsigned long long)window.vertex_w_gate_rejected_stage[2],
		(unsigned long long)window.vertex_w_gate_rejected_stage[3],
		(unsigned long long)window.vertex_w_gate_rejected_stage[4],
		(unsigned long long)window.vertex_w_gate_rejected_stage[5],
		(unsigned long long)window.vertex_w_gate_rejected_stage[6],
		(unsigned long long)window.vertex_w_gate_rejected_stage[7],
		(unsigned long long)window.vertex_w_gate_rejected_stage[8],
		(unsigned long long)window.vertex_w_gate_rejected_width[0],
		(unsigned long long)window.vertex_w_gate_rejected_width[1],
		(unsigned long long)window.vertex_w_gate_rejected_width[2],
		(unsigned long long)window.vertex_w_gate_rejected_width[3]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_nclip_validity] f=%llu attempts=%llu invalid=%llu "
		"invalid_mask=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"mismatch_mask=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.nclip_validity_attempts,
		(unsigned long long)window.nclip_validity_invalid,
		(unsigned long long)window.nclip_invalid_mask[0],
		(unsigned long long)window.nclip_invalid_mask[1],
		(unsigned long long)window.nclip_invalid_mask[2],
		(unsigned long long)window.nclip_invalid_mask[3],
		(unsigned long long)window.nclip_invalid_mask[4],
		(unsigned long long)window.nclip_invalid_mask[5],
		(unsigned long long)window.nclip_invalid_mask[6],
		(unsigned long long)window.nclip_invalid_mask[7],
		(unsigned long long)window.nclip_mismatch_mask[0],
		(unsigned long long)window.nclip_mismatch_mask[1],
		(unsigned long long)window.nclip_mismatch_mask[2],
		(unsigned long long)window.nclip_mismatch_mask[3],
		(unsigned long long)window.nclip_mismatch_mask[4],
		(unsigned long long)window.nclip_mismatch_mask[5],
		(unsigned long long)window.nclip_mismatch_mask[6],
		(unsigned long long)window.nclip_mismatch_mask[7],
		nclip_invalid_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_nclip_xyz] f=%llu zmask=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"xy-only=%llu compares=%llu sign-change=%llu y=%llu/%llu/%llu/%llu "
		"samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.nclip_z_invalid_mask[0],
		(unsigned long long)window.nclip_z_invalid_mask[1],
		(unsigned long long)window.nclip_z_invalid_mask[2],
		(unsigned long long)window.nclip_z_invalid_mask[3],
		(unsigned long long)window.nclip_z_invalid_mask[4],
		(unsigned long long)window.nclip_z_invalid_mask[5],
		(unsigned long long)window.nclip_z_invalid_mask[6],
		(unsigned long long)window.nclip_z_invalid_mask[7],
		(unsigned long long)window.nclip_xy_only_attempts,
		(unsigned long long)window.nclip_xy_only_compares,
		(unsigned long long)window.nclip_xy_only_sign_changes,
		(unsigned long long)window.nclip_xy_only_y_band[0],
		(unsigned long long)window.nclip_xy_only_y_band[1],
		(unsigned long long)window.nclip_xy_only_y_band[2],
		(unsigned long long)window.nclip_xy_only_y_band[3],
		nclip_xy_only_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_nclip_reference_summary] f=%llu comparisons=%llu "
		"double_reference_disagree=%llu native_reference_disagree=%llu "
		"zero=%llu/%llu samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.nclip_compares,
		(unsigned long long)window.nclip_reference_sign_disagreements,
		(unsigned long long)window.nclip_reference_native_disagreements,
		(unsigned long long)window.nclip_double_zero,
		(unsigned long long)window.nclip_reference_zero,
		nclip_reference_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_gpu_area_summary] f=%llu tri=%llu/%llu/%llu "
		"native_sign=%llu/%llu/%llu precise_sign=%llu/%llu/%llu "
		"disagree=%llu precise_zero=%llu invalid_w=%llu "
		"near_native=%llu/%llu/%llu/%llu/%llu "
		"near_precise=%llu/%llu/%llu/%llu/%llu y=%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.gpu_triangles[0],
		(unsigned long long)window.gpu_triangles[1],
		(unsigned long long)window.gpu_triangles[2],
		(unsigned long long)window.gpu_area_native_sign[0],
		(unsigned long long)window.gpu_area_native_sign[1],
		(unsigned long long)window.gpu_area_native_sign[2],
		(unsigned long long)window.gpu_area_precise_sign[0],
		(unsigned long long)window.gpu_area_precise_sign[1],
		(unsigned long long)window.gpu_area_precise_sign[2],
		(unsigned long long)window.gpu_area_sign_disagreements,
		(unsigned long long)window.gpu_area_precise_zero,
		(unsigned long long)window.gpu_triangle_invalid_w,
		(unsigned long long)window.gpu_area_near_native[0],
		(unsigned long long)window.gpu_area_near_native[1],
		(unsigned long long)window.gpu_area_near_native[2],
		(unsigned long long)window.gpu_area_near_native[3],
		(unsigned long long)window.gpu_area_near_native[4],
		(unsigned long long)window.gpu_area_near_precise[0],
		(unsigned long long)window.gpu_area_near_precise[1],
		(unsigned long long)window.gpu_area_near_precise[2],
		(unsigned long long)window.gpu_area_near_precise[3],
		(unsigned long long)window.gpu_area_near_precise[4],
		(unsigned long long)window.gpu_area_anomaly_y[0],
		(unsigned long long)window.gpu_area_anomaly_y[1],
		(unsigned long long)window.gpu_area_anomaly_y[2],
		(unsigned long long)window.gpu_area_anomaly_y[3]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_gpu_quad_summary] f=%llu pairs=%llu native_fold=%llu "
		"precise_fold=%llu introduced=%llu removed=%llu "
		"oversize=%llu/%llu oversize_disagree=%llu samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.gpu_quad_pairs,
		(unsigned long long)window.gpu_quad_native_fold,
		(unsigned long long)window.gpu_quad_precise_fold,
		(unsigned long long)window.gpu_quad_fold_introduced,
		(unsigned long long)window.gpu_quad_fold_removed,
		(unsigned long long)window.gpu_oversize_x,
		(unsigned long long)window.gpu_oversize_y,
		(unsigned long long)window.gpu_oversize_sign_disagreements,
		gpu_area_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_gpu_fold_summary] f=%llu introduced_y=%llu/%llu/%llu/%llu "
		"opcode=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/"
		"%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/"
		"%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/"
		"%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.gpu_quad_fold_introduced_y[0],
		(unsigned long long)window.gpu_quad_fold_introduced_y[1],
		(unsigned long long)window.gpu_quad_fold_introduced_y[2],
		(unsigned long long)window.gpu_quad_fold_introduced_y[3],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[0],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[1],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[2],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[3],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[4],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[5],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[6],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[7],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[8],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[9],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[10],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[11],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[12],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[13],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[14],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[15],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[16],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[17],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[18],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[19],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[20],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[21],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[22],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[23],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[24],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[25],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[26],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[27],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[28],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[29],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[30],
		(unsigned long long)window.gpu_quad_fold_introduced_opcode[31],
		gpu_fold_samples);
	{
		unsigned source;
		for (source = 0; source < 3; source++)
		{
			uint64_t total = 0;
			unsigned c;
			for (c = 0; c < 6; c++)
				total += window.vertex_delta_class[source][c];
			if (!total)
				continue;
			log_cb(RETRO_LOG_INFO,
				"[pgxp_precision] f=%llu source=%u total=%llu "
				"delta=%llu/%llu/%llu/%llu/%llu/%llu "
				"axis_exact=%llu/%llu subpixel=%llu/%llu ge1=%llu/%llu "
				"sign_x=%llu/%llu/%llu sign_y=%llu/%llu/%llu "
				"mean_abs=%.6f/%.6f bias=%.6f/%.6f max=%.3f/%.3f\n",
				(unsigned long long)frame_number, source,
				(unsigned long long)total,
				(unsigned long long)window.vertex_delta_class[source][0],
				(unsigned long long)window.vertex_delta_class[source][1],
				(unsigned long long)window.vertex_delta_class[source][2],
				(unsigned long long)window.vertex_delta_class[source][3],
				(unsigned long long)window.vertex_delta_class[source][4],
				(unsigned long long)window.vertex_delta_class[source][5],
				(unsigned long long)window.vertex_axis_exact[source][0],
				(unsigned long long)window.vertex_axis_exact[source][1],
				(unsigned long long)window.vertex_axis_subpixel[source][0],
				(unsigned long long)window.vertex_axis_subpixel[source][1],
				(unsigned long long)window.vertex_axis_ge_one[source][0],
				(unsigned long long)window.vertex_axis_ge_one[source][1],
				(unsigned long long)window.vertex_axis_sign[source][0][0],
				(unsigned long long)window.vertex_axis_sign[source][0][1],
				(unsigned long long)window.vertex_axis_sign[source][0][2],
				(unsigned long long)window.vertex_axis_sign[source][1][0],
				(unsigned long long)window.vertex_axis_sign[source][1][1],
				(unsigned long long)window.vertex_axis_sign[source][1][2],
				window.vertex_delta_abs_sum[source][0] / (double)total,
				window.vertex_delta_abs_sum[source][1] / (double)total,
				window.vertex_delta_sum[source][0] / (double)total,
				window.vertex_delta_sum[source][1] / (double)total,
				window.vertex_delta_max[source][0],
				window.vertex_delta_max[source][1]);
		}
	}
	{
		unsigned stage;
		for (stage = 0; stage < PGXP_DIAG_TRACE_STAGES; stage++)
		{
			uint64_t total = 0;
			unsigned c;
			for (c = 0; c < 6; c++) total += window.vertex_stage_delta[stage][c];
			if (total)
				log_cb(RETRO_LOG_INFO,
					"[pgxp_precision_stage] f=%llu stage=%u total=%llu "
					"delta=%llu/%llu/%llu/%llu/%llu/%llu\n",
					(unsigned long long)frame_number, stage,
					(unsigned long long)total,
					(unsigned long long)window.vertex_stage_delta[stage][0],
					(unsigned long long)window.vertex_stage_delta[stage][1],
					(unsigned long long)window.vertex_stage_delta[stage][2],
					(unsigned long long)window.vertex_stage_delta[stage][3],
					(unsigned long long)window.vertex_stage_delta[stage][4],
					(unsigned long long)window.vertex_stage_delta[stage][5]);
		}
	}
	{
		unsigned width;
		for (width = 0; width < PGXP_DIAG_WRITER_WIDTHS; width++)
		{
			uint64_t total = 0;
			unsigned c;
			for (c = 0; c < 6; c++) total += window.vertex_writer_delta[width][c];
			if (total)
				log_cb(RETRO_LOG_INFO,
					"[pgxp_precision_writer] f=%llu width=%u total=%llu "
					"delta=%llu/%llu/%llu/%llu/%llu/%llu\n",
					(unsigned long long)frame_number, width,
					(unsigned long long)total,
					(unsigned long long)window.vertex_writer_delta[width][0],
					(unsigned long long)window.vertex_writer_delta[width][1],
					(unsigned long long)window.vertex_writer_delta[width][2],
					(unsigned long long)window.vertex_writer_delta[width][3],
					(unsigned long long)window.vertex_writer_delta[width][4],
					(unsigned long long)window.vertex_writer_delta[width][5]);
		}
	}
	{
		unsigned source;
		for (source = 0; source < 3; source++)
			if (window.rendered_z_far[source])
				log_cb(RETRO_LOG_INFO,
					"[pgxp_rendered_z_far] f=%llu source=%u "
					"band=%llu/%llu/%llu/%llu\n",
					(unsigned long long)frame_number, source,
					(unsigned long long)window.rendered_z_band[source][0],
					(unsigned long long)window.rendered_z_band[source][1],
					(unsigned long long)window.rendered_z_band[source][2],
					(unsigned long long)window.rendered_z_band[source][3]);
	}
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
		"[pgxp_trace_ledger] f=%llu gte=%llu mfc2=%llu sll=%llu "
		"sra=%llu cpu=%llu store=%llu load=%llu fifo=%llu cb=%llu vertex=%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_GTE],
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_MFC2],
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_SLL5],
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_SRA5],
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_CPU],
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_STORE],
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_LOAD],
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_FIFO],
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_CB],
		(unsigned long long)window.trace_events[PGXP_TRACE_EVENT_VERTEX]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_trace_reason] f=%llu ok/no-id/invalid/mismatch/lineage/other/cache/reserved="
		"%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.trace_reasons[0],
		(unsigned long long)window.trace_reasons[1],
		(unsigned long long)window.trace_reasons[2],
		(unsigned long long)window.trace_reasons[3],
		(unsigned long long)window.trace_reasons[4],
		(unsigned long long)window.trace_reasons[5],
		(unsigned long long)window.trace_reasons[6],
		(unsigned long long)window.trace_reasons[7]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_trace_terminal] f=%llu tracked=%llu cache=%llu native="
		"%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)window.trace_terminal[0][0],
		(unsigned long long)window.trace_terminal[1][6],
		(unsigned long long)window.trace_terminal[2][0],
		(unsigned long long)window.trace_terminal[2][1],
		(unsigned long long)window.trace_terminal[2][2],
		(unsigned long long)window.trace_terminal[2][3],
		(unsigned long long)window.trace_terminal[2][4],
		(unsigned long long)window.trace_terminal[2][5],
		(unsigned long long)window.trace_terminal[2][6],
		(unsigned long long)window.trace_terminal[2][7]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_primitive_summary] f=%llu total=%llu "
		"class_u=%llu/%llu/%llu/%llu class_t=%llu/%llu/%llu/%llu "
		"y=%llu/%llu/%llu/%llu sra_vertices=%llu tol_reverts=%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)primitive_total,
		(unsigned long long)primitive_class[0][0][0],
		(unsigned long long)primitive_class[0][0][1],
		(unsigned long long)primitive_class[0][1][0],
		(unsigned long long)primitive_class[0][1][1],
		(unsigned long long)primitive_class[1][0][0],
		(unsigned long long)primitive_class[1][0][1],
		(unsigned long long)primitive_class[1][1][0],
		(unsigned long long)primitive_class[1][1][1],
		(unsigned long long)primitive_y_band[0],
		(unsigned long long)primitive_y_band[1],
		(unsigned long long)primitive_y_band[2],
		(unsigned long long)primitive_y_band[3],
		(unsigned long long)primitive_sra_vertices,
		(unsigned long long)primitive_tolerance_reverts);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_edge_summary] f=%llu observed=%llu/%llu "
		"compared_uu_ut_tt=%llu/%llu/%llu overflow=%llu samples=%u "
		"handoff=gl_decoded_triangle_edge_weld\n",
		(unsigned long long)frame_number,
		(unsigned long long)edge_observations[0],
		(unsigned long long)edge_observations[1],
		(unsigned long long)edge_compares[0],
		(unsigned long long)edge_compares[1],
		(unsigned long long)edge_compares[2],
		(unsigned long long)edge_table_overflow, edge_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_seam_summary] f=%llu primitives=%llu edges=%llu "
		"exact=%llu accepted=%llu "
		"stale=%llu unanchored=%llu far=%llu moved=%llu conflicts=%llu "
		"delta=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"age=%llu/%llu/%llu/%llu/%llu/%llu samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)seam_primitives,
		(unsigned long long)seam_observed_edges,
		(unsigned long long)seam_results[PGXP_DIAG_SEAM_EXACT],
		(unsigned long long)seam_results[PGXP_DIAG_SEAM_ACCEPTED],
		(unsigned long long)seam_results[PGXP_DIAG_SEAM_STALE],
		(unsigned long long)seam_results[PGXP_DIAG_SEAM_UNANCHORED],
		(unsigned long long)seam_results[PGXP_DIAG_SEAM_FAR],
		(unsigned long long)seam_moved_vertices,
		(unsigned long long)seam_conflicts,
		(unsigned long long)seam_delta_bins[0],
		(unsigned long long)seam_delta_bins[1],
		(unsigned long long)seam_delta_bins[2],
		(unsigned long long)seam_delta_bins[3],
		(unsigned long long)seam_delta_bins[4],
		(unsigned long long)seam_delta_bins[5],
		(unsigned long long)seam_delta_bins[6],
		(unsigned long long)seam_age_bins[0],
		(unsigned long long)seam_age_bins[1],
		(unsigned long long)seam_age_bins[2],
		(unsigned long long)seam_age_bins[3],
		(unsigned long long)seam_age_bins[4],
		(unsigned long long)seam_age_bins[5],
		seam_accept_samples);
	{
		static const char* const edge_kind_name[PGXP_DIAG_EDGE_KINDS] = {
			"uu", "ut", "tt"
		};
		unsigned kind;
		for (kind = 0; kind < PGXP_DIAG_EDGE_KINDS; kind++)
		{
			double mean = edge_compares[kind] ?
				edge_delta_sum[kind] / (double)edge_compares[kind] : 0.0;
			log_cb(RETRO_LOG_INFO,
				"[pgxp_edge_kind] f=%llu kind=%s "
				"delta=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"near_delta=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"packet=%llu/%llu/%llu/%llu/%llu/%llu "
				"mismatch_y=%llu/%llu/%llu/%llu mean=%.6f max=%.6f\n",
				(unsigned long long)frame_number, edge_kind_name[kind],
				(unsigned long long)edge_delta_bins[kind][0],
				(unsigned long long)edge_delta_bins[kind][1],
				(unsigned long long)edge_delta_bins[kind][2],
				(unsigned long long)edge_delta_bins[kind][3],
				(unsigned long long)edge_delta_bins[kind][4],
				(unsigned long long)edge_delta_bins[kind][5],
				(unsigned long long)edge_delta_bins[kind][6],
				(unsigned long long)edge_near_delta_bins[kind][0],
				(unsigned long long)edge_near_delta_bins[kind][1],
				(unsigned long long)edge_near_delta_bins[kind][2],
				(unsigned long long)edge_near_delta_bins[kind][3],
				(unsigned long long)edge_near_delta_bins[kind][4],
				(unsigned long long)edge_near_delta_bins[kind][5],
				(unsigned long long)edge_near_delta_bins[kind][6],
				(unsigned long long)edge_packet_bins[kind][0],
				(unsigned long long)edge_packet_bins[kind][1],
				(unsigned long long)edge_packet_bins[kind][2],
				(unsigned long long)edge_packet_bins[kind][3],
				(unsigned long long)edge_packet_bins[kind][4],
				(unsigned long long)edge_packet_bins[kind][5],
				(unsigned long long)edge_mismatch_y[kind][0],
				(unsigned long long)edge_mismatch_y[kind][1],
				(unsigned long long)edge_mismatch_y[kind][2],
				(unsigned long long)edge_mismatch_y[kind][3],
				mean, edge_delta_max[kind]);
		}
	}
	log_cb(RETRO_LOG_INFO,
		"[pgxp_recovery_summary] f=%llu attempts=%llu hits=%llu "
		"age=%llu/%llu/%llu/%llu/%llu ambiguous=%llu used=%llu "
		"misses=%llu "
		"too_old=%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)recovery_attempts,
		(unsigned long long)recovery_hits,
		(unsigned long long)recovery_age_hits[0],
		(unsigned long long)recovery_age_hits[1],
		(unsigned long long)recovery_age_hits[2],
		(unsigned long long)recovery_age_hits[3],
		(unsigned long long)recovery_age_hits[4],
		(unsigned long long)recovery_ambiguous,
		(unsigned long long)recovery_ambiguous_used,
		(unsigned long long)recovery_misses,
		(unsigned long long)recovery_too_old);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_recovery_old_age] f=%llu age5=%llu age6=%llu age7=%llu "
		"age8plus=%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)recovery_old_age[0],
		(unsigned long long)recovery_old_age[1],
		(unsigned long long)recovery_old_age[2],
		(unsigned long long)recovery_old_age[3]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_recovery_stage] f=%llu attempts=%llu/%llu/%llu/%llu/"
		"%llu/%llu/%llu/%llu/%llu hits=%llu/%llu/%llu/%llu/%llu/"
		"%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)recovery_stage_attempts[0],
		(unsigned long long)recovery_stage_attempts[1],
		(unsigned long long)recovery_stage_attempts[2],
		(unsigned long long)recovery_stage_attempts[3],
		(unsigned long long)recovery_stage_attempts[4],
		(unsigned long long)recovery_stage_attempts[5],
		(unsigned long long)recovery_stage_attempts[6],
		(unsigned long long)recovery_stage_attempts[7],
		(unsigned long long)recovery_stage_attempts[8],
		(unsigned long long)recovery_stage_hits[0],
		(unsigned long long)recovery_stage_hits[1],
		(unsigned long long)recovery_stage_hits[2],
		(unsigned long long)recovery_stage_hits[3],
		(unsigned long long)recovery_stage_hits[4],
		(unsigned long long)recovery_stage_hits[5],
		(unsigned long long)recovery_stage_hits[6],
		(unsigned long long)recovery_stage_hits[7],
		(unsigned long long)recovery_stage_hits[8]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_recovery_hash] f=%llu way=%llu/%llu/%llu/%llu "
		"evictions=%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)recovery_way_hits[0],
		(unsigned long long)recovery_way_hits[1],
		(unsigned long long)recovery_way_hits[2],
		(unsigned long long)recovery_way_hits[3],
		(unsigned long long)recovery_evictions);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_writer_summary] f=%llu writes=%llu/%llu/%llu/%llu "
		"native=%llu/%llu/%llu/%llu tracked_invalid_w=%llu samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)writer_writes[0],
		(unsigned long long)writer_writes[1],
		(unsigned long long)writer_writes[2],
		(unsigned long long)writer_writes[3],
		(unsigned long long)writer_native[0],
		(unsigned long long)writer_native[1],
		(unsigned long long)writer_native[2],
		(unsigned long long)writer_native[3],
		(unsigned long long)writer_tracked_invalid_w, writer_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_writer_w] f=%llu tracked=%llu/%llu/%llu/%llu "
		"source_w=%llu/%llu/%llu/%llu retained_w=%llu/%llu/%llu/%llu "
		"samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)writer_tracked[0],
		(unsigned long long)writer_tracked[1],
		(unsigned long long)writer_tracked[2],
		(unsigned long long)writer_tracked[3],
		(unsigned long long)writer_tracked_source_w[0],
		(unsigned long long)writer_tracked_source_w[1],
		(unsigned long long)writer_tracked_source_w[2],
		(unsigned long long)writer_tracked_source_w[3],
		(unsigned long long)writer_tracked_retained_w[0],
		(unsigned long long)writer_tracked_retained_w[1],
		(unsigned long long)writer_tracked_retained_w[2],
		(unsigned long long)writer_tracked_retained_w[3],
		writer_tracked_samples);
	{
		unsigned width;
		for (width = 0; width < PGXP_DIAG_WRITER_WIDTHS; width++)
			log_cb(RETRO_LOG_INFO,
				"[pgxp_writer_detail] f=%llu width=%u "
				"reason=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"stage=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
				(unsigned long long)frame_number, width,
				(unsigned long long)writer_native_reason[width][0],
				(unsigned long long)writer_native_reason[width][1],
				(unsigned long long)writer_native_reason[width][2],
				(unsigned long long)writer_native_reason[width][3],
				(unsigned long long)writer_native_reason[width][4],
				(unsigned long long)writer_native_reason[width][5],
				(unsigned long long)writer_native_reason[width][6],
				(unsigned long long)writer_native_reason[width][7],
				(unsigned long long)writer_native_stage[width][0],
				(unsigned long long)writer_native_stage[width][1],
				(unsigned long long)writer_native_stage[width][2],
				(unsigned long long)writer_native_stage[width][3],
				(unsigned long long)writer_native_stage[width][4],
				(unsigned long long)writer_native_stage[width][5],
				(unsigned long long)writer_native_stage[width][6],
				(unsigned long long)writer_native_stage[width][7],
				(unsigned long long)writer_native_stage[width][8]);
	}
	{
		unsigned bucket;
		for (bucket = 0; bucket < PGXP_DIAG_PRIMITIVE_BUCKETS; bucket++)
		{
			uint64_t bucket_total = primitive_composition[bucket][0] +
				primitive_composition[bucket][1] +
				primitive_composition[bucket][2] +
				primitive_composition[bucket][3];
			if (!bucket_total)
				continue;
			log_cb(RETRO_LOG_INFO,
				"[pgxp_primitive_provenance] f=%llu bucket=%u total=%llu "
				"composition=%llu/%llu/%llu/%llu "
				"tracked_stage=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"cache_stage=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"native_stage=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"native_reason=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"native_sra5_reason=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
				(unsigned long long)frame_number, bucket,
				(unsigned long long)bucket_total,
				(unsigned long long)primitive_composition[bucket][0],
				(unsigned long long)primitive_composition[bucket][1],
				(unsigned long long)primitive_composition[bucket][2],
				(unsigned long long)primitive_composition[bucket][3],
				(unsigned long long)primitive_source_stage[bucket][0][0],
				(unsigned long long)primitive_source_stage[bucket][0][1],
				(unsigned long long)primitive_source_stage[bucket][0][2],
				(unsigned long long)primitive_source_stage[bucket][0][3],
				(unsigned long long)primitive_source_stage[bucket][0][4],
				(unsigned long long)primitive_source_stage[bucket][0][5],
				(unsigned long long)primitive_source_stage[bucket][0][6],
				(unsigned long long)primitive_source_stage[bucket][0][7],
				(unsigned long long)primitive_source_stage[bucket][0][8],
				(unsigned long long)primitive_source_stage[bucket][1][0],
				(unsigned long long)primitive_source_stage[bucket][1][1],
				(unsigned long long)primitive_source_stage[bucket][1][2],
				(unsigned long long)primitive_source_stage[bucket][1][3],
				(unsigned long long)primitive_source_stage[bucket][1][4],
				(unsigned long long)primitive_source_stage[bucket][1][5],
				(unsigned long long)primitive_source_stage[bucket][1][6],
				(unsigned long long)primitive_source_stage[bucket][1][7],
				(unsigned long long)primitive_source_stage[bucket][1][8],
				(unsigned long long)primitive_source_stage[bucket][2][0],
				(unsigned long long)primitive_source_stage[bucket][2][1],
				(unsigned long long)primitive_source_stage[bucket][2][2],
				(unsigned long long)primitive_source_stage[bucket][2][3],
				(unsigned long long)primitive_source_stage[bucket][2][4],
				(unsigned long long)primitive_source_stage[bucket][2][5],
				(unsigned long long)primitive_source_stage[bucket][2][6],
				(unsigned long long)primitive_source_stage[bucket][2][7],
				(unsigned long long)primitive_source_stage[bucket][2][8],
				(unsigned long long)primitive_native_reason[bucket][0],
				(unsigned long long)primitive_native_reason[bucket][1],
				(unsigned long long)primitive_native_reason[bucket][2],
				(unsigned long long)primitive_native_reason[bucket][3],
				(unsigned long long)primitive_native_reason[bucket][4],
				(unsigned long long)primitive_native_reason[bucket][5],
				(unsigned long long)primitive_native_reason[bucket][6],
				(unsigned long long)primitive_native_reason[bucket][7],
				(unsigned long long)primitive_native_sra5_reason[bucket][0],
				(unsigned long long)primitive_native_sra5_reason[bucket][1],
				(unsigned long long)primitive_native_sra5_reason[bucket][2],
				(unsigned long long)primitive_native_sra5_reason[bucket][3],
				(unsigned long long)primitive_native_sra5_reason[bucket][4],
				(unsigned long long)primitive_native_sra5_reason[bucket][5],
				(unsigned long long)primitive_native_sra5_reason[bucket][6],
				(unsigned long long)primitive_native_sra5_reason[bucket][7]);
		}
	}
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
	gpu_area_window_samples = 0;
	gpu_fold_window_samples = 0;
	nclip_invalid_window_samples = 0;
	nclip_reference_window_samples = 0;
	nclip_xy_only_window_samples = 0;
	primitive_total = 0;
	memset(primitive_class, 0, sizeof(primitive_class));
	memset(primitive_y_band, 0, sizeof(primitive_y_band));
	primitive_sra_vertices = 0;
	primitive_tolerance_reverts = 0;
	memset(primitive_composition, 0, sizeof(primitive_composition));
	memset(primitive_source_stage, 0, sizeof(primitive_source_stage));
	memset(primitive_native_reason, 0, sizeof(primitive_native_reason));
	memset(primitive_native_sra5_reason, 0,
		sizeof(primitive_native_sra5_reason));
	memset(edge_observations, 0, sizeof(edge_observations));
	memset(edge_compares, 0, sizeof(edge_compares));
	memset(edge_delta_bins, 0, sizeof(edge_delta_bins));
	memset(edge_near_delta_bins, 0, sizeof(edge_near_delta_bins));
	memset(edge_packet_bins, 0, sizeof(edge_packet_bins));
	memset(edge_mismatch_y, 0, sizeof(edge_mismatch_y));
	memset(edge_delta_sum, 0, sizeof(edge_delta_sum));
	memset(edge_delta_max, 0, sizeof(edge_delta_max));
	edge_table_overflow = 0;
	memset(seam_results, 0, sizeof(seam_results));
	memset(seam_delta_bins, 0, sizeof(seam_delta_bins));
	memset(seam_age_bins, 0, sizeof(seam_age_bins));
	seam_primitives = 0;
	seam_observed_edges = 0;
	seam_moved_vertices = 0;
	seam_conflicts = 0;
	seam_accept_window_samples = 0;
	recovery_attempts = recovery_hits = recovery_ambiguous = recovery_misses = 0;
	recovery_ambiguous_used = 0;
	memset(recovery_age_hits, 0, sizeof(recovery_age_hits));
	memset(recovery_old_age, 0, sizeof(recovery_old_age));
	recovery_too_old = 0;
	memset(recovery_stage_attempts, 0, sizeof(recovery_stage_attempts));
	memset(recovery_stage_hits, 0, sizeof(recovery_stage_hits));
	memset(recovery_way_hits, 0, sizeof(recovery_way_hits));
	recovery_evictions = 0;
	memset(writer_writes, 0, sizeof(writer_writes));
	memset(writer_native, 0, sizeof(writer_native));
	memset(writer_native_reason, 0, sizeof(writer_native_reason));
	memset(writer_native_stage, 0, sizeof(writer_native_stage));
	memset(writer_tracked, 0, sizeof(writer_tracked));
	memset(writer_tracked_source_w, 0, sizeof(writer_tracked_source_w));
	memset(writer_tracked_retained_w, 0, sizeof(writer_tracked_retained_w));
	writer_tracked_invalid_w = 0;
	writer_samples = 0;
	writer_tracked_samples = 0;
	dispatch_samples = 0;
}

#endif /* PGXP_DIAG */

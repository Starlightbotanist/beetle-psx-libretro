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
#define PGXP_DIAG_TJ_VERTEX_SLOTS 16384u
#define PGXP_DIAG_TJ_VERTEX_WAYS 4u
#define PGXP_DIAG_TJ_EDGE_CAPACITY 16384u
#define PGXP_DIAG_TJ_MAX_STEPS 256u
#define PGXP_DIAG_TJ_TOPOLOGIES 3u
#define PGXP_DIAG_TJ_SAMPLES 2048u
#define PGXP_DIAG_TJ_WINDOW_SAMPLES 8u
#define PGXP_DIAG_SUBMIT_TRIANGLES 32768u
#define PGXP_DIAG_SUBMIT_HASH_BUCKETS 65536u
#define PGXP_DIAG_SUBMIT_MODELS 5u
#define PGXP_DIAG_SUBMIT_SAMPLES 4096u
#define PGXP_DIAG_SUBMIT_WINDOW_SAMPLES 16u
#define PGXP_DIAG_SUBMIT_MAX_BBOX_PIXELS 65536u
#define PGXP_DIAG_SUBMIT_LINK_KINDS 3u
#define PGXP_DIAG_SUBMIT_W_BINS 7u
#define PGXP_DIAG_SUBMIT_UV_BINS 6u
#define PGXP_DIAG_GL_VERTEX_CAPACITY 16384u
#define PGXP_DIAG_GL_HASH_BUCKETS 32768u
#define PGXP_DIAG_GL_PAIR_CAPACITY 65536u
#define PGXP_DIAG_GL_WINDOW_SAMPLES 16u
#define PGXP_DIAG_GL_NATIVE_SAMPLES 4096u
#define PGXP_DIAG_GL_NATIVE_WINDOW_SAMPLES 16u

/* The broad projection experiment in 3b1b8013 proved that native screen
 * coincidence is not sufficient provenance for mutating live geometry.  Keep
 * the complete buffered classifier active, but make production diagnostic
 * builds observation-only until the stricter endpoint/packet/coverage gates
 * below have device evidence.  The focused harness opts in explicitly. */
#ifndef PGXP_DIAG_GL_REPAIR_APPLY
#define PGXP_DIAG_GL_REPAIR_APPLY 0
#endif

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

/* Frame-complete T-junction census.  A candidate is a native PSX vertex
 * strictly inside another decoded edge.  Keeping triangle neighbours lets us
 * distinguish an actual partitioned boundary from an unrelated vertex that
 * happens to reuse the same screen coordinate. */
typedef struct
{
	float precise_x;
	float precise_y;
	float precise_w;
	int32_t neighbor_x[2];
	int32_t neighbor_y[2];
	uint64_t packet;
	uint16_t u;
	uint16_t v;
	uint8_t opcode;
	uint8_t textured;
	uint8_t gouraud;
	uint8_t invalid_w;
	uint8_t stage;
	uint8_t valid;
} PGXP_diag_tj_observation;

typedef struct
{
	int32_t native_x;
	int32_t native_y;
	PGXP_diag_tj_observation observation[PGXP_DIAG_TJ_VERTEX_WAYS];
	uint8_t valid;
} PGXP_diag_tj_vertex;

typedef struct
{
	int32_t native_x[2];
	int32_t native_y[2];
	int32_t third_x;
	int32_t third_y;
	float precise_x[2];
	float precise_y[2];
	float precise_w[2];
	uint64_t packet;
	uint16_t u[2];
	uint16_t v[2];
	uint8_t opcode;
	uint8_t textured;
	uint8_t gouraud;
	uint8_t invalid_w;
	uint8_t stage[2];
} PGXP_diag_tj_edge;

/* Lossless frame-local copy of the triangles handed to OpenGL.  Native X/Y
 * is intentionally kept in PS1 pixels while precise X/Y remains in the
 * actual upscaled RHI coordinate system.  That makes native topology exact
 * and lets the coverage model reproduce the active internal resolution
 * without round-tripping the float input. */
typedef struct
{
	int32_t native_x[3];
	int32_t native_y[3];
	float precise_x[3];
	float precise_y[3];
	float precise_w[3];
	uint64_t packet;
	uint16_t u[3];
	uint16_t v[3];
	uint8_t opcode;
	uint8_t upscale_shift;
	uint8_t textured;
	uint8_t gouraud;
	uint8_t invalid_w;
	uint8_t semi_transparent;
} PGXP_diag_submit_triangle;

typedef struct
{
	uint32_t triangle;
	uint32_t next;
	uint8_t vertex;
} PGXP_diag_submit_node;

/* Sidecar for the currently mapped OpenGL command buffer.  Keeping native
 * topology outside gl_command_vertex avoids changing the release renderer's
 * vertex ABI while still letting the final buffered stream be repaired with
 * full packet provenance. */
typedef struct
{
	int32_t native_x;
	int32_t native_y;
	uint64_t packet;
	uint64_t material_key;
	uint32_t triangle_start;
	uint16_t u;
	uint16_t v;
	uint8_t opcode;
	uint8_t vertex;
	uint8_t upscale_shift;
	uint8_t textured;
	uint8_t gouraud;
	uint8_t invalid_w;
	uint8_t semi_transparent;
	uint8_t valid;
} PGXP_diag_gl_vertex;

typedef struct
{
	float x;
	float y;
	uint16_t count;
	uint8_t conflict;
	uint8_t apply;
} PGXP_diag_gl_proposal;

typedef struct
{
	uint32_t point;
	uint32_t linked;
} PGXP_diag_gl_pair;

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
static PGXP_diag_tj_vertex tj_vertex_table[PGXP_DIAG_TJ_VERTEX_SLOTS];
static PGXP_diag_tj_edge tj_edges[PGXP_DIAG_TJ_EDGE_CAPACITY];
static uint32_t tj_table_frame = ~UINT32_C(0);
static uint32_t tj_edge_count;
static uint64_t tj_edges_recorded;
static uint64_t tj_lattice_edges;
static uint64_t tj_long_edges;
static uint64_t tj_interior_points;
static uint64_t tj_matches[PGXP_DIAG_EDGE_KINDS];
static uint64_t tj_topology[PGXP_DIAG_EDGE_KINDS]
	[PGXP_DIAG_TJ_TOPOLOGIES];
static uint64_t tj_perp_bins[PGXP_DIAG_EDGE_KINDS]
	[PGXP_DIAG_EDGE_DELTA_BINS];
static uint64_t tj_predicted_bins[PGXP_DIAG_EDGE_KINDS]
	[PGXP_DIAG_EDGE_DELTA_BINS];
static uint64_t tj_packet_bins[PGXP_DIAG_EDGE_KINDS]
	[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t tj_y_band[PGXP_DIAG_EDGE_KINDS][4];
static uint64_t tj_step_bins[PGXP_DIAG_EDGE_KINDS][7];
static uint64_t tj_risk[PGXP_DIAG_EDGE_KINDS];
static uint64_t tj_offset_side[PGXP_DIAG_EDGE_KINDS][3];
static uint64_t tj_risk_invalid_w[4];
static uint64_t tj_risk_gouraud[4];
static uint64_t tj_risk_packet[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t tj_risk_y[4];
static uint64_t tj_context_invalid_w[4];
static uint64_t tj_context_gouraud[4];
static uint64_t tj_context_semi[4];
static uint64_t tj_projected_outside;
static uint64_t tj_degenerate_precise_edge;
static uint64_t tj_vertex_overflow;
static uint64_t tj_edge_overflow;
static uint64_t tj_observation_evictions;
static uint32_t tj_samples;
static uint32_t tj_window_samples;
static PGXP_diag_submit_triangle submit_triangles[PGXP_DIAG_SUBMIT_TRIANGLES];
static PGXP_diag_submit_node submit_nodes[PGXP_DIAG_SUBMIT_TRIANGLES * 3u];
static uint32_t submit_heads[PGXP_DIAG_SUBMIT_HASH_BUCKETS];
static uint32_t submit_table_frame = ~UINT32_C(0);
static uint32_t submit_triangle_count;
static uint32_t submit_pending_triangle;
static uint8_t submit_pending_vertices;
static uint8_t submit_pending_valid;
static unsigned submit_gl_subpixel_bits;
static uint64_t submit_primitives;
static uint64_t submit_quads;
static uint64_t submit_triangle_overflow;
static uint64_t submit_transport_calls;
static uint64_t submit_transport_vertices;
static uint64_t submit_transport_mismatch_calls;
static uint64_t submit_transport_mismatch_vertices;
static uint64_t submit_transport_orphans;
static uint64_t submit_edges;
static uint64_t submit_lattice_edges;
static uint64_t submit_long_edges;
static uint64_t submit_interior_points;
static uint64_t submit_matches;
static uint64_t submit_topology[PGXP_DIAG_TJ_TOPOLOGIES];
static uint64_t submit_risk;
static uint64_t submit_risk_y[4];
static uint64_t submit_risk_packet[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t submit_risk_gouraud[4];
static uint64_t submit_risk_opcode[32][32];
static uint64_t submit_raster_pairs;
static uint64_t submit_raster_native_pixels;
static uint64_t submit_raster_hole_pairs[PGXP_DIAG_SUBMIT_MODELS];
static uint64_t submit_raster_hole_pixels[PGXP_DIAG_SUBMIT_MODELS];
static uint64_t submit_raster_snap_improved;
static uint64_t submit_raster_snap_closed;
static uint64_t submit_raster_bbox_pixels;
static uint64_t submit_raster_bbox_skips;
static uint64_t submit_raster_degenerate;
static uint64_t submit_link_candidates[PGXP_DIAG_SUBMIT_LINK_KINDS]
	[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t submit_link_tested[PGXP_DIAG_SUBMIT_LINK_KINDS]
	[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t submit_link_improved[PGXP_DIAG_SUBMIT_LINK_KINDS]
	[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t submit_link_closed[PGXP_DIAG_SUBMIT_LINK_KINDS]
	[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t submit_link_worse[PGXP_DIAG_SUBMIT_LINK_KINDS]
	[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t submit_link_raw_pixels[PGXP_DIAG_SUBMIT_LINK_KINDS]
	[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t submit_link_snap_pixels[PGXP_DIAG_SUBMIT_LINK_KINDS]
	[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t submit_link_w_error[PGXP_DIAG_SUBMIT_LINK_KINDS]
	[PGXP_DIAG_SUBMIT_W_BINS];
static uint64_t submit_link_uv_error[PGXP_DIAG_SUBMIT_LINK_KINDS]
	[PGXP_DIAG_SUBMIT_UV_BINS];
static uint32_t submit_samples;
static uint32_t submit_window_samples;
static PGXP_diag_gl_vertex gl_vertices[PGXP_DIAG_GL_VERTEX_CAPACITY];
static PGXP_diag_gl_proposal gl_proposals[PGXP_DIAG_GL_VERTEX_CAPACITY];
static PGXP_diag_gl_pair gl_pairs[PGXP_DIAG_GL_PAIR_CAPACITY];
static uint32_t gl_hash_heads[PGXP_DIAG_GL_HASH_BUCKETS];
static uint32_t gl_hash_next[PGXP_DIAG_GL_VERTEX_CAPACITY];
static uint32_t gl_vertex_count;
static uint8_t gl_vertex_overflow;
static uint64_t gl_repair_buffers;
static uint64_t gl_repair_vertices;
static uint64_t gl_repair_metadata_mismatch;
static uint64_t gl_repair_metadata_overflow;
static uint64_t gl_repair_inactive;
static uint64_t gl_repair_triangles;
static uint64_t gl_repair_edges;
static uint64_t gl_repair_lattice_edges;
static uint64_t gl_repair_long_edges;
static uint64_t gl_repair_interior_points;
static uint64_t gl_repair_matches;
static uint64_t gl_repair_topology[PGXP_DIAG_TJ_TOPOLOGIES];
static uint64_t gl_repair_candidates;
static uint64_t gl_repair_gate_invalid_w;
static uint64_t gl_repair_gate_movement;
static uint64_t gl_repair_gate_link_kind;
static uint64_t gl_repair_gate_material;
static uint64_t gl_repair_material[2];
static uint64_t gl_repair_gate_raster;
static uint64_t gl_repair_raster_tested;
static uint64_t gl_repair_raster_base_pixels;
static uint64_t gl_repair_raster_target_pixels;
static uint64_t gl_repair_accepted;
static uint64_t gl_repair_accepted_link[PGXP_DIAG_SUBMIT_LINK_KINDS];
static uint64_t gl_repair_pair_overflow;
static uint64_t gl_repair_proposals;
static uint64_t gl_repair_consistent;
static uint64_t gl_repair_conflicts;
static uint64_t gl_repair_atomic_pairs;
static uint64_t gl_repair_moved;
static uint64_t gl_repair_applied;
static uint64_t gl_repair_y[4];
static uint64_t gl_repair_gouraud[4];
static uint64_t gl_repair_packet[PGXP_DIAG_EDGE_PACKET_BINS];
static uint64_t gl_repair_opcode[32][32];
static uint64_t gl_repair_move_bins[PGXP_DIAG_EDGE_DELTA_BINS];
static double gl_repair_move_sum;
static float gl_repair_move_max;
static uint32_t gl_repair_samples;
static uint32_t gl_repair_window_samples;
static uint64_t gl_native_triangles[4];
static uint64_t gl_native_selected[4];
static uint64_t gl_native_w[2];
static uint64_t gl_native_selected_w[2];
static uint64_t gl_native_vertices;
static uint64_t gl_native_moved;
static uint64_t gl_native_axis_moved[2];
static uint64_t gl_native_delta_bins[PGXP_DIAG_EDGE_DELTA_BINS];
static double gl_native_move_sum;
static float gl_native_move_max;
static uint32_t gl_native_samples;
static uint32_t gl_native_window_samples;
static unsigned gl_repair_mode = PGXP_DIAG_GL_REPAIR_APPLY ?
	PGXP_DIAG_GL_TEST_BROAD_REPLAY : PGXP_DIAG_GL_TEST_OFF;
static uint64_t gl_repair_mode_mask;
/* gl_repair_mode_mask records every selected runtime mode by bit index. */
typedef char pgxp_diag_gl_mode_mask_must_fit_u64[
	PGXP_DIAG_GL_TEST_COUNT <= 64 ? 1 : -1];
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

static void pgxp_diag_gl_reset_stream(void)
{
	gl_vertex_count = 0;
	gl_vertex_overflow = 0;
}

static void pgxp_diag_gl_reset_window(void)
{
	gl_repair_buffers = 0;
	gl_repair_vertices = 0;
	gl_repair_metadata_mismatch = 0;
	gl_repair_metadata_overflow = 0;
	gl_repair_inactive = 0;
	gl_repair_triangles = 0;
	gl_repair_edges = 0;
	gl_repair_lattice_edges = 0;
	gl_repair_long_edges = 0;
	gl_repair_interior_points = 0;
	gl_repair_matches = 0;
	memset(gl_repair_topology, 0, sizeof(gl_repair_topology));
	gl_repair_candidates = 0;
	gl_repair_gate_invalid_w = 0;
	gl_repair_gate_movement = 0;
	gl_repair_gate_link_kind = 0;
	gl_repair_gate_material = 0;
	memset(gl_repair_material, 0, sizeof(gl_repair_material));
	gl_repair_gate_raster = 0;
	gl_repair_raster_tested = 0;
	gl_repair_raster_base_pixels = 0;
	gl_repair_raster_target_pixels = 0;
	gl_repair_accepted = 0;
	memset(gl_repair_accepted_link, 0, sizeof(gl_repair_accepted_link));
	gl_repair_pair_overflow = 0;
	gl_repair_proposals = 0;
	gl_repair_consistent = 0;
	gl_repair_conflicts = 0;
	gl_repair_atomic_pairs = 0;
	gl_repair_moved = 0;
	gl_repair_applied = 0;
	memset(gl_repair_y, 0, sizeof(gl_repair_y));
	memset(gl_repair_gouraud, 0, sizeof(gl_repair_gouraud));
	memset(gl_repair_packet, 0, sizeof(gl_repair_packet));
	memset(gl_repair_opcode, 0, sizeof(gl_repair_opcode));
	memset(gl_repair_move_bins, 0, sizeof(gl_repair_move_bins));
	gl_repair_move_sum = 0.0;
	gl_repair_move_max = 0.0f;
	gl_repair_window_samples = 0;
	memset(gl_native_triangles, 0, sizeof(gl_native_triangles));
	memset(gl_native_selected, 0, sizeof(gl_native_selected));
	memset(gl_native_w, 0, sizeof(gl_native_w));
	memset(gl_native_selected_w, 0, sizeof(gl_native_selected_w));
	gl_native_vertices = 0;
	gl_native_moved = 0;
	memset(gl_native_axis_moved, 0, sizeof(gl_native_axis_moved));
	memset(gl_native_delta_bins, 0, sizeof(gl_native_delta_bins));
	gl_native_move_sum = 0.0;
	gl_native_move_max = 0.0f;
	gl_native_window_samples = 0;
	gl_repair_mode_mask = 0;
}

static const char* pgxp_diag_gl_mode_name(unsigned mode)
{
	static const char* const names[PGXP_DIAG_GL_TEST_COUNT] = {
		"off", "native_boundary", "broad_replay", "native_t_improved",
		"native_t_closed", "native_t_closed_endpoint",
		"native_t_closed_interior", "native_t_closed_material",
		"perp_improved", "perp_closed",
		"perp_closed_endpoint", "perp_closed_interior",
		"perp_point_closed", "perp_improved_material",
		"perp_closed_material", "swan_y_pos", "swan_y_neg",
		"subpixel_nearest", "subpixel_floor", "subpixel_y_phase",
		"upper_left", "upper_left_swan", "upper_left_nearest",
		"native_ot_all", "native_ot_flat_tri", "native_ot_flat_quad",
		"native_ot_gouraud_tri", "native_ot_gouraud_quad",
		"native_ot_x", "native_ot_y", "native_ot_delta_small",
		"native_ot_delta_large", "native_ot_valid_w",
		"native_ot_invalid_w", "vulkan_clip_math", "ot_y_blend_50",
		"ot_y_blend_75", "ot_y_blend_875", "ot_y_gt_half",
		"ot_y_gt_quarter", "ot_y_le_half", "ot_y_native_x_blend_25",
		"ot_y_native_x_blend_50", "ot_y_native_x_blend_75",
		"ot_y_native_x_gt_half", "conservative_raster",
		"coverage_expand_quarter", "coverage_expand_half",
		"coverage_expand_one", "coverage_expand_two",
		"texture_solid_opaque", "texture_transparent_marker",
		"coverage_expand_three", "coverage_expand_four",
		"coverage_expand_two_cap4", "coverage_expand_three_cap4",
		"coverage_expand_four_cap4", "coverage_valid_w_two",
		"coverage_valid_w_three", "coverage_valid_w_four",
		"coverage_valid_w_five", "coverage_valid_w_four_cap16",
		"vk_valid_w_snap_nearest_4bit",
		"vk_valid_w_snap_floor_4bit"
	};
	return mode < PGXP_DIAG_GL_TEST_COUNT ? names[mode] : "invalid";
}

void PGXP_DiagGLSetMode(unsigned mode)
{
	if (mode >= PGXP_DIAG_GL_TEST_COUNT)
		mode = PGXP_DIAG_GL_TEST_OFF;
	if (mode == gl_repair_mode)
		return;
	gl_repair_mode = mode;
	if (log_cb)
		log_cb(RETRO_LOG_INFO,
			"[pgxp_gl_test_mode] mf=%u mode=%u name=%s\n",
			mode_frame, mode, pgxp_diag_gl_mode_name(mode));
}

unsigned PGXP_DiagGLGetMode(void)
{
	return gl_repair_mode;
}

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
	memset(tj_vertex_table, 0, sizeof(tj_vertex_table));
	memset(tj_edges, 0, sizeof(tj_edges));
	tj_table_frame = ~UINT32_C(0);
	tj_edge_count = 0;
	tj_edges_recorded = 0;
	tj_lattice_edges = 0;
	tj_long_edges = 0;
	tj_interior_points = 0;
	memset(tj_matches, 0, sizeof(tj_matches));
	memset(tj_topology, 0, sizeof(tj_topology));
	memset(tj_perp_bins, 0, sizeof(tj_perp_bins));
	memset(tj_predicted_bins, 0, sizeof(tj_predicted_bins));
	memset(tj_packet_bins, 0, sizeof(tj_packet_bins));
	memset(tj_y_band, 0, sizeof(tj_y_band));
	memset(tj_step_bins, 0, sizeof(tj_step_bins));
	memset(tj_risk, 0, sizeof(tj_risk));
	memset(tj_offset_side, 0, sizeof(tj_offset_side));
	memset(tj_risk_invalid_w, 0, sizeof(tj_risk_invalid_w));
	memset(tj_risk_gouraud, 0, sizeof(tj_risk_gouraud));
	memset(tj_risk_packet, 0, sizeof(tj_risk_packet));
	memset(tj_risk_y, 0, sizeof(tj_risk_y));
	memset(tj_context_invalid_w, 0, sizeof(tj_context_invalid_w));
	memset(tj_context_gouraud, 0, sizeof(tj_context_gouraud));
	memset(tj_context_semi, 0, sizeof(tj_context_semi));
	tj_projected_outside = 0;
	tj_degenerate_precise_edge = 0;
	tj_vertex_overflow = 0;
	tj_edge_overflow = 0;
	tj_observation_evictions = 0;
	tj_samples = 0;
	tj_window_samples = 0;
	submit_table_frame = ~UINT32_C(0);
	submit_triangle_count = 0;
	submit_pending_triangle = 0;
	submit_pending_vertices = 0;
	submit_pending_valid = 0;
	submit_primitives = 0;
	submit_quads = 0;
	submit_triangle_overflow = 0;
	submit_transport_calls = 0;
	submit_transport_vertices = 0;
	submit_transport_mismatch_calls = 0;
	submit_transport_mismatch_vertices = 0;
	submit_transport_orphans = 0;
	submit_edges = 0;
	submit_lattice_edges = 0;
	submit_long_edges = 0;
	submit_interior_points = 0;
	submit_matches = 0;
	memset(submit_topology, 0, sizeof(submit_topology));
	submit_risk = 0;
	memset(submit_risk_y, 0, sizeof(submit_risk_y));
	memset(submit_risk_packet, 0, sizeof(submit_risk_packet));
	memset(submit_risk_gouraud, 0, sizeof(submit_risk_gouraud));
	memset(submit_risk_opcode, 0, sizeof(submit_risk_opcode));
	submit_raster_pairs = 0;
	submit_raster_native_pixels = 0;
	memset(submit_raster_hole_pairs, 0, sizeof(submit_raster_hole_pairs));
	memset(submit_raster_hole_pixels, 0, sizeof(submit_raster_hole_pixels));
	submit_raster_snap_improved = 0;
	submit_raster_snap_closed = 0;
	submit_raster_bbox_pixels = 0;
	submit_raster_bbox_skips = 0;
	submit_raster_degenerate = 0;
	memset(submit_link_candidates, 0, sizeof(submit_link_candidates));
	memset(submit_link_tested, 0, sizeof(submit_link_tested));
	memset(submit_link_improved, 0, sizeof(submit_link_improved));
	memset(submit_link_closed, 0, sizeof(submit_link_closed));
	memset(submit_link_worse, 0, sizeof(submit_link_worse));
	memset(submit_link_raw_pixels, 0, sizeof(submit_link_raw_pixels));
	memset(submit_link_snap_pixels, 0, sizeof(submit_link_snap_pixels));
	memset(submit_link_w_error, 0, sizeof(submit_link_w_error));
	memset(submit_link_uv_error, 0, sizeof(submit_link_uv_error));
	submit_samples = 0;
	submit_window_samples = 0;
	pgxp_diag_gl_reset_stream();
	pgxp_diag_gl_reset_window();
	gl_repair_samples = 0;
	gl_native_samples = 0;
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

static unsigned pgxp_diag_submit_link_kind(
		const PGXP_diag_submit_triangle* edge, unsigned first,
		unsigned second, const PGXP_diag_submit_triangle* point,
		unsigned point_vertex, unsigned linked_vertex)
{
	if (point->native_x[point_vertex] == point->native_x[linked_vertex] &&
	    point->native_y[point_vertex] == point->native_y[linked_vertex])
		return 0; /* zero-length native short edge: unsafe to collapse */
	if ((point->native_x[linked_vertex] == edge->native_x[first] &&
	     point->native_y[linked_vertex] == edge->native_y[first]) ||
	    (point->native_x[linked_vertex] == edge->native_x[second] &&
	     point->native_y[linked_vertex] == edge->native_y[second]))
		return 1; /* classic T-junction ending at a long-edge endpoint */
	return 2; /* both short-boundary vertices lie inside the long edge */
}

static double pgxp_diag_submit_native_t(
		const PGXP_diag_submit_triangle* edge, unsigned first,
		unsigned second, int32_t x, int32_t y)
{
	double dx = (double)edge->native_x[second] - edge->native_x[first];
	double dy = (double)edge->native_y[second] - edge->native_y[first];
	double length_squared = dx * dx + dy * dy;
	if (length_squared <= 0.0)
		return 0.0;
	return (((double)x - edge->native_x[first]) * dx +
		((double)y - edge->native_y[first]) * dy) / length_squared;
}

static unsigned pgxp_diag_submit_w_error_bin(
		const PGXP_diag_submit_triangle* edge, unsigned first,
		unsigned second, const PGXP_diag_submit_triangle* point,
		unsigned point_vertex, unsigned linked_vertex)
{
	double inv_first;
	double inv_second;
	double maximum = 0.0;
	unsigned vertices[2];
	unsigned i;
	if (edge->invalid_w || point->invalid_w ||
	    fabsf(edge->precise_w[first]) <= 1.0e-12f ||
	    fabsf(edge->precise_w[second]) <= 1.0e-12f)
		return PGXP_DIAG_SUBMIT_W_BINS - 1u;
	inv_first = 1.0 / edge->precise_w[first];
	inv_second = 1.0 / edge->precise_w[second];
	vertices[0] = point_vertex;
	vertices[1] = linked_vertex;
	for (i = 0; i < 2; i++)
	{
		unsigned vertex = vertices[i];
		double t;
		double predicted;
		double actual;
		double relative;
		if (fabsf(point->precise_w[vertex]) <= 1.0e-12f)
			return PGXP_DIAG_SUBMIT_W_BINS - 1u;
		t = pgxp_diag_submit_native_t(edge, first, second,
			point->native_x[vertex], point->native_y[vertex]);
		predicted = inv_first + t * (inv_second - inv_first);
		actual = 1.0 / point->precise_w[vertex];
		relative = fabs(actual - predicted) /
			(fabs(predicted) > 1.0e-12 ? fabs(predicted) : 1.0e-12);
		if (!isfinite(relative))
			return PGXP_DIAG_SUBMIT_W_BINS - 1u;
		if (relative > maximum)
			maximum = relative;
	}
	if (maximum <= 1.0e-4) return 0;
	if (maximum <= 1.0e-3) return 1;
	if (maximum <= 1.0e-2) return 2;
	if (maximum <= 5.0e-2) return 3;
	if (maximum <= 2.0e-1) return 4;
	if (maximum <= 1.0) return 5;
	return 6;
}

static double pgxp_diag_submit_uv_delta(double actual, double predicted)
{
	double delta = fmod(fabs(actual - predicted), 256.0);
	return delta > 128.0 ? 256.0 - delta : delta;
}

static unsigned pgxp_diag_submit_uv_error_bin(
		const PGXP_diag_submit_triangle* edge, unsigned first,
		unsigned second, const PGXP_diag_submit_triangle* point,
		unsigned point_vertex, unsigned linked_vertex)
{
	double du = (double)(int)edge->u[second] - edge->u[first];
	double dv = (double)(int)edge->v[second] - edge->v[first];
	double maximum = 0.0;
	unsigned vertices[2];
	unsigned i;
	if (du > 128.0) du -= 256.0;
	if (du < -128.0) du += 256.0;
	if (dv > 128.0) dv -= 256.0;
	if (dv < -128.0) dv += 256.0;
	vertices[0] = point_vertex;
	vertices[1] = linked_vertex;
	for (i = 0; i < 2; i++)
	{
		unsigned vertex = vertices[i];
		double t = pgxp_diag_submit_native_t(edge, first, second,
			point->native_x[vertex], point->native_y[vertex]);
		double predicted_u = edge->u[first] + t * du;
		double predicted_v = edge->v[first] + t * dv;
		double error_u = pgxp_diag_submit_uv_delta(point->u[vertex],
			predicted_u);
		double error_v = pgxp_diag_submit_uv_delta(point->v[vertex],
			predicted_v);
		if (error_u > maximum) maximum = error_u;
		if (error_v > maximum) maximum = error_v;
	}
	if (maximum <= 0.5) return 0;
	if (maximum <= 1.0) return 1;
	if (maximum <= 2.0) return 2;
	if (maximum <= 8.0) return 3;
	if (maximum <= 32.0) return 4;
	return 5;
}

static unsigned pgxp_diag_tj_gcd(unsigned a, unsigned b)
{
	while (b)
	{
		unsigned remainder = a % b;
		a = b;
		b = remainder;
	}
	return a;
}

static unsigned pgxp_diag_tj_step_bin(unsigned steps)
{
	if (steps <= 2) return 0;
	if (steps <= 4) return 1;
	if (steps <= 8) return 2;
	if (steps <= 16) return 3;
	if (steps <= 32) return 4;
	if (steps <= 64) return 5;
	return 6;
}

static int64_t pgxp_diag_tj_native_cross(int32_t x0, int32_t y0,
		int32_t x1, int32_t y1, int32_t px, int32_t py)
{
	return (int64_t)(x1 - x0) * (int64_t)(py - y0) -
		(int64_t)(y1 - y0) * (int64_t)(px - x0);
}

static int pgxp_diag_tj_point_on_segment(int32_t x0, int32_t y0,
		int32_t x1, int32_t y1, int32_t px, int32_t py)
{
	int32_t min_x = x0 < x1 ? x0 : x1;
	int32_t max_x = x0 > x1 ? x0 : x1;
	int32_t min_y = y0 < y1 ? y0 : y1;
	int32_t max_y = y0 > y1 ? y0 : y1;
	return pgxp_diag_tj_native_cross(x0, y0, x1, y1, px, py) == 0 &&
		px >= min_x && px <= max_x && py >= min_y && py <= max_y;
}

static PGXP_diag_tj_vertex* pgxp_diag_tj_find_vertex(
		int32_t x, int32_t y, int create)
{
	uint32_t hash = UINT32_C(2166136261);
	unsigned probe;

#define PGXP_TJ_HASH_VALUE(value) do { \
	hash = (hash ^ (uint32_t)(value)) * UINT32_C(16777619); \
} while (0)
	PGXP_TJ_HASH_VALUE(x);
	PGXP_TJ_HASH_VALUE(y);
#undef PGXP_TJ_HASH_VALUE

	for (probe = 0; probe < PGXP_DIAG_TJ_VERTEX_SLOTS; probe++)
	{
		PGXP_diag_tj_vertex* vertex = &tj_vertex_table[
			(hash + probe) & (PGXP_DIAG_TJ_VERTEX_SLOTS - 1)];
		if (!vertex->valid)
		{
			if (!create)
				return NULL;
			vertex->native_x = x;
			vertex->native_y = y;
			vertex->valid = 1;
			return vertex;
		}
		if (vertex->native_x == x && vertex->native_y == y)
			return vertex;
	}
	if (create)
		tj_vertex_overflow++;
	return NULL;
}

static PGXP_diag_tj_observation* pgxp_diag_tj_observation_slot(
		PGXP_diag_tj_vertex* vertex)
{
	PGXP_diag_tj_observation* oldest = &vertex->observation[0];
	unsigned i;

	for (i = 0; i < PGXP_DIAG_TJ_VERTEX_WAYS; i++)
	{
		PGXP_diag_tj_observation* observation = &vertex->observation[i];
		if (!observation->valid)
			return observation;
		if (observation->packet < oldest->packet)
			oldest = observation;
	}
	tj_observation_evictions++;
	return oldest;
}

static void pgxp_diag_tj_reset_frame(uint32_t frame)
{
	memset(tj_vertex_table, 0, sizeof(tj_vertex_table));
	tj_edge_count = 0;
	tj_table_frame = frame;
}

static void pgxp_diag_tj_sample(const PGXP_diag_tj_edge* edge,
		const PGXP_diag_tj_observation* point,
		int32_t point_x, int32_t point_y, float predicted_x,
		float predicted_y, float perpendicular, float predicted_delta,
		float signed_perpendicular, float native_t, float projected_t,
		int64_t edge_side, int64_t point_side, uint64_t gap)
{
	if (!log_cb || tj_samples >= PGXP_DIAG_TJ_SAMPLES ||
	    tj_window_samples >= PGXP_DIAG_TJ_WINDOW_SAMPLES)
		return;
	tj_samples++;
	tj_window_samples++;
	log_cb(RETRO_LOG_INFO,
		"[pgxp_tjunction] n=%u mf=%u edge=%llu/%02x point=%llu/%02x "
		"native=%d/%d-%d/%d point=%d/%d "
		"edge_xy=%.4f/%.4f-%.4f/%.4f point_xy=%.4f/%.4f "
		"predicted=%.4f/%.4f perp=%.6f signed=%.6f "
		"predicted_delta=%.6f t=%.6f projected_t=%.6f "
		"side=%d/%d gap=%llu g=%u/%u invalid_w=%u/%u "
		"w=%.6f/%.6f/%.6f stage=%u/%u/%u uv=%u/%u-%u/%u@%u/%u\n",
		tj_samples, mode_frame,
		(unsigned long long)edge->packet, edge->opcode,
		(unsigned long long)point->packet, point->opcode,
		edge->native_x[0], edge->native_y[0],
		edge->native_x[1], edge->native_y[1], point_x, point_y,
		edge->precise_x[0], edge->precise_y[0],
		edge->precise_x[1], edge->precise_y[1],
		point->precise_x, point->precise_y, predicted_x, predicted_y,
		perpendicular, signed_perpendicular, predicted_delta, native_t,
		projected_t, edge_side < 0 ? -1 : edge_side > 0 ? 1 : 0,
		point_side < 0 ? -1 : point_side > 0 ? 1 : 0,
		(unsigned long long)gap, edge->gouraud, point->gouraud,
		edge->invalid_w, point->invalid_w, edge->precise_w[0],
		edge->precise_w[1], point->precise_w, edge->stage[0],
		edge->stage[1], point->stage, edge->u[0], edge->v[0],
		edge->u[1], edge->v[1], point->u, point->v);
}

static void pgxp_diag_tj_finish_frame(void)
{
	unsigned edge_index;

	if (tj_table_frame == ~UINT32_C(0))
		return;

	for (edge_index = 0; edge_index < tj_edge_count; edge_index++)
	{
		const PGXP_diag_tj_edge* edge = &tj_edges[edge_index];
		int32_t dx = edge->native_x[1] - edge->native_x[0];
		int32_t dy = edge->native_y[1] - edge->native_y[0];
		unsigned abs_dx = (unsigned)(dx < 0 ? -dx : dx);
		unsigned abs_dy = (unsigned)(dy < 0 ? -dy : dy);
		unsigned steps = pgxp_diag_tj_gcd(abs_dx, abs_dy);
		unsigned step;

		if (steps <= 1 || steps > PGXP_DIAG_TJ_MAX_STEPS)
			continue;
		for (step = 1; step < steps; step++)
		{
			int32_t point_x = edge->native_x[0] +
				(int32_t)(((int64_t)dx * step) / steps);
			int32_t point_y = edge->native_y[0] +
				(int32_t)(((int64_t)dy * step) / steps);
			PGXP_diag_tj_vertex* vertex = pgxp_diag_tj_find_vertex(
				point_x, point_y, 0);
			unsigned way;

			if (!vertex)
				continue;
			for (way = 0; way < PGXP_DIAG_TJ_VERTEX_WAYS; way++)
			{
				const PGXP_diag_tj_observation* point =
					&vertex->observation[way];
				unsigned kind;
				unsigned topology = 0;
				unsigned perpendicular_bin;
				unsigned predicted_bin;
				unsigned y_band;
				unsigned context;
				float edge_dx;
				float edge_dy;
				float length_squared;
				float length;
				float relative_x;
				float relative_y;
				float cross;
				float perpendicular;
				float signed_perpendicular;
				float native_t;
				float projected_t;
				float predicted_x;
				float predicted_y;
				float predicted_dx;
				float predicted_dy;
				float predicted_delta;
				uint64_t gap;
				int64_t edge_side;
				int64_t point_side = 0;
				int linked = 0;
				int mixed_side = 0;
				unsigned offset_side = 0;
				unsigned neighbor;

				if (!point->valid || point->packet == edge->packet)
					continue;
				kind = edge->textured == point->textured ?
					(edge->textured ? 2u : 0u) : 1u;
				edge_dx = edge->precise_x[1] - edge->precise_x[0];
				edge_dy = edge->precise_y[1] - edge->precise_y[0];
				length_squared = edge_dx * edge_dx + edge_dy * edge_dy;
				if (length_squared <= 1.0e-12f)
				{
					tj_degenerate_precise_edge++;
					continue;
				}
				length = sqrtf(length_squared);
				relative_x = point->precise_x - edge->precise_x[0];
				relative_y = point->precise_y - edge->precise_y[0];
				cross = edge_dx * relative_y - edge_dy * relative_x;
				signed_perpendicular = cross / length;
				perpendicular = fabsf(signed_perpendicular);
				native_t = (float)step / (float)steps;
				projected_t = (relative_x * edge_dx + relative_y * edge_dy) /
					length_squared;
				predicted_x = edge->precise_x[0] + native_t * edge_dx;
				predicted_y = edge->precise_y[0] + native_t * edge_dy;
				predicted_dx = fabsf(point->precise_x - predicted_x);
				predicted_dy = fabsf(point->precise_y - predicted_y);
				predicted_delta = predicted_dx > predicted_dy ?
					predicted_dx : predicted_dy;
				gap = edge->packet > point->packet ?
					edge->packet - point->packet : point->packet - edge->packet;

				edge_side = pgxp_diag_tj_native_cross(
					edge->native_x[0], edge->native_y[0],
					edge->native_x[1], edge->native_y[1],
					edge->third_x, edge->third_y);
				for (neighbor = 0; neighbor < 2; neighbor++)
				{
					int32_t nx = point->neighbor_x[neighbor];
					int32_t ny = point->neighbor_y[neighbor];
					int64_t side;
					if ((nx != point_x || ny != point_y) &&
					    pgxp_diag_tj_point_on_segment(edge->native_x[0],
						edge->native_y[0], edge->native_x[1],
						edge->native_y[1], nx, ny))
					{
						linked = 1;
						continue;
					}
					side = pgxp_diag_tj_native_cross(edge->native_x[0],
						edge->native_y[0], edge->native_x[1],
						edge->native_y[1], nx, ny);
					if (!side)
						continue;
					if (!point_side)
						point_side = side;
					else if ((point_side < 0) != (side < 0))
						mixed_side = 1;
				}
				if (linked)
					topology = !mixed_side && edge_side && point_side &&
						((edge_side < 0) != (point_side < 0)) ? 2u : 1u;
				if (topology == 2 && perpendicular > 1.0e-6f)
					offset_side = ((signed_perpendicular < 0) ==
						(edge_side < 0)) ? 1u : 2u;

				perpendicular_bin = pgxp_diag_edge_delta_bin(perpendicular);
				predicted_bin = pgxp_diag_edge_delta_bin(predicted_delta);
				y_band = point_y < 64 ? 0u : point_y < 128 ? 1u :
					point_y < 192 ? 2u : 3u;
				context = (edge->invalid_w ? 2u : 0u) |
					(point->invalid_w ? 1u : 0u);
				tj_matches[kind]++;
				tj_topology[kind][topology]++;
				tj_perp_bins[kind][perpendicular_bin]++;
				tj_predicted_bins[kind][predicted_bin]++;
				tj_packet_bins[kind][pgxp_diag_edge_packet_bin(gap)]++;
				tj_y_band[kind][y_band]++;
				tj_step_bins[kind][pgxp_diag_tj_step_bin(steps)]++;
				if (topology == 2)
					tj_offset_side[kind][offset_side]++;
				tj_context_invalid_w[context]++;
				context = (edge->gouraud ? 2u : 0u) |
					(point->gouraud ? 1u : 0u);
				tj_context_gouraud[context]++;
				context = ((edge->opcode & 0x02) ? 2u : 0u) |
					((point->opcode & 0x02) ? 1u : 0u);
				tj_context_semi[context]++;
				if (projected_t <= 0.0f || projected_t >= 1.0f)
					tj_projected_outside++;

				if (topology == 2 && offset_side == 2 && kind == 2 &&
				    !(edge->opcode & 0x02) && !(point->opcode & 0x02) &&
				    perpendicular_bin >= 2 &&
				    perpendicular <= 4.0f)
				{
					tj_risk[kind]++;
					context = (edge->invalid_w ? 2u : 0u) |
						(point->invalid_w ? 1u : 0u);
					tj_risk_invalid_w[context]++;
					context = (edge->gouraud ? 2u : 0u) |
						(point->gouraud ? 1u : 0u);
					tj_risk_gouraud[context]++;
					tj_risk_packet[pgxp_diag_edge_packet_bin(gap)]++;
					tj_risk_y[y_band]++;
					pgxp_diag_tj_sample(edge, point, point_x, point_y,
						predicted_x, predicted_y, perpendicular,
						predicted_delta, signed_perpendicular,
						native_t, projected_t, edge_side, point_side, gap);
				}
			}
		}
	}

	tj_edge_count = 0;
	tj_table_frame = ~UINT32_C(0);
}

static void pgxp_diag_tj_observe(
		const PGXP_diag_primitive_vertex vertices[3],
		int textured, int gouraud, int invalid_w, unsigned upscale_shift)
{
	static const uint8_t edge_vertices[3][3] = {
		{ 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 }
	};
	unsigned scale = 1u << upscale_shift;
	float inverse_scale = 1.0f / (float)scale;
	unsigned i;

	if (tj_table_frame != mode_frame)
		pgxp_diag_tj_reset_frame(mode_frame);

	for (i = 0; i < 3; i++)
	{
		PGXP_diag_tj_vertex* vertex = pgxp_diag_tj_find_vertex(
			vertices[i].native_x / (int32_t)scale,
			vertices[i].native_y / (int32_t)scale, 1);
		PGXP_diag_tj_observation* observation;
		unsigned neighbor0 = (i + 1) % 3;
		unsigned neighbor1 = (i + 2) % 3;

		if (!vertex)
			continue;
		observation = pgxp_diag_tj_observation_slot(vertex);
		observation->precise_x = vertices[i].precise_after_x * inverse_scale;
		observation->precise_y = vertices[i].precise_after_y * inverse_scale;
		observation->precise_w = vertices[i].precise_after_w;
		observation->neighbor_x[0] =
			vertices[neighbor0].native_x / (int32_t)scale;
		observation->neighbor_y[0] =
			vertices[neighbor0].native_y / (int32_t)scale;
		observation->neighbor_x[1] =
			vertices[neighbor1].native_x / (int32_t)scale;
		observation->neighbor_y[1] =
			vertices[neighbor1].native_y / (int32_t)scale;
		observation->packet = current_packet;
		observation->u = vertices[i].u;
		observation->v = vertices[i].v;
		observation->opcode = current_opcode;
		observation->textured = textured != 0;
		observation->gouraud = gouraud != 0;
		observation->invalid_w = invalid_w != 0;
		observation->stage = i < packet_vertex_count ?
			packet_vertices[i].stage : PGXP_TRACE_NONE;
		observation->valid = 1;
	}

	for (i = 0; i < 3; i++)
	{
		unsigned first = edge_vertices[i][0];
		unsigned second = edge_vertices[i][1];
		unsigned third = edge_vertices[i][2];
		int32_t x0 = vertices[first].native_x / (int32_t)scale;
		int32_t y0 = vertices[first].native_y / (int32_t)scale;
		int32_t x1 = vertices[second].native_x / (int32_t)scale;
		int32_t y1 = vertices[second].native_y / (int32_t)scale;
		int32_t dx = x1 - x0;
		int32_t dy = y1 - y0;
		unsigned abs_dx = (unsigned)(dx < 0 ? -dx : dx);
		unsigned abs_dy = (unsigned)(dy < 0 ? -dy : dy);
		unsigned steps = pgxp_diag_tj_gcd(abs_dx, abs_dy);
		PGXP_diag_tj_edge* edge;

		tj_edges_recorded++;
		if (steps <= 1)
			continue;
		tj_lattice_edges++;
		tj_interior_points += steps - 1;
		if (steps > PGXP_DIAG_TJ_MAX_STEPS)
		{
			tj_long_edges++;
			continue;
		}
		if (tj_edge_count >= PGXP_DIAG_TJ_EDGE_CAPACITY)
		{
			tj_edge_overflow++;
			continue;
		}
		edge = &tj_edges[tj_edge_count++];
		edge->native_x[0] = x0;
		edge->native_y[0] = y0;
		edge->native_x[1] = x1;
		edge->native_y[1] = y1;
		edge->third_x = vertices[third].native_x / (int32_t)scale;
		edge->third_y = vertices[third].native_y / (int32_t)scale;
		edge->precise_x[0] = vertices[first].precise_after_x * inverse_scale;
		edge->precise_y[0] = vertices[first].precise_after_y * inverse_scale;
		edge->precise_w[0] = vertices[first].precise_after_w;
		edge->precise_x[1] = vertices[second].precise_after_x * inverse_scale;
		edge->precise_y[1] = vertices[second].precise_after_y * inverse_scale;
		edge->precise_w[1] = vertices[second].precise_after_w;
		edge->packet = current_packet;
		edge->u[0] = vertices[first].u;
		edge->v[0] = vertices[first].v;
		edge->u[1] = vertices[second].u;
		edge->v[1] = vertices[second].v;
		edge->opcode = current_opcode;
		edge->textured = textured != 0;
		edge->gouraud = gouraud != 0;
		edge->invalid_w = invalid_w != 0;
		edge->stage[0] = first < packet_vertex_count ?
			packet_vertices[first].stage : PGXP_TRACE_NONE;
		edge->stage[1] = second < packet_vertex_count ?
			packet_vertices[second].stage : PGXP_TRACE_NONE;
	}
}

static uint32_t pgxp_diag_submit_hash(int32_t x, int32_t y)
{
	uint32_t hash = UINT32_C(2166136261);
	hash = (hash ^ (uint32_t)x) * UINT32_C(16777619);
	hash = (hash ^ (uint32_t)y) * UINT32_C(16777619);
	return hash & (PGXP_DIAG_SUBMIT_HASH_BUCKETS - 1u);
}

static double pgxp_diag_submit_quantize(double value, unsigned bits,
		int round_nearest)
{
	double units;
	if (bits > 16u)
		bits = 16u;
	units = (double)(UINT32_C(1) << bits);
	return round_nearest ? floor(value * units + 0.5) / units :
		floor(value * units) / units;
}

static double pgxp_diag_submit_cross(const double a[2], const double b[2],
		double x, double y)
{
	return (b[0] - a[0]) * (y - a[1]) -
		(b[1] - a[1]) * (x - a[0]);
}

static int pgxp_diag_submit_point_in_triangle(const double triangle[3][2],
		double x, double y)
{
	double area = pgxp_diag_submit_cross(triangle[0], triangle[1],
		triangle[2][0], triangle[2][1]);
	double c0 = pgxp_diag_submit_cross(triangle[0], triangle[1], x, y);
	double c1 = pgxp_diag_submit_cross(triangle[1], triangle[2], x, y);
	double c2 = pgxp_diag_submit_cross(triangle[2], triangle[0], x, y);
	int negative = c0 < 0.0 || c1 < 0.0 || c2 < 0.0;
	int positive = c0 > 0.0 || c1 > 0.0 || c2 > 0.0;
	if (area == 0.0)
		return 0;
	return !(negative && positive);
}

/* Compare the two sides of one submitted T-junction at actual framebuffer
 * pixel centres.  The five models are:
 *   0 raw floating-point PGXP geometry;
 *   1 nearest fixed-point conversion at GL_SUBPIXEL_BITS;
 *   2 floor conversion at GL_SUBPIXEL_BITS (the other plausible phase);
 *   3 nearest conversion at eight subpixel bits;
 *   4 model 1 after projecting the short edge's two boundary vertices onto
 *     the long recovered edge.
 *
 * Model 0 separates a geometric crack from raster quantisation.  Models 1--3
 * test whether backend precision creates/removes a visible sample.  Model 4
 * tests the narrow repair we would actually want, without modifying output. */
static int pgxp_diag_submit_raster(
		const PGXP_diag_submit_triangle* edge_triangle,
		unsigned edge_first, unsigned edge_second,
		const PGXP_diag_submit_triangle* point_triangle,
		unsigned point_vertex, unsigned linked_vertex,
		uint64_t holes[PGXP_DIAG_SUBMIT_MODELS], uint64_t* native_pixels,
		uint64_t* bbox_pixels)
{
	double native_edge[3][2];
	double native_point[3][2];
	double model_edge[PGXP_DIAG_SUBMIT_MODELS][3][2];
	double model_point[PGXP_DIAG_SUBMIT_MODELS][3][2];
	double edge_native_dx;
	double edge_native_dy;
	double edge_native_len2;
	double point_t;
	double linked_t;
	double projected_point[2];
	double projected_linked[2];
	double min_x;
	double min_y;
	double max_x;
	double max_y;
	unsigned gl_bits = submit_gl_subpixel_bits ? submit_gl_subpixel_bits : 4u;
	unsigned scale;
	unsigned model;
	unsigned i;
	int x0;
	int y0;
	int x1;
	int y1;
	int x;
	int y;
	uint64_t area;

	memset(holes, 0, sizeof(uint64_t) * PGXP_DIAG_SUBMIT_MODELS);
	*native_pixels = 0;
	*bbox_pixels = 0;
	if (edge_triangle->upscale_shift != point_triangle->upscale_shift)
		return 0;
	scale = 1u << edge_triangle->upscale_shift;

	for (i = 0; i < 3; i++)
	{
		native_edge[i][0] = (double)edge_triangle->native_x[i] * scale;
		native_edge[i][1] = (double)edge_triangle->native_y[i] * scale;
		native_point[i][0] = (double)point_triangle->native_x[i] * scale;
		native_point[i][1] = (double)point_triangle->native_y[i] * scale;
		for (model = 0; model < PGXP_DIAG_SUBMIT_MODELS; model++)
		{
			model_edge[model][i][0] = edge_triangle->precise_x[i];
			model_edge[model][i][1] = edge_triangle->precise_y[i];
			model_point[model][i][0] = point_triangle->precise_x[i];
			model_point[model][i][1] = point_triangle->precise_y[i];
		}
	}

	edge_native_dx = (double)edge_triangle->native_x[edge_second] -
		edge_triangle->native_x[edge_first];
	edge_native_dy = (double)edge_triangle->native_y[edge_second] -
		edge_triangle->native_y[edge_first];
	edge_native_len2 = edge_native_dx * edge_native_dx +
		edge_native_dy * edge_native_dy;
	if (edge_native_len2 <= 0.0)
		return 0;
	point_t = (((double)point_triangle->native_x[point_vertex] -
		edge_triangle->native_x[edge_first]) * edge_native_dx +
		((double)point_triangle->native_y[point_vertex] -
		edge_triangle->native_y[edge_first]) * edge_native_dy) /
		edge_native_len2;
	linked_t = (((double)point_triangle->native_x[linked_vertex] -
		edge_triangle->native_x[edge_first]) * edge_native_dx +
		((double)point_triangle->native_y[linked_vertex] -
		edge_triangle->native_y[edge_first]) * edge_native_dy) /
		edge_native_len2;
	projected_point[0] = edge_triangle->precise_x[edge_first] + point_t *
		(edge_triangle->precise_x[edge_second] -
		edge_triangle->precise_x[edge_first]);
	projected_point[1] = edge_triangle->precise_y[edge_first] + point_t *
		(edge_triangle->precise_y[edge_second] -
		edge_triangle->precise_y[edge_first]);
	projected_linked[0] = edge_triangle->precise_x[edge_first] + linked_t *
		(edge_triangle->precise_x[edge_second] -
		edge_triangle->precise_x[edge_first]);
	projected_linked[1] = edge_triangle->precise_y[edge_first] + linked_t *
		(edge_triangle->precise_y[edge_second] -
		edge_triangle->precise_y[edge_first]);
	model_point[4][point_vertex][0] = projected_point[0];
	model_point[4][point_vertex][1] = projected_point[1];
	model_point[4][linked_vertex][0] = projected_linked[0];
	model_point[4][linked_vertex][1] = projected_linked[1];

	for (model = 1; model < PGXP_DIAG_SUBMIT_MODELS; model++)
	{
		unsigned bits = model == 3 ? 8u : gl_bits;
		int nearest = model != 2;
		for (i = 0; i < 3; i++)
		{
			model_edge[model][i][0] = pgxp_diag_submit_quantize(
				model_edge[model][i][0], bits, nearest);
			model_edge[model][i][1] = pgxp_diag_submit_quantize(
				model_edge[model][i][1], bits, nearest);
			model_point[model][i][0] = pgxp_diag_submit_quantize(
				model_point[model][i][0], bits, nearest);
			model_point[model][i][1] = pgxp_diag_submit_quantize(
				model_point[model][i][1], bits, nearest);
		}
	}

	min_x = max_x = projected_point[0];
	min_y = max_y = projected_point[1];
#define PGXP_SUBMIT_BBOX_POINT(px, py) do { \
	double _x = (px); double _y = (py); \
	if (_x < min_x) { min_x = _x; } \
	if (_x > max_x) { max_x = _x; } \
	if (_y < min_y) { min_y = _y; } \
	if (_y > max_y) { max_y = _y; } \
} while (0)
	PGXP_SUBMIT_BBOX_POINT(projected_linked[0], projected_linked[1]);
	PGXP_SUBMIT_BBOX_POINT(point_triangle->precise_x[point_vertex],
		point_triangle->precise_y[point_vertex]);
	PGXP_SUBMIT_BBOX_POINT(point_triangle->precise_x[linked_vertex],
		point_triangle->precise_y[linked_vertex]);
	PGXP_SUBMIT_BBOX_POINT(native_point[point_vertex][0],
		native_point[point_vertex][1]);
	PGXP_SUBMIT_BBOX_POINT(native_point[linked_vertex][0],
		native_point[linked_vertex][1]);
#undef PGXP_SUBMIT_BBOX_POINT
	x0 = (int)floor(min_x) - 1;
	y0 = (int)floor(min_y) - 1;
	x1 = (int)ceil(max_x) + 1;
	y1 = (int)ceil(max_y) + 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > (int)(1024u * scale)) x1 = (int)(1024u * scale);
	if (y1 > (int)(512u * scale)) y1 = (int)(512u * scale);
	if (x1 <= x0 || y1 <= y0)
		return 0;
	area = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
	*bbox_pixels = area;
	if (area > PGXP_DIAG_SUBMIT_MAX_BBOX_PIXELS)
		return -1;

	for (y = y0; y < y1; y++)
	{
		double sample_y = (double)y + 0.5;
		for (x = x0; x < x1; x++)
		{
			double sample_x = (double)x + 0.5;
			int native_covered =
				pgxp_diag_submit_point_in_triangle(native_edge,
					sample_x, sample_y) ||
				pgxp_diag_submit_point_in_triangle(native_point,
					sample_x, sample_y);
			if (!native_covered)
				continue;
			(*native_pixels)++;
			for (model = 0; model < PGXP_DIAG_SUBMIT_MODELS; model++)
			{
				int precise_covered =
					pgxp_diag_submit_point_in_triangle(model_edge[model],
						sample_x, sample_y) ||
					pgxp_diag_submit_point_in_triangle(model_point[model],
						sample_x, sample_y);
				if (!precise_covered)
					holes[model]++;
			}
		}
	}
	return 1;
}

static void pgxp_diag_submit_sample(
		const PGXP_diag_submit_triangle* edge_triangle,
		unsigned edge_first, unsigned edge_second,
		const PGXP_diag_submit_triangle* point_triangle,
		unsigned point_vertex, unsigned linked_vertex,
		float perpendicular, uint64_t gap,
		const uint64_t holes[PGXP_DIAG_SUBMIT_MODELS],
		uint64_t native_pixels, uint64_t bbox_pixels)
{
	if (!log_cb || submit_samples >= PGXP_DIAG_SUBMIT_SAMPLES ||
	    submit_window_samples >= PGXP_DIAG_SUBMIT_WINDOW_SAMPLES ||
	    (!holes[0] && !holes[1] && !holes[2] && !holes[3]))
		return;
	submit_samples++;
	submit_window_samples++;
	log_cb(RETRO_LOG_INFO,
		"[pgxp_submit_gap] n=%u mf=%u edge=%llu/%02x point=%llu/%02x "
		"native=%d/%d-%d/%d point=%d/%d linked=%d/%d "
		"edge_xy=%.4f/%.4f-%.4f/%.4f point_xy=%.4f/%.4f "
		"linked_xy=%.4f/%.4f perp=%.6f gap=%llu "
		"holes=raw/glnear/glfloor/high8/snap=%llu/%llu/%llu/%llu/%llu "
		"native_pixels=%llu bbox=%llu subpixel=%u invalid_w=%u/%u "
		"g=%u/%u uv=%u/%u-%u/%u@%u/%u\n",
		submit_samples, mode_frame,
		(unsigned long long)edge_triangle->packet, edge_triangle->opcode,
		(unsigned long long)point_triangle->packet, point_triangle->opcode,
		edge_triangle->native_x[edge_first],
		edge_triangle->native_y[edge_first],
		edge_triangle->native_x[edge_second],
		edge_triangle->native_y[edge_second],
		point_triangle->native_x[point_vertex],
		point_triangle->native_y[point_vertex],
		point_triangle->native_x[linked_vertex],
		point_triangle->native_y[linked_vertex],
		edge_triangle->precise_x[edge_first],
		edge_triangle->precise_y[edge_first],
		edge_triangle->precise_x[edge_second],
		edge_triangle->precise_y[edge_second],
		point_triangle->precise_x[point_vertex],
		point_triangle->precise_y[point_vertex],
		point_triangle->precise_x[linked_vertex],
		point_triangle->precise_y[linked_vertex],
		perpendicular, (unsigned long long)gap,
		(unsigned long long)holes[0], (unsigned long long)holes[1],
		(unsigned long long)holes[2], (unsigned long long)holes[3],
		(unsigned long long)holes[4],
		(unsigned long long)native_pixels,
		(unsigned long long)bbox_pixels, submit_gl_subpixel_bits,
		edge_triangle->invalid_w, point_triangle->invalid_w,
		edge_triangle->gouraud, point_triangle->gouraud,
		edge_triangle->u[edge_first], edge_triangle->v[edge_first],
		edge_triangle->u[edge_second], edge_triangle->v[edge_second],
		point_triangle->u[point_vertex], point_triangle->v[point_vertex]);
}

static void pgxp_diag_submit_reset_frame(uint32_t frame)
{
	if (submit_pending_valid)
		submit_transport_orphans++;
	submit_pending_valid = 0;
	submit_triangle_count = 0;
	submit_table_frame = frame;
}

static void pgxp_diag_submit_finish_frame(void)
{
	uint32_t triangle_index;
	uint32_t node_count;

	if (submit_table_frame == ~UINT32_C(0))
		return;
	if (submit_pending_valid)
	{
		submit_transport_orphans++;
		submit_pending_valid = 0;
	}
	memset(submit_heads, 0xff, sizeof(submit_heads));
	node_count = submit_triangle_count * 3u;
	for (triangle_index = 0; triangle_index < submit_triangle_count;
	     triangle_index++)
	{
		const PGXP_diag_submit_triangle* triangle =
			&submit_triangles[triangle_index];
		unsigned vertex;
		for (vertex = 0; vertex < 3; vertex++)
		{
			uint32_t node_index = triangle_index * 3u + vertex;
			uint32_t bucket = pgxp_diag_submit_hash(
				triangle->native_x[vertex], triangle->native_y[vertex]);
			PGXP_diag_submit_node* node = &submit_nodes[node_index];
			node->triangle = triangle_index;
			node->vertex = (uint8_t)vertex;
			node->next = submit_heads[bucket];
			submit_heads[bucket] = node_index;
		}
	}

	for (triangle_index = 0; triangle_index < submit_triangle_count;
	     triangle_index++)
	{
		static const uint8_t edge_vertices[3][3] = {
			{ 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 }
		};
		const PGXP_diag_submit_triangle* edge_triangle =
			&submit_triangles[triangle_index];
		unsigned edge_index;

		for (edge_index = 0; edge_index < 3; edge_index++)
		{
			unsigned first = edge_vertices[edge_index][0];
			unsigned second = edge_vertices[edge_index][1];
			unsigned third = edge_vertices[edge_index][2];
			int32_t x0 = edge_triangle->native_x[first];
			int32_t y0 = edge_triangle->native_y[first];
			int32_t x1 = edge_triangle->native_x[second];
			int32_t y1 = edge_triangle->native_y[second];
			int32_t dx = x1 - x0;
			int32_t dy = y1 - y0;
			unsigned abs_dx = (unsigned)(dx < 0 ? -dx : dx);
			unsigned abs_dy = (unsigned)(dy < 0 ? -dy : dy);
			unsigned steps = pgxp_diag_tj_gcd(abs_dx, abs_dy);
			unsigned step;

			submit_edges++;
			if (steps <= 1)
				continue;
			submit_lattice_edges++;
			submit_interior_points += steps - 1;
			if (steps > PGXP_DIAG_TJ_MAX_STEPS)
			{
				submit_long_edges++;
				continue;
			}

			for (step = 1; step < steps; step++)
			{
				int32_t point_x = x0 +
					(int32_t)(((int64_t)dx * step) / steps);
				int32_t point_y = y0 +
					(int32_t)(((int64_t)dy * step) / steps);
				uint32_t node_index = submit_heads[
					pgxp_diag_submit_hash(point_x, point_y)];

				while (node_index != UINT32_MAX && node_index < node_count)
				{
					const PGXP_diag_submit_node* node =
						&submit_nodes[node_index];
					const PGXP_diag_submit_triangle* point_triangle =
						&submit_triangles[node->triangle];
					unsigned point_vertex = node->vertex;
					unsigned linked_vertex = 3u;
					int64_t edge_side;
					int64_t point_side = 0;
					int mixed_side = 0;
					unsigned topology = 0;
					unsigned neighbor;
					float edge_dx;
					float edge_dy;
					float length_squared;
					float signed_perpendicular;
					float perpendicular;
					unsigned offset_side = 0;
					uint64_t gap;

					node_index = node->next;
					if (node->triangle == triangle_index ||
					    point_triangle->packet == edge_triangle->packet ||
					    point_triangle->native_x[point_vertex] != point_x ||
					    point_triangle->native_y[point_vertex] != point_y ||
					    point_triangle->upscale_shift !=
						    edge_triangle->upscale_shift)
						continue;
					submit_matches++;
					edge_side = pgxp_diag_tj_native_cross(x0, y0, x1, y1,
						edge_triangle->native_x[third],
						edge_triangle->native_y[third]);
					for (neighbor = 0; neighbor < 3; neighbor++)
					{
						int64_t side;
						if (neighbor == point_vertex)
							continue;
						if (pgxp_diag_tj_point_on_segment(x0, y0, x1, y1,
							point_triangle->native_x[neighbor],
							point_triangle->native_y[neighbor]))
						{
							if (linked_vertex == 3u)
								linked_vertex = neighbor;
							continue;
						}
						side = pgxp_diag_tj_native_cross(x0, y0, x1, y1,
							point_triangle->native_x[neighbor],
							point_triangle->native_y[neighbor]);
						if (!side)
							continue;
						if (!point_side)
							point_side = side;
						else if ((point_side < 0) != (side < 0))
							mixed_side = 1;
					}
					if (linked_vertex != 3u)
						topology = !mixed_side && edge_side && point_side &&
							((edge_side < 0) != (point_side < 0)) ? 2u : 1u;
					submit_topology[topology]++;

					edge_dx = (edge_triangle->precise_x[second] -
						edge_triangle->precise_x[first]) /
						(float)(1u << edge_triangle->upscale_shift);
					edge_dy = (edge_triangle->precise_y[second] -
						edge_triangle->precise_y[first]) /
						(float)(1u << edge_triangle->upscale_shift);
					length_squared = edge_dx * edge_dx + edge_dy * edge_dy;
					if (length_squared <= 1.0e-12f)
					{
						submit_raster_degenerate++;
						continue;
					}
					signed_perpendicular = (edge_dx *
						((point_triangle->precise_y[point_vertex] -
						 edge_triangle->precise_y[first]) /
						 (float)(1u << edge_triangle->upscale_shift)) -
						edge_dy *
						((point_triangle->precise_x[point_vertex] -
						 edge_triangle->precise_x[first]) /
						 (float)(1u << edge_triangle->upscale_shift))) /
						sqrtf(length_squared);
					perpendicular = fabsf(signed_perpendicular);
					if (topology == 2u && perpendicular > 1.0e-6f)
						offset_side = ((signed_perpendicular < 0) ==
							(edge_side < 0)) ? 1u : 2u;

					if (topology == 2u && offset_side == 2u &&
					    edge_triangle->textured && point_triangle->textured &&
					    !edge_triangle->semi_transparent &&
					    !point_triangle->semi_transparent &&
					    perpendicular > (1.0f / 64.0f) &&
					    perpendicular <= 4.0f)
					{
						uint64_t holes[PGXP_DIAG_SUBMIT_MODELS];
						uint64_t native_pixels;
						uint64_t bbox_pixels;
						int raster_result;
						unsigned context;
						unsigned y_band = point_y < 64 ? 0u :
							point_y < 128 ? 1u : point_y < 192 ? 2u : 3u;
						unsigned packet_bin;
						unsigned link_kind;
						unsigned model;
						gap = edge_triangle->packet > point_triangle->packet ?
							edge_triangle->packet - point_triangle->packet :
							point_triangle->packet - edge_triangle->packet;
						packet_bin = pgxp_diag_edge_packet_bin(gap);
						link_kind = pgxp_diag_submit_link_kind(edge_triangle,
							first, second, point_triangle, point_vertex,
							linked_vertex);
						submit_risk++;
						submit_risk_y[y_band]++;
						submit_risk_packet[packet_bin]++;
						context = (edge_triangle->gouraud ? 2u : 0u) |
							(point_triangle->gouraud ? 1u : 0u);
						submit_risk_gouraud[context]++;
						submit_risk_opcode[edge_triangle->opcode & 0x1fu]
							[point_triangle->opcode & 0x1fu]++;
						submit_link_candidates[link_kind][packet_bin]++;
						submit_link_w_error[link_kind]
							[pgxp_diag_submit_w_error_bin(edge_triangle,
							first, second, point_triangle, point_vertex,
							linked_vertex)]++;
						submit_link_uv_error[link_kind]
							[pgxp_diag_submit_uv_error_bin(edge_triangle,
							first, second, point_triangle, point_vertex,
							linked_vertex)]++;
						raster_result = pgxp_diag_submit_raster(edge_triangle,
							first, second, point_triangle, point_vertex,
							linked_vertex, holes, &native_pixels, &bbox_pixels);
						if (raster_result < 0)
						{
							submit_raster_bbox_skips++;
							continue;
						}
						if (!raster_result)
							continue;
						submit_link_tested[link_kind][packet_bin]++;
						submit_link_raw_pixels[link_kind][packet_bin] += holes[1];
						submit_link_snap_pixels[link_kind][packet_bin] += holes[4];
						if (holes[1] > holes[4])
							submit_link_improved[link_kind][packet_bin]++;
						else if (holes[4] > holes[1])
							submit_link_worse[link_kind][packet_bin]++;
						if (holes[1] && !holes[4])
							submit_link_closed[link_kind][packet_bin]++;
						submit_raster_pairs++;
						submit_raster_native_pixels += native_pixels;
						submit_raster_bbox_pixels += bbox_pixels;
						for (model = 0; model < PGXP_DIAG_SUBMIT_MODELS; model++)
						{
							submit_raster_hole_pixels[model] += holes[model];
							if (holes[model])
								submit_raster_hole_pairs[model]++;
						}
						if (holes[1] > holes[4])
							submit_raster_snap_improved++;
						if (holes[1] && !holes[4])
							submit_raster_snap_closed++;
						pgxp_diag_submit_sample(edge_triangle, first, second,
							point_triangle, point_vertex, linked_vertex,
							perpendicular, gap, holes, native_pixels, bbox_pixels);
					}
				}
			}
		}
	}

	submit_triangle_count = 0;
	submit_table_frame = ~UINT32_C(0);
}

void PGXP_DiagSubmitPrimitive(const PGXP_diag_primitive_vertex* vertices,
		unsigned count, int invalid_w, unsigned upscale_shift)
{
	static const uint8_t triangle_vertices[2][3] = {
		{ 0, 1, 2 }, { 3, 2, 1 }
	};
	unsigned triangle_count = count == 4 ? 2u : count == 3 ? 1u : 0u;
	unsigned triangle;
	unsigned scale;

	if (!triangle_count || upscale_shift > 4u)
		return;
	if (submit_table_frame != mode_frame)
		pgxp_diag_submit_reset_frame(mode_frame);
	if (submit_pending_valid)
		submit_transport_orphans++;
	submit_pending_triangle = submit_triangle_count;
	submit_pending_vertices = (uint8_t)(triangle_count * 3u);
	submit_pending_valid = 1;
	submit_primitives++;
	if (count == 4)
		submit_quads++;
	scale = 1u << upscale_shift;
	for (triangle = 0; triangle < triangle_count; triangle++)
	{
		PGXP_diag_submit_triangle* output;
		unsigned i;
		if (submit_triangle_count >= PGXP_DIAG_SUBMIT_TRIANGLES)
		{
			submit_triangle_overflow++;
			submit_pending_valid = 0;
			continue;
		}
		output = &submit_triangles[submit_triangle_count++];
		memset(output, 0, sizeof(*output));
		for (i = 0; i < 3; i++)
		{
			unsigned source = triangle_vertices[triangle][i];
			output->native_x[i] = vertices[source].native_x / (int32_t)scale;
			output->native_y[i] = vertices[source].native_y / (int32_t)scale;
			output->precise_x[i] = vertices[source].precise_after_x;
			output->precise_y[i] = vertices[source].precise_after_y;
			output->precise_w[i] = vertices[source].precise_after_w;
			output->u[i] = vertices[source].u;
			output->v[i] = vertices[source].v;
		}
		output->packet = current_packet;
		output->opcode = current_opcode;
		output->upscale_shift = (uint8_t)upscale_shift;
		output->textured = !!(current_opcode & 0x04);
		output->gouraud = !!(current_opcode & 0x10);
		output->invalid_w = invalid_w != 0;
		output->semi_transparent = !!(current_opcode & 0x02);
	}
}

void PGXP_DiagGLPrimitive(const void* vertices, unsigned count,
		unsigned stride_bytes, uint64_t material_key)
{
	const uint8_t* bytes = (const uint8_t*)vertices;
	uint64_t mismatches = 0;
	unsigned base = gl_vertex_count;
	int sidecar_space = count <= PGXP_DIAG_GL_VERTEX_CAPACITY -
		gl_vertex_count;
	unsigned i;

	/* Every GL command vertex gets a sidecar slot, including sprites and
	 * lines.  Invalid placeholders preserve exact alignment across mixed draw
	 * calls; only final polygon triangles receive topology metadata. */
	if (sidecar_space)
	{
		memset(&gl_vertices[base], 0, count * sizeof(gl_vertices[0]));
		gl_vertex_count += count;
	}
	else
	{
		gl_vertex_overflow = 1;
		gl_repair_metadata_overflow++;
	}

	/* Sprites and non-PGXP callers do not install a pending polygon record.
	 * Their placeholder slots must remain invalid. */
	if (!submit_pending_valid)
		return;
	submit_transport_calls++;
	submit_transport_vertices += count;
	if (!vertices || stride_bytes < 4u * sizeof(float) ||
	    count != submit_pending_vertices)
	{
		submit_transport_mismatch_calls++;
		if (log_cb && submit_transport_mismatch_calls <= 8)
			log_cb(RETRO_LOG_ERROR,
				"[pgxp_submit_transport_error] mf=%u pending=%u "
				"expected=%u actual=%u stride=%u\n", mode_frame,
				submit_pending_valid, submit_pending_vertices, count,
				stride_bytes);
		submit_pending_valid = 0;
		return;
	}
	for (i = 0; i < count; i++)
	{
		const float* actual = (const float*)(bytes + i * stride_bytes);
		const PGXP_diag_submit_triangle* expected =
			&submit_triangles[submit_pending_triangle + i / 3u];
		unsigned vertex = i % 3u;
		if (actual[0] != expected->precise_x[vertex] ||
		    actual[1] != expected->precise_y[vertex] ||
		    actual[3] != expected->precise_w[vertex])
		{
			mismatches++;
			if (log_cb && submit_transport_mismatch_calls < 8 &&
			    mismatches <= 3)
				log_cb(RETRO_LOG_ERROR,
					"[pgxp_submit_transport_vertex] mf=%u i=%u "
					"expected=%.9g/%.9g/%.9g actual=%.9g/%.9g/%.9g\n",
					mode_frame, i, expected->precise_x[vertex],
					expected->precise_y[vertex], expected->precise_w[vertex],
					actual[0], actual[1], actual[3]);
		}
	}
	if (mismatches)
	{
		submit_transport_mismatch_calls++;
		submit_transport_mismatch_vertices += mismatches;
	}
	else if (sidecar_space)
	{
		for (i = 0; i < count; i++)
		{
			const PGXP_diag_submit_triangle* expected =
				&submit_triangles[submit_pending_triangle + i / 3u];
			PGXP_diag_gl_vertex* output = &gl_vertices[base + i];
			unsigned vertex = i % 3u;
			output->native_x = expected->native_x[vertex];
			output->native_y = expected->native_y[vertex];
			output->packet = expected->packet;
			output->material_key = material_key;
			output->triangle_start = base + (i / 3u) * 3u;
			output->u = expected->u[vertex];
			output->v = expected->v[vertex];
			output->opcode = expected->opcode;
			output->vertex = (uint8_t)vertex;
			output->upscale_shift = expected->upscale_shift;
			output->textured = expected->textured;
			output->gouraud = expected->gouraud;
			output->invalid_w = expected->invalid_w;
			output->semi_transparent = expected->semi_transparent;
			output->valid = 1;
		}
	}
	submit_pending_valid = 0;
}

static int pgxp_diag_gl_triangle_valid(uint32_t start, unsigned count)
{
	unsigned i;
	if (start > count || count - start < 3u)
		return 0;
	for (i = 0; i < 3; i++)
		if (!gl_vertices[start + i].valid ||
		    gl_vertices[start + i].triangle_start != start ||
		    gl_vertices[start + i].vertex != i)
			return 0;
	return 1;
}

static float* pgxp_diag_gl_position(void* vertices, unsigned stride_bytes,
		uint32_t index)
{
	return (float*)((uint8_t*)vertices + (size_t)index * stride_bytes);
}

static void pgxp_diag_gl_add_proposal(uint32_t index, float x, float y,
		float epsilon)
{
	PGXP_diag_gl_proposal* proposal = &gl_proposals[index];
	if (!proposal->count)
	{
		proposal->x = x;
		proposal->y = y;
	}
	else if (fabsf(proposal->x - x) > epsilon ||
	         fabsf(proposal->y - y) > epsilon)
		proposal->conflict = 1;
	if (proposal->count != UINT16_MAX)
		proposal->count++;
}

static int pgxp_diag_gl_mode_is_perpendicular(unsigned mode)
{
	return mode >= PGXP_DIAG_GL_TEST_PERP_IMPROVED &&
		mode <= PGXP_DIAG_GL_TEST_PERP_CLOSED_MATERIAL;
}

static int pgxp_diag_gl_mode_is_closed(unsigned mode)
{
	return mode == PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED ||
		mode == PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_ENDPOINT ||
		mode == PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_INTERIOR ||
		mode == PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_MATERIAL ||
		mode == PGXP_DIAG_GL_TEST_PERP_CLOSED ||
		mode == PGXP_DIAG_GL_TEST_PERP_CLOSED_ENDPOINT ||
		mode == PGXP_DIAG_GL_TEST_PERP_CLOSED_INTERIOR ||
		mode == PGXP_DIAG_GL_TEST_PERP_POINT_CLOSED ||
		mode == PGXP_DIAG_GL_TEST_PERP_CLOSED_MATERIAL;
}

static int pgxp_diag_gl_mode_needs_raster(unsigned mode)
{
	return mode >= PGXP_DIAG_GL_TEST_NATIVE_T_IMPROVED;
}

static int pgxp_diag_gl_mode_accepts_link(unsigned mode, unsigned link_kind)
{
	/* The 526c324b classifier found zero improvements and 5,302 worsened
	 * pairs among zero-length native short edges.  Keep them only in the
	 * observation and exact broad-replay controls. */
	if (mode != PGXP_DIAG_GL_TEST_OFF &&
	    mode != PGXP_DIAG_GL_TEST_BROAD_REPLAY && !link_kind)
		return 0;
	if ((mode == PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_ENDPOINT ||
	     mode == PGXP_DIAG_GL_TEST_PERP_CLOSED_ENDPOINT) && link_kind != 1u)
		return 0;
	if ((mode == PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_INTERIOR ||
	     mode == PGXP_DIAG_GL_TEST_PERP_CLOSED_INTERIOR) && link_kind != 2u)
		return 0;
	return 1;
}

static int pgxp_diag_gl_mode_needs_material(unsigned mode)
{
	return mode == PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_MATERIAL ||
		mode == PGXP_DIAG_GL_TEST_PERP_IMPROVED_MATERIAL ||
		mode == PGXP_DIAG_GL_TEST_PERP_CLOSED_MATERIAL;
}

static int pgxp_diag_gl_mode_is_native_handoff(unsigned mode)
{
	return (mode >= PGXP_DIAG_GL_TEST_NATIVE_OT_ALL &&
		mode <= PGXP_DIAG_GL_TEST_NATIVE_OT_INVALID_W) ||
		(mode >= PGXP_DIAG_GL_TEST_OT_Y_BLEND_50 &&
		 mode <= PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_GT_HALF);
}

static int pgxp_diag_gl_native_select(unsigned mode, unsigned class_index,
		int invalid_w, float triangle_delta)
{
	switch (mode)
	{
		case PGXP_DIAG_GL_TEST_NATIVE_OT_ALL:
		case PGXP_DIAG_GL_TEST_NATIVE_OT_X_ONLY:
		case PGXP_DIAG_GL_TEST_NATIVE_OT_Y_ONLY:
		case PGXP_DIAG_GL_TEST_OT_Y_BLEND_50:
		case PGXP_DIAG_GL_TEST_OT_Y_BLEND_75:
		case PGXP_DIAG_GL_TEST_OT_Y_BLEND_875:
		case PGXP_DIAG_GL_TEST_OT_Y_GT_HALF:
		case PGXP_DIAG_GL_TEST_OT_Y_GT_QUARTER:
		case PGXP_DIAG_GL_TEST_OT_Y_LE_HALF:
		case PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_25:
		case PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_50:
		case PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_75:
		case PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_GT_HALF:
			return 1;
		case PGXP_DIAG_GL_TEST_NATIVE_OT_FLAT_TRI:
			return class_index == 0u;
		case PGXP_DIAG_GL_TEST_NATIVE_OT_FLAT_QUAD:
			return class_index == 1u;
		case PGXP_DIAG_GL_TEST_NATIVE_OT_GOURAUD_TRI:
			return class_index == 2u;
		case PGXP_DIAG_GL_TEST_NATIVE_OT_GOURAUD_QUAD:
			return class_index == 3u;
		case PGXP_DIAG_GL_TEST_NATIVE_OT_DELTA_SMALL:
			return triangle_delta <= 0.5f;
		case PGXP_DIAG_GL_TEST_NATIVE_OT_DELTA_LARGE:
			return triangle_delta > 0.5f;
		case PGXP_DIAG_GL_TEST_NATIVE_OT_VALID_W:
			return !invalid_w;
		case PGXP_DIAG_GL_TEST_NATIVE_OT_INVALID_W:
			return invalid_w;
		default:
			return 0;
	}
}

static void pgxp_diag_gl_native_weights(unsigned mode,
		float* x_weight, float* y_weight)
{
	*x_weight = 1.0f;
	*y_weight = 1.0f;
	switch (mode)
	{
		case PGXP_DIAG_GL_TEST_NATIVE_OT_X_ONLY:
			*y_weight = 0.0f;
			break;
		case PGXP_DIAG_GL_TEST_NATIVE_OT_Y_ONLY:
		case PGXP_DIAG_GL_TEST_OT_Y_GT_HALF:
		case PGXP_DIAG_GL_TEST_OT_Y_GT_QUARTER:
		case PGXP_DIAG_GL_TEST_OT_Y_LE_HALF:
			*x_weight = 0.0f;
			break;
		case PGXP_DIAG_GL_TEST_OT_Y_BLEND_50:
			*x_weight = 0.0f;
			*y_weight = 0.5f;
			break;
		case PGXP_DIAG_GL_TEST_OT_Y_BLEND_75:
			*x_weight = 0.0f;
			*y_weight = 0.75f;
			break;
		case PGXP_DIAG_GL_TEST_OT_Y_BLEND_875:
			*x_weight = 0.0f;
			*y_weight = 0.875f;
			break;
		case PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_25:
			*x_weight = 0.25f;
			break;
		case PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_50:
			*x_weight = 0.5f;
			break;
		case PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_75:
			*x_weight = 0.75f;
			break;
		default:
			break;
	}
}

static int pgxp_diag_gl_native_axis_selected(unsigned mode,
		int x_axis, float delta)
{
	if (!x_axis)
	{
		if (mode == PGXP_DIAG_GL_TEST_OT_Y_GT_HALF)
			return delta > 0.5f;
		if (mode == PGXP_DIAG_GL_TEST_OT_Y_GT_QUARTER)
			return delta > 0.25f;
		if (mode == PGXP_DIAG_GL_TEST_OT_Y_LE_HALF)
			return delta <= 0.5f;
	}
	else if (mode == PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_GT_HALF)
		return delta > 0.5f;
	return 1;
}

/* Reproduce dd4f6ade's known-positive native coverage substitution at the
 * final mapped GL stream, where the sidecar proves that each native/precise
 * vertex still belongs to the same decoded polygon.  The runtime modes split
 * that control into exhaustive primitive classes without guessing adjacency.
 * W, UV, depth, colour, ordering, batching, and every non-opaque-textured
 * primitive remain byte-for-byte untouched. */
static void pgxp_diag_gl_native_handoff(void* vertices, unsigned count,
		unsigned stride_bytes, unsigned mode)
{
	uint32_t start;
	float x_weight;
	float y_weight;
	pgxp_diag_gl_native_weights(mode, &x_weight, &y_weight);

	for (start = 0; start < count; start++)
	{
		const PGXP_diag_gl_vertex* triangle;
		float delta[3];
		float precise_x[3];
		float precise_y[3];
		float precise_w[3];
		float triangle_delta = 0.0f;
		unsigned class_index;
		unsigned invalid_index;
		unsigned i;

		if (!pgxp_diag_gl_triangle_valid(start, count) ||
		    gl_vertices[start].vertex != 0)
			continue;
		triangle = &gl_vertices[start];
		if (!triangle->textured || triangle->semi_transparent)
			continue;
		class_index = (triangle->gouraud ? 2u : 0u) |
			((triangle->opcode & 0x08u) ? 1u : 0u);
		invalid_index = triangle->invalid_w ? 1u : 0u;
		gl_native_triangles[class_index]++;
		gl_native_w[invalid_index]++;

		for (i = 0; i < 3; i++)
		{
			const float* position = pgxp_diag_gl_position(vertices,
				stride_bytes, start + i);
			float dx;
			float dy;
			precise_x[i] = position[0];
			precise_y[i] = position[1];
			precise_w[i] = position[3];
			dx = (float)gl_vertices[start + i].native_x - position[0];
			dy = (float)gl_vertices[start + i].native_y - position[1];
			delta[i] = hypotf(dx, dy);
			if (delta[i] > triangle_delta)
				triangle_delta = delta[i];
		}
		if (!pgxp_diag_gl_native_select(mode, class_index,
			triangle->invalid_w, triangle_delta))
			continue;
		if (log_cb && gl_native_samples < PGXP_DIAG_GL_NATIVE_SAMPLES &&
		    gl_native_window_samples < PGXP_DIAG_GL_NATIVE_WINDOW_SAMPLES)
		{
			log_cb(RETRO_LOG_INFO,
				"[pgxp_gl_native_sample] n=%u mf=%u mode=%u "
				"packet=%llu opcode=%02x class=%u invalid_w=%u upscale=%u "
				"delta=%.6f vertex_delta=%.6f/%.6f/%.6f "
				"native=%d/%d,%d/%d,%d/%d "
				"precise=%.6f/%.6f,%.6f/%.6f,%.6f/%.6f "
				"w=%.9g/%.9g/%.9g uv=%u/%u,%u/%u,%u/%u\n",
				gl_native_samples + 1, mode_frame, mode,
				(unsigned long long)triangle->packet, triangle->opcode,
				class_index, invalid_index, triangle->upscale_shift,
				triangle_delta, delta[0], delta[1], delta[2],
				gl_vertices[start].native_x, gl_vertices[start].native_y,
				gl_vertices[start + 1].native_x,
				gl_vertices[start + 1].native_y,
				gl_vertices[start + 2].native_x,
				gl_vertices[start + 2].native_y,
				precise_x[0], precise_y[0], precise_x[1], precise_y[1],
				precise_x[2], precise_y[2], precise_w[0], precise_w[1],
				precise_w[2], gl_vertices[start].u, gl_vertices[start].v,
				gl_vertices[start + 1].u, gl_vertices[start + 1].v,
				gl_vertices[start + 2].u, gl_vertices[start + 2].v);
			gl_native_samples++;
			gl_native_window_samples++;
		}

		gl_native_selected[class_index]++;
		gl_native_selected_w[invalid_index]++;
		gl_native_vertices += 3;
		for (i = 0; i < 3; i++)
		{
			float* position = pgxp_diag_gl_position(vertices, stride_bytes,
				start + i);
			float dx = (float)gl_vertices[start + i].native_x - position[0];
			float dy = (float)gl_vertices[start + i].native_y - position[1];
			int apply_x = x_weight > 0.0f &&
				pgxp_diag_gl_native_axis_selected(mode, 1, fabsf(dx));
			int apply_y = y_weight > 0.0f &&
				pgxp_diag_gl_native_axis_selected(mode, 0, fabsf(dy));
			float applied_x = apply_x ? dx * x_weight : 0.0f;
			float applied_y = apply_y ? dy * y_weight : 0.0f;
			float applied_delta = hypotf(applied_x, applied_y);
			if (applied_delta > 1.0e-6f)
			{
				gl_native_moved++;
				if (apply_x && fabsf(dx) > 1.0e-6f)
					gl_native_axis_moved[0]++;
				if (apply_y && fabsf(dy) > 1.0e-6f)
					gl_native_axis_moved[1]++;
				gl_native_delta_bins[
					pgxp_diag_edge_delta_bin(applied_delta)]++;
				gl_native_move_sum += applied_delta;
				if (applied_delta > gl_native_move_max)
					gl_native_move_max = applied_delta;
			}
			if (apply_x)
				position[0] += applied_x;
			if (apply_y)
				position[1] += applied_y;
		}
	}
}

static void pgxp_diag_gl_build_triangle(void* vertices,
		unsigned stride_bytes, uint32_t start,
		PGXP_diag_submit_triangle* output)
{
	unsigned i;
	memset(output, 0, sizeof(*output));
	for (i = 0; i < 3; i++)
	{
		const PGXP_diag_gl_vertex* sidecar = &gl_vertices[start + i];
		const float* position = pgxp_diag_gl_position(vertices, stride_bytes,
			start + i);
		output->native_x[i] = sidecar->native_x;
		output->native_y[i] = sidecar->native_y;
		output->precise_x[i] = position[0];
		output->precise_y[i] = position[1];
		output->precise_w[i] = position[3];
		output->u[i] = sidecar->u;
		output->v[i] = sidecar->v;
	}
	output->packet = gl_vertices[start].packet;
	output->opcode = gl_vertices[start].opcode;
	output->upscale_shift = gl_vertices[start].upscale_shift;
	output->textured = gl_vertices[start].textured;
	output->gouraud = gl_vertices[start].gouraud;
	output->invalid_w = gl_vertices[start].invalid_w;
	output->semi_transparent = gl_vertices[start].semi_transparent;
}

static int pgxp_diag_gl_raster_gate(void* vertices, unsigned stride_bytes,
		uint32_t edge_start, unsigned first, unsigned second,
		uint32_t point_start, unsigned point_vertex, unsigned linked_vertex,
		float point_target_x, float point_target_y,
		float linked_target_x, float linked_target_y,
		unsigned mode, uint64_t* base_pixels, uint64_t* target_pixels)
{
	PGXP_diag_submit_triangle edge_triangle;
	PGXP_diag_submit_triangle point_triangle;
	uint64_t holes[PGXP_DIAG_SUBMIT_MODELS];
	uint64_t native_pixels;
	uint64_t bbox_pixels;
	int result;

	pgxp_diag_gl_build_triangle(vertices, stride_bytes, edge_start,
		&edge_triangle);
	pgxp_diag_gl_build_triangle(vertices, stride_bytes, point_start,
		&point_triangle);
	result = pgxp_diag_submit_raster(&edge_triangle, first, second,
		&point_triangle, point_vertex, linked_vertex, holes, &native_pixels,
		&bbox_pixels);
	if (result <= 0)
		return 0;
	*base_pixels = holes[1];
	if (!pgxp_diag_gl_mode_is_perpendicular(mode))
	{
		*target_pixels = holes[4];
		return 1;
	}

	/* For the perpendicular modes, preserve each PGXP vertex's along-edge
	 * position and remove only its normal displacement.  Re-evaluate that
	 * exact target rather than assuming the native-t model's coverage. */
	point_triangle.precise_x[point_vertex] = point_target_x;
	point_triangle.precise_y[point_vertex] = point_target_y;
	point_triangle.precise_x[linked_vertex] = linked_target_x;
	point_triangle.precise_y[linked_vertex] = linked_target_y;
	result = pgxp_diag_submit_raster(&edge_triangle, first, second,
		&point_triangle, point_vertex, linked_vertex, holes, &native_pixels,
		&bbox_pixels);
	if (result <= 0)
		return 0;
	*target_pixels = holes[1];
	return 1;
}

/* Repair T-junctions in the actual mapped OpenGL stream.  The long triangle
 * retains its PGXP coordinates; only the two vertices forming the matching
 * short boundary are projected onto it.  All decisions are buffered so a
 * vertex with disagreeing targets is left untouched, and both ends of a
 * boundary must survive conflict resolution before either move is applied. */
void PGXP_DiagGLRepair(void* vertices, unsigned count,
		unsigned stride_bytes)
{
	static const uint8_t edge_vertices[3][3] = {
		{ 0, 1, 2 }, { 1, 2, 0 }, { 2, 0, 1 }
	};
	uint32_t pair_count = 0;
	uint32_t edge_start;
	int pair_overflow = 0;

	if (!vertices || !count)
	{
		pgxp_diag_gl_reset_stream();
		return;
	}

	gl_repair_buffers++;
	gl_repair_vertices += count;
	if (stride_bytes < 4u * sizeof(float) ||
	    gl_vertex_overflow || count != gl_vertex_count)
	{
		gl_repair_metadata_mismatch++;
		if (log_cb && gl_repair_metadata_mismatch <= 8)
			log_cb(RETRO_LOG_ERROR,
				"[pgxp_gl_repair_error] mf=%u vertices=%u sidecar=%u "
				"stride=%u overflow=%u\n", mode_frame, count,
				gl_vertex_count, stride_bytes, gl_vertex_overflow);
		pgxp_diag_gl_reset_stream();
		return;
	}
	if (!(PGXP_GetModes() & (PGXP_MODE_MEMORY | PGXP_VERTEX_CACHE)))
	{
		gl_repair_inactive++;
		pgxp_diag_gl_reset_stream();
		return;
	}
	gl_repair_mode_mask |= UINT64_C(1) << gl_repair_mode;
	if (pgxp_diag_gl_mode_is_native_handoff(gl_repair_mode))
	{
		pgxp_diag_gl_native_handoff(vertices, count, stride_bytes,
			gl_repair_mode);
		pgxp_diag_gl_reset_stream();
		return;
	}
	/* Raster modes 15..22 never alter CPU vertices.  Keep collecting the
	 * exact submitted stream, but never feed them into the retired T-junction
	 * projector (whose default branch would otherwise interpret an unknown
	 * mode as a broad native-t mutation). */
	if (gl_repair_mode > PGXP_DIAG_GL_TEST_PERP_CLOSED_MATERIAL)
	{
		pgxp_diag_gl_reset_stream();
		return;
	}

	memset(gl_hash_heads, 0xff, sizeof(gl_hash_heads));
	memset(gl_proposals, 0, count * sizeof(gl_proposals[0]));
	for (edge_start = 0; edge_start < count; edge_start++)
	{
		uint32_t bucket;
		if (!gl_vertices[edge_start].valid)
		{
			gl_hash_next[edge_start] = UINT32_MAX;
			continue;
		}
		bucket = pgxp_diag_submit_hash(gl_vertices[edge_start].native_x,
			gl_vertices[edge_start].native_y) &
			(PGXP_DIAG_GL_HASH_BUCKETS - 1u);
		gl_hash_next[edge_start] = gl_hash_heads[bucket];
		gl_hash_heads[bucket] = edge_start;
	}

	for (edge_start = 0; edge_start < count; edge_start++)
	{
		const PGXP_diag_gl_vertex* edge_triangle;
		unsigned edge_index;
		if (!pgxp_diag_gl_triangle_valid(edge_start, count) ||
		    gl_vertices[edge_start].vertex != 0)
			continue;
		edge_triangle = &gl_vertices[edge_start];
		gl_repair_triangles++;
		if (!edge_triangle->textured || edge_triangle->semi_transparent)
			continue;

		for (edge_index = 0; edge_index < 3; edge_index++)
		{
			unsigned first = edge_vertices[edge_index][0];
			unsigned second = edge_vertices[edge_index][1];
			unsigned third = edge_vertices[edge_index][2];
			const PGXP_diag_gl_vertex* edge_first =
				&gl_vertices[edge_start + first];
			const PGXP_diag_gl_vertex* edge_second =
				&gl_vertices[edge_start + second];
			int32_t x0 = edge_first->native_x;
			int32_t y0 = edge_first->native_y;
			int32_t x1 = edge_second->native_x;
			int32_t y1 = edge_second->native_y;
			int32_t dx = x1 - x0;
			int32_t dy = y1 - y0;
			unsigned abs_dx = (unsigned)(dx < 0 ? -dx : dx);
			unsigned abs_dy = (unsigned)(dy < 0 ? -dy : dy);
			unsigned steps = pgxp_diag_tj_gcd(abs_dx, abs_dy);
			unsigned step;

			gl_repair_edges++;
			if (steps <= 1)
				continue;
			gl_repair_lattice_edges++;
			gl_repair_interior_points += steps - 1u;
			if (steps > PGXP_DIAG_TJ_MAX_STEPS)
			{
				gl_repair_long_edges++;
				continue;
			}

			for (step = 1; step < steps; step++)
			{
				int32_t point_x = x0 +
					(int32_t)(((int64_t)dx * step) / steps);
				int32_t point_y = y0 +
					(int32_t)(((int64_t)dy * step) / steps);
				uint32_t bucket = pgxp_diag_submit_hash(point_x, point_y) &
					(PGXP_DIAG_GL_HASH_BUCKETS - 1u);
				uint32_t point_index = gl_hash_heads[bucket];

				while (point_index != UINT32_MAX && point_index < count)
				{
					uint32_t current_index = point_index;
					const PGXP_diag_gl_vertex* point =
						&gl_vertices[current_index];
					uint32_t point_start = point->triangle_start;
					unsigned point_vertex = point->vertex;
					unsigned linked_vertex = 3u;
					unsigned other_vertex = 3u;
					unsigned linked_count = 0;
					int64_t edge_side;
					int64_t point_side = 0;
					int mixed_side = 0;
					unsigned topology = 0;
					unsigned link_kind;
					int material_same;
					unsigned neighbor;
					float* edge_position0;
					float* edge_position1;
					float* point_position;
					float* linked_position;
					float edge_dx;
					float edge_dy;
					float length_squared;
					float signed_perpendicular;
					float perpendicular;
					float scale;
					float point_t;
					float linked_t;
					float point_target_x;
					float point_target_y;
					float linked_target_x;
					float linked_target_y;
					float point_move;
					float linked_move;
					uint64_t base_pixels = 0;
					uint64_t target_pixels = 0;
					uint64_t gap;

					point_index = gl_hash_next[current_index];
					if (!point->valid || point->native_x != point_x ||
					    point->native_y != point_y ||
					    point_start == edge_start ||
					    point->packet == edge_triangle->packet ||
					    point->upscale_shift != edge_triangle->upscale_shift ||
					    !pgxp_diag_gl_triangle_valid(point_start, count))
						continue;
					gl_repair_matches++;
					edge_side = pgxp_diag_tj_native_cross(x0, y0, x1, y1,
						gl_vertices[edge_start + third].native_x,
						gl_vertices[edge_start + third].native_y);
					for (neighbor = 0; neighbor < 3; neighbor++)
					{
						const PGXP_diag_gl_vertex* candidate;
						int64_t side;
						if (neighbor == point_vertex)
							continue;
						candidate = &gl_vertices[point_start + neighbor];
						if (pgxp_diag_tj_point_on_segment(x0, y0, x1, y1,
							candidate->native_x, candidate->native_y))
						{
							linked_vertex = neighbor;
							linked_count++;
							continue;
						}
						other_vertex = neighbor;
						side = pgxp_diag_tj_native_cross(x0, y0, x1, y1,
							candidate->native_x, candidate->native_y);
						if (!side)
							continue;
						if (!point_side)
							point_side = side;
						else if ((point_side < 0) != (side < 0))
							mixed_side = 1;
					}
					if (linked_count == 1u && other_vertex != 3u)
						topology = !mixed_side && edge_side && point_side &&
							((edge_side < 0) != (point_side < 0)) ? 2u : 1u;
					gl_repair_topology[topology]++;
					if (topology != 2u || !point->textured ||
					    point->semi_transparent)
						continue;

					scale = (float)(1u << edge_triangle->upscale_shift);
					edge_position0 = pgxp_diag_gl_position(vertices,
						stride_bytes, edge_start + first);
					edge_position1 = pgxp_diag_gl_position(vertices,
						stride_bytes, edge_start + second);
					point_position = pgxp_diag_gl_position(vertices,
						stride_bytes, current_index);
					linked_position = pgxp_diag_gl_position(vertices,
						stride_bytes, point_start + linked_vertex);
					edge_dx = (edge_position1[0] - edge_position0[0]) / scale;
					edge_dy = (edge_position1[1] - edge_position0[1]) / scale;
					length_squared = edge_dx * edge_dx + edge_dy * edge_dy;
					if (length_squared <= 1.0e-12f)
						continue;
					signed_perpendicular = (edge_dx *
						((point_position[1] - edge_position0[1]) / scale) -
						edge_dy *
						((point_position[0] - edge_position0[0]) / scale)) /
						sqrtf(length_squared);
					perpendicular = fabsf(signed_perpendicular);
					if ((signed_perpendicular < 0) == (edge_side < 0) ||
					    perpendicular <= (1.0f / 64.0f) ||
					    perpendicular > 4.0f)
						continue;

					gl_repair_candidates++;
					gl_repair_y[point_y < 64 ? 0u : point_y < 128 ? 1u :
						point_y < 192 ? 2u : 3u]++;
					gl_repair_gouraud[(edge_triangle->gouraud ? 2u : 0u) |
						(point->gouraud ? 1u : 0u)]++;
					gap = edge_triangle->packet > point->packet ?
						edge_triangle->packet - point->packet :
						point->packet - edge_triangle->packet;
					gl_repair_packet[pgxp_diag_edge_packet_bin(gap)]++;
					gl_repair_opcode[edge_triangle->opcode & 0x1fu]
						[point->opcode & 0x1fu]++;
					if (edge_triangle->invalid_w || point->invalid_w)
					{
						gl_repair_gate_invalid_w++;
						continue;
					}
					link_kind = point->native_x ==
						gl_vertices[point_start + linked_vertex].native_x &&
						point->native_y ==
						gl_vertices[point_start + linked_vertex].native_y ? 0u :
						((gl_vertices[point_start + linked_vertex].native_x == x0 &&
						  gl_vertices[point_start + linked_vertex].native_y == y0) ||
						 (gl_vertices[point_start + linked_vertex].native_x == x1 &&
						  gl_vertices[point_start + linked_vertex].native_y == y1)) ?
						1u : 2u;
					material_same = point->material_key ==
						edge_triangle->material_key;
					gl_repair_material[material_same ? 1u : 0u]++;
					if (!pgxp_diag_gl_mode_accepts_link(gl_repair_mode, link_kind))
					{
						gl_repair_gate_link_kind++;
						continue;
					}
					if (pgxp_diag_gl_mode_needs_material(gl_repair_mode) &&
					    !material_same)
					{
						gl_repair_gate_material++;
						continue;
					}

					if (gl_repair_mode == PGXP_DIAG_GL_TEST_NATIVE_BOUNDARY)
					{
						point_target_x = point->native_x * scale;
						point_target_y = point->native_y * scale;
						linked_target_x =
							gl_vertices[point_start + linked_vertex].native_x * scale;
						linked_target_y =
							gl_vertices[point_start + linked_vertex].native_y * scale;
					}
					else if (pgxp_diag_gl_mode_is_perpendicular(gl_repair_mode))
					{
						point_t = (edge_dx *
							((point_position[0] - edge_position0[0]) / scale) +
							edge_dy *
							((point_position[1] - edge_position0[1]) / scale)) /
							length_squared;
						linked_t = (edge_dx *
							((linked_position[0] - edge_position0[0]) / scale) +
							edge_dy *
							((linked_position[1] - edge_position0[1]) / scale)) /
							length_squared;
						point_target_x = edge_position0[0] + point_t *
							(edge_position1[0] - edge_position0[0]);
						point_target_y = edge_position0[1] + point_t *
							(edge_position1[1] - edge_position0[1]);
						linked_target_x = edge_position0[0] + linked_t *
							(edge_position1[0] - edge_position0[0]);
						linked_target_y = edge_position0[1] + linked_t *
							(edge_position1[1] - edge_position0[1]);
						if (gl_repair_mode == PGXP_DIAG_GL_TEST_PERP_POINT_CLOSED)
						{
							linked_target_x = linked_position[0];
							linked_target_y = linked_position[1];
						}
					}
					else
					{
						point_t = ((float)(point_x - x0) * dx +
							(float)(point_y - y0) * dy) /
							(float)((int64_t)dx * dx + (int64_t)dy * dy);
						linked_t = ((float)(gl_vertices[point_start + linked_vertex].native_x - x0) * dx +
							(float)(gl_vertices[point_start + linked_vertex].native_y - y0) * dy) /
							(float)((int64_t)dx * dx + (int64_t)dy * dy);
						point_target_x = edge_position0[0] + point_t *
							(edge_position1[0] - edge_position0[0]);
						point_target_y = edge_position0[1] + point_t *
							(edge_position1[1] - edge_position0[1]);
						linked_target_x = edge_position0[0] + linked_t *
							(edge_position1[0] - edge_position0[0]);
						linked_target_y = edge_position0[1] + linked_t *
							(edge_position1[1] - edge_position0[1]);
					}
					point_move = hypotf(point_target_x - point_position[0],
						point_target_y - point_position[1]) / scale;
					linked_move = hypotf(linked_target_x - linked_position[0],
						linked_target_y - linked_position[1]) / scale;
					if (!isfinite(point_move) || !isfinite(linked_move) ||
					    point_move > 4.0f || linked_move > 4.0f)
					{
						gl_repair_gate_movement++;
						continue;
					}
					if (pgxp_diag_gl_mode_needs_raster(gl_repair_mode))
					{
						int raster_result = pgxp_diag_gl_raster_gate(vertices,
							stride_bytes, edge_start, first, second, point_start,
							point_vertex, linked_vertex, point_target_x,
							point_target_y, linked_target_x, linked_target_y,
							gl_repair_mode, &base_pixels, &target_pixels);
						if (!raster_result)
						{
							gl_repair_gate_raster++;
							continue;
						}
						gl_repair_raster_tested++;
						gl_repair_raster_base_pixels += base_pixels;
						gl_repair_raster_target_pixels += target_pixels;
						if ((pgxp_diag_gl_mode_is_closed(gl_repair_mode) &&
						     (!base_pixels || target_pixels)) ||
						    (!pgxp_diag_gl_mode_is_closed(gl_repair_mode) &&
						     base_pixels <= target_pixels))
						{
							gl_repair_gate_raster++;
							continue;
						}
					}
					if (pair_count >= PGXP_DIAG_GL_PAIR_CAPACITY)
					{
						pair_overflow = 1;
						continue;
					}

					pgxp_diag_gl_add_proposal(current_index, point_target_x,
						point_target_y, scale / 64.0f);
					pgxp_diag_gl_add_proposal(point_start + linked_vertex,
						linked_target_x, linked_target_y, scale / 64.0f);
					gl_pairs[pair_count].point = current_index;
					gl_pairs[pair_count].linked = point_start + linked_vertex;
					pair_count++;
					gl_repair_proposals += 2;
					gl_repair_accepted++;
					gl_repair_accepted_link[link_kind]++;

					if (log_cb &&
					    gl_repair_samples < PGXP_DIAG_SUBMIT_SAMPLES &&
					    gl_repair_window_samples < PGXP_DIAG_GL_WINDOW_SAMPLES)
					{
						log_cb(RETRO_LOG_INFO,
							"[pgxp_gl_repair_candidate] n=%u mf=%u "
							"edge=%llu/%02x point=%llu/%02x "
							"native=%d/%d-%d/%d point=%d/%d linked=%d/%d "
							"point_xy=%.5f/%.5f->%.5f/%.5f "
							"linked_xy=%.5f/%.5f->%.5f/%.5f "
							"perp=%.6f move=%.6f/%.6f scale=%.0f gap=%llu "
							"mode=%u kind=%u material=%u pixels=%llu/%llu\n",
							gl_repair_samples + 1, mode_frame,
							(unsigned long long)edge_triangle->packet,
							edge_triangle->opcode,
							(unsigned long long)point->packet, point->opcode,
							x0, y0, x1, y1, point_x, point_y,
							gl_vertices[point_start + linked_vertex].native_x,
							gl_vertices[point_start + linked_vertex].native_y,
							point_position[0], point_position[1],
							point_target_x, point_target_y,
							linked_position[0], linked_position[1],
							linked_target_x, linked_target_y,
							perpendicular, point_move, linked_move, scale,
							(unsigned long long)gap, gl_repair_mode, link_kind,
							material_same,
							(unsigned long long)base_pixels,
							(unsigned long long)target_pixels);
						gl_repair_samples++;
						gl_repair_window_samples++;
					}
				}
			}
		}
	}

	if (pair_overflow)
	{
		gl_repair_pair_overflow++;
		pgxp_diag_gl_reset_stream();
		return;
	}
	/* A conflict at either end invalidates its whole boundary.  Propagate that
	 * decision through shared pairs before applying anything, so an ambiguous
	 * junction cannot leave a half-repaired short edge behind. */
	{
		int changed;
		do
		{
			changed = 0;
			for (edge_start = 0; edge_start < pair_count; edge_start++)
			{
				PGXP_diag_gl_proposal* point =
					&gl_proposals[gl_pairs[edge_start].point];
				PGXP_diag_gl_proposal* linked =
					&gl_proposals[gl_pairs[edge_start].linked];
				if (point->conflict != linked->conflict)
				{
					point->conflict = 1;
					linked->conflict = 1;
					changed = 1;
				}
			}
		} while (changed);
	}
	for (edge_start = 0; edge_start < count; edge_start++)
		if (gl_proposals[edge_start].count)
		{
			if (gl_proposals[edge_start].conflict)
				gl_repair_conflicts++;
			else
				gl_repair_consistent++;
		}
	for (edge_start = 0; edge_start < pair_count; edge_start++)
	{
		PGXP_diag_gl_proposal* point =
			&gl_proposals[gl_pairs[edge_start].point];
		PGXP_diag_gl_proposal* linked =
			&gl_proposals[gl_pairs[edge_start].linked];
		if (!point->conflict && !linked->conflict)
		{
			point->apply = 1;
			linked->apply = 1;
			gl_repair_atomic_pairs++;
		}
	}
	for (edge_start = 0; edge_start < count; edge_start++)
		if (gl_proposals[edge_start].apply)
		{
			float* position = pgxp_diag_gl_position(vertices, stride_bytes,
				edge_start);
			float scale = (float)(1u << gl_vertices[edge_start].upscale_shift);
			float movement = hypotf(gl_proposals[edge_start].x - position[0],
				gl_proposals[edge_start].y - position[1]) / scale;
			if (movement > 1.0e-6f)
			{
				gl_repair_moved++;
				gl_repair_move_bins[pgxp_diag_edge_delta_bin(movement)]++;
				gl_repair_move_sum += movement;
				if (movement > gl_repair_move_max)
					gl_repair_move_max = movement;
				if (gl_repair_mode != PGXP_DIAG_GL_TEST_OFF)
				{
					position[0] = gl_proposals[edge_start].x;
					position[1] = gl_proposals[edge_start].y;
					gl_repair_applied++;
				}
			}
		}

	pgxp_diag_gl_reset_stream();
}

void PGXP_DiagGLRasterCaps(unsigned subpixel_bits)
{
	submit_gl_subpixel_bits = subpixel_bits > 16u ? 16u : subpixel_bits;
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
	pgxp_diag_tj_observe(vertices, textured, gouraud, invalid_w,
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

	/* Primitive observation is frame-local.  Complete the prior frame before
	 * advancing mode_frame so packet ages and the 60-frame aggregate align. */
	pgxp_diag_tj_finish_frame();
	pgxp_diag_submit_finish_frame();
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
		"handoff=unaltered analysis=frame_complete_tjunction\n",
		(unsigned long long)frame_number,
		(unsigned long long)edge_observations[0],
		(unsigned long long)edge_observations[1],
		(unsigned long long)edge_compares[0],
		(unsigned long long)edge_compares[1],
		(unsigned long long)edge_compares[2],
		(unsigned long long)edge_table_overflow, edge_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_tjunction_summary] f=%llu edges=%llu lattice=%llu "
		"long=%llu interior_points=%llu matches=%llu/%llu/%llu "
		"risk=%llu/%llu/%llu outside=%llu degenerate=%llu "
		"overflow=%llu/%llu evictions=%llu samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)tj_edges_recorded,
		(unsigned long long)tj_lattice_edges,
		(unsigned long long)tj_long_edges,
		(unsigned long long)tj_interior_points,
		(unsigned long long)tj_matches[0],
		(unsigned long long)tj_matches[1],
		(unsigned long long)tj_matches[2],
		(unsigned long long)tj_risk[0],
		(unsigned long long)tj_risk[1],
		(unsigned long long)tj_risk[2],
		(unsigned long long)tj_projected_outside,
		(unsigned long long)tj_degenerate_precise_edge,
		(unsigned long long)tj_vertex_overflow,
		(unsigned long long)tj_edge_overflow,
		(unsigned long long)tj_observation_evictions, tj_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_tjunction_context] f=%llu invalid_w=%llu/%llu/%llu/%llu "
		"gouraud=%llu/%llu/%llu/%llu semi=%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)tj_context_invalid_w[0],
		(unsigned long long)tj_context_invalid_w[1],
		(unsigned long long)tj_context_invalid_w[2],
		(unsigned long long)tj_context_invalid_w[3],
		(unsigned long long)tj_context_gouraud[0],
		(unsigned long long)tj_context_gouraud[1],
		(unsigned long long)tj_context_gouraud[2],
		(unsigned long long)tj_context_gouraud[3],
		(unsigned long long)tj_context_semi[0],
		(unsigned long long)tj_context_semi[1],
		(unsigned long long)tj_context_semi[2],
		(unsigned long long)tj_context_semi[3]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_tjunction_risk] f=%llu invalid_w=%llu/%llu/%llu/%llu "
		"gouraud=%llu/%llu/%llu/%llu packet=%llu/%llu/%llu/%llu/%llu/%llu "
		"y=%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)tj_risk_invalid_w[0],
		(unsigned long long)tj_risk_invalid_w[1],
		(unsigned long long)tj_risk_invalid_w[2],
		(unsigned long long)tj_risk_invalid_w[3],
		(unsigned long long)tj_risk_gouraud[0],
		(unsigned long long)tj_risk_gouraud[1],
		(unsigned long long)tj_risk_gouraud[2],
		(unsigned long long)tj_risk_gouraud[3],
		(unsigned long long)tj_risk_packet[0],
		(unsigned long long)tj_risk_packet[1],
		(unsigned long long)tj_risk_packet[2],
		(unsigned long long)tj_risk_packet[3],
		(unsigned long long)tj_risk_packet[4],
		(unsigned long long)tj_risk_packet[5],
		(unsigned long long)tj_risk_y[0],
		(unsigned long long)tj_risk_y[1],
		(unsigned long long)tj_risk_y[2],
		(unsigned long long)tj_risk_y[3]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_submit_summary] f=%llu primitives=%llu quads=%llu "
		"triangles=%llu overflow=%llu "
		"transport=calls/vertices/mismatch_calls/mismatch_vertices/orphans="
		"%llu/%llu/%llu/%llu/%llu "
		"edges=%llu lattice=%llu long=%llu "
		"interior_points=%llu matches=%llu topology=%llu/%llu/%llu "
		"risk=%llu subpixel=%u handoff=final_gl_draw_arrays\n",
		(unsigned long long)frame_number,
		(unsigned long long)submit_primitives,
		(unsigned long long)submit_quads,
		(unsigned long long)(submit_primitives + submit_quads),
		(unsigned long long)submit_triangle_overflow,
		(unsigned long long)submit_transport_calls,
		(unsigned long long)submit_transport_vertices,
		(unsigned long long)submit_transport_mismatch_calls,
		(unsigned long long)submit_transport_mismatch_vertices,
		(unsigned long long)submit_transport_orphans,
		(unsigned long long)submit_edges,
		(unsigned long long)submit_lattice_edges,
		(unsigned long long)submit_long_edges,
		(unsigned long long)submit_interior_points,
		(unsigned long long)submit_matches,
		(unsigned long long)submit_topology[0],
		(unsigned long long)submit_topology[1],
		(unsigned long long)submit_topology[2],
		(unsigned long long)submit_risk, submit_gl_subpixel_bits);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_submit_risk] f=%llu y=%llu/%llu/%llu/%llu "
		"gouraud=%llu/%llu/%llu/%llu "
		"packet=%llu/%llu/%llu/%llu/%llu/%llu\n",
		(unsigned long long)frame_number,
		(unsigned long long)submit_risk_y[0],
		(unsigned long long)submit_risk_y[1],
		(unsigned long long)submit_risk_y[2],
		(unsigned long long)submit_risk_y[3],
		(unsigned long long)submit_risk_gouraud[0],
		(unsigned long long)submit_risk_gouraud[1],
		(unsigned long long)submit_risk_gouraud[2],
		(unsigned long long)submit_risk_gouraud[3],
		(unsigned long long)submit_risk_packet[0],
		(unsigned long long)submit_risk_packet[1],
		(unsigned long long)submit_risk_packet[2],
		(unsigned long long)submit_risk_packet[3],
		(unsigned long long)submit_risk_packet[4],
		(unsigned long long)submit_risk_packet[5]);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_submit_raster] f=%llu tested_pairs=%llu native_pixels=%llu "
		"hole_pairs=raw/glnear/glfloor/high8/snap=%llu/%llu/%llu/%llu/%llu "
		"hole_pixels=raw/glnear/glfloor/high8/snap=%llu/%llu/%llu/%llu/%llu "
		"snap_improved=%llu snap_closed=%llu bbox_pixels=%llu skips=%llu "
		"degenerate=%llu samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)submit_raster_pairs,
		(unsigned long long)submit_raster_native_pixels,
		(unsigned long long)submit_raster_hole_pairs[0],
		(unsigned long long)submit_raster_hole_pairs[1],
		(unsigned long long)submit_raster_hole_pairs[2],
		(unsigned long long)submit_raster_hole_pairs[3],
		(unsigned long long)submit_raster_hole_pairs[4],
		(unsigned long long)submit_raster_hole_pixels[0],
		(unsigned long long)submit_raster_hole_pixels[1],
		(unsigned long long)submit_raster_hole_pixels[2],
		(unsigned long long)submit_raster_hole_pixels[3],
		(unsigned long long)submit_raster_hole_pixels[4],
		(unsigned long long)submit_raster_snap_improved,
		(unsigned long long)submit_raster_snap_closed,
		(unsigned long long)submit_raster_bbox_pixels,
		(unsigned long long)submit_raster_bbox_skips,
		(unsigned long long)submit_raster_degenerate, submit_samples);
	{
		static const char* const link_name[PGXP_DIAG_SUBMIT_LINK_KINDS] = {
			"same_native", "endpoint", "interior"
		};
		unsigned kind;
		for (kind = 0; kind < PGXP_DIAG_SUBMIT_LINK_KINDS; kind++)
		{
			log_cb(RETRO_LOG_INFO,
				"[pgxp_submit_link] f=%llu kind=%s "
				"candidate=%llu/%llu/%llu/%llu/%llu/%llu "
				"tested=%llu/%llu/%llu/%llu/%llu/%llu "
				"improved=%llu/%llu/%llu/%llu/%llu/%llu "
				"closed=%llu/%llu/%llu/%llu/%llu/%llu "
				"worse=%llu/%llu/%llu/%llu/%llu/%llu\n",
				(unsigned long long)frame_number, link_name[kind],
				(unsigned long long)submit_link_candidates[kind][0],
				(unsigned long long)submit_link_candidates[kind][1],
				(unsigned long long)submit_link_candidates[kind][2],
				(unsigned long long)submit_link_candidates[kind][3],
				(unsigned long long)submit_link_candidates[kind][4],
				(unsigned long long)submit_link_candidates[kind][5],
				(unsigned long long)submit_link_tested[kind][0],
				(unsigned long long)submit_link_tested[kind][1],
				(unsigned long long)submit_link_tested[kind][2],
				(unsigned long long)submit_link_tested[kind][3],
				(unsigned long long)submit_link_tested[kind][4],
				(unsigned long long)submit_link_tested[kind][5],
				(unsigned long long)submit_link_improved[kind][0],
				(unsigned long long)submit_link_improved[kind][1],
				(unsigned long long)submit_link_improved[kind][2],
				(unsigned long long)submit_link_improved[kind][3],
				(unsigned long long)submit_link_improved[kind][4],
				(unsigned long long)submit_link_improved[kind][5],
				(unsigned long long)submit_link_closed[kind][0],
				(unsigned long long)submit_link_closed[kind][1],
				(unsigned long long)submit_link_closed[kind][2],
				(unsigned long long)submit_link_closed[kind][3],
				(unsigned long long)submit_link_closed[kind][4],
				(unsigned long long)submit_link_closed[kind][5],
				(unsigned long long)submit_link_worse[kind][0],
				(unsigned long long)submit_link_worse[kind][1],
				(unsigned long long)submit_link_worse[kind][2],
				(unsigned long long)submit_link_worse[kind][3],
				(unsigned long long)submit_link_worse[kind][4],
				(unsigned long long)submit_link_worse[kind][5]);
			log_cb(RETRO_LOG_INFO,
				"[pgxp_submit_link_pixels] f=%llu kind=%s "
				"base=%llu/%llu/%llu/%llu/%llu/%llu "
				"snap=%llu/%llu/%llu/%llu/%llu/%llu "
				"invw_error=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"uv_error=%llu/%llu/%llu/%llu/%llu/%llu\n",
				(unsigned long long)frame_number, link_name[kind],
				(unsigned long long)submit_link_raw_pixels[kind][0],
				(unsigned long long)submit_link_raw_pixels[kind][1],
				(unsigned long long)submit_link_raw_pixels[kind][2],
				(unsigned long long)submit_link_raw_pixels[kind][3],
				(unsigned long long)submit_link_raw_pixels[kind][4],
				(unsigned long long)submit_link_raw_pixels[kind][5],
				(unsigned long long)submit_link_snap_pixels[kind][0],
				(unsigned long long)submit_link_snap_pixels[kind][1],
				(unsigned long long)submit_link_snap_pixels[kind][2],
				(unsigned long long)submit_link_snap_pixels[kind][3],
				(unsigned long long)submit_link_snap_pixels[kind][4],
				(unsigned long long)submit_link_snap_pixels[kind][5],
				(unsigned long long)submit_link_w_error[kind][0],
				(unsigned long long)submit_link_w_error[kind][1],
				(unsigned long long)submit_link_w_error[kind][2],
				(unsigned long long)submit_link_w_error[kind][3],
				(unsigned long long)submit_link_w_error[kind][4],
				(unsigned long long)submit_link_w_error[kind][5],
				(unsigned long long)submit_link_w_error[kind][6],
				(unsigned long long)submit_link_uv_error[kind][0],
				(unsigned long long)submit_link_uv_error[kind][1],
				(unsigned long long)submit_link_uv_error[kind][2],
				(unsigned long long)submit_link_uv_error[kind][3],
				(unsigned long long)submit_link_uv_error[kind][4],
				(unsigned long long)submit_link_uv_error[kind][5]);
		}
	}
	log_cb(RETRO_LOG_INFO,
		"[pgxp_gl_repair] f=%llu buffers=%llu vertices=%llu "
		"metadata=mismatch/overflow/inactive=%llu/%llu/%llu "
		"triangles=%llu edges=%llu lattice=%llu long=%llu "
		"interior_points=%llu matches=%llu topology=%llu/%llu/%llu "
		"candidates=%llu gate=invalid_w/movement/pair_overflow=%llu/%llu/%llu "
		"proposals=%llu consistent=%llu conflicts=%llu "
		"atomic_pairs=%llu would_move=%llu applied=%llu mean=%.6f max=%.6f "
		"mode=%u name=%s mask=%016llx apply=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)gl_repair_buffers,
		(unsigned long long)gl_repair_vertices,
		(unsigned long long)gl_repair_metadata_mismatch,
		(unsigned long long)gl_repair_metadata_overflow,
		(unsigned long long)gl_repair_inactive,
		(unsigned long long)gl_repair_triangles,
		(unsigned long long)gl_repair_edges,
		(unsigned long long)gl_repair_lattice_edges,
		(unsigned long long)gl_repair_long_edges,
		(unsigned long long)gl_repair_interior_points,
		(unsigned long long)gl_repair_matches,
		(unsigned long long)gl_repair_topology[0],
		(unsigned long long)gl_repair_topology[1],
		(unsigned long long)gl_repair_topology[2],
		(unsigned long long)gl_repair_candidates,
		(unsigned long long)gl_repair_gate_invalid_w,
		(unsigned long long)gl_repair_gate_movement,
		(unsigned long long)gl_repair_pair_overflow,
		(unsigned long long)gl_repair_proposals,
		(unsigned long long)gl_repair_consistent,
		(unsigned long long)gl_repair_conflicts,
		(unsigned long long)gl_repair_atomic_pairs,
		(unsigned long long)gl_repair_moved,
		(unsigned long long)gl_repair_applied,
		gl_repair_moved ? gl_repair_move_sum /
			(double)gl_repair_moved : 0.0,
		gl_repair_move_max, gl_repair_mode,
		pgxp_diag_gl_mode_name(gl_repair_mode),
		(unsigned long long)gl_repair_mode_mask,
		gl_repair_mode > PGXP_DIAG_GL_TEST_OFF &&
		(gl_repair_mode <= PGXP_DIAG_GL_TEST_PERP_CLOSED_MATERIAL ||
		 pgxp_diag_gl_mode_is_native_handoff(gl_repair_mode)));
	log_cb(RETRO_LOG_INFO,
		"[pgxp_gl_native_handoff] f=%llu mode=%u name=%s "
		"triangles=%llu class=FT/FQ/GT/GQ:%llu/%llu/%llu/%llu "
		"selected=%llu class=%llu/%llu/%llu/%llu "
		"w=valid/invalid:%llu/%llu selected_w=%llu/%llu "
		"vertices=%llu moved=%llu axis=x/y:%llu/%llu "
		"delta=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
		"mean=%.6f max=%.6f samples=%u\n",
		(unsigned long long)frame_number, gl_repair_mode,
		pgxp_diag_gl_mode_name(gl_repair_mode),
		(unsigned long long)(gl_native_triangles[0] +
			gl_native_triangles[1] + gl_native_triangles[2] +
			gl_native_triangles[3]),
		(unsigned long long)gl_native_triangles[0],
		(unsigned long long)gl_native_triangles[1],
		(unsigned long long)gl_native_triangles[2],
		(unsigned long long)gl_native_triangles[3],
		(unsigned long long)(gl_native_selected[0] +
			gl_native_selected[1] + gl_native_selected[2] +
			gl_native_selected[3]),
		(unsigned long long)gl_native_selected[0],
		(unsigned long long)gl_native_selected[1],
		(unsigned long long)gl_native_selected[2],
		(unsigned long long)gl_native_selected[3],
		(unsigned long long)gl_native_w[0],
		(unsigned long long)gl_native_w[1],
		(unsigned long long)gl_native_selected_w[0],
		(unsigned long long)gl_native_selected_w[1],
		(unsigned long long)gl_native_vertices,
		(unsigned long long)gl_native_moved,
		(unsigned long long)gl_native_axis_moved[0],
		(unsigned long long)gl_native_axis_moved[1],
		(unsigned long long)gl_native_delta_bins[0],
		(unsigned long long)gl_native_delta_bins[1],
		(unsigned long long)gl_native_delta_bins[2],
		(unsigned long long)gl_native_delta_bins[3],
		(unsigned long long)gl_native_delta_bins[4],
		(unsigned long long)gl_native_delta_bins[5],
		(unsigned long long)gl_native_delta_bins[6],
		gl_native_moved ? gl_native_move_sum /
			(double)gl_native_moved : 0.0,
		gl_native_move_max, gl_native_samples);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_gl_repair_gate] f=%llu mode=%u "
		"rejected=link/material/raster=%llu/%llu/%llu "
		"material=different/same=%llu/%llu tested=%llu accepted=%llu "
		"accepted_link=same/endpoint/interior=%llu/%llu/%llu "
		"pixels=base/target=%llu/%llu\n",
		(unsigned long long)frame_number, gl_repair_mode,
		(unsigned long long)gl_repair_gate_link_kind,
		(unsigned long long)gl_repair_gate_material,
		(unsigned long long)gl_repair_gate_raster,
		(unsigned long long)gl_repair_material[0],
		(unsigned long long)gl_repair_material[1],
		(unsigned long long)gl_repair_raster_tested,
		(unsigned long long)gl_repair_accepted,
		(unsigned long long)gl_repair_accepted_link[0],
		(unsigned long long)gl_repair_accepted_link[1],
		(unsigned long long)gl_repair_accepted_link[2],
		(unsigned long long)gl_repair_raster_base_pixels,
		(unsigned long long)gl_repair_raster_target_pixels);
	log_cb(RETRO_LOG_INFO,
		"[pgxp_gl_repair_context] f=%llu "
		"y=%llu/%llu/%llu/%llu gouraud=%llu/%llu/%llu/%llu "
		"packet=%llu/%llu/%llu/%llu/%llu/%llu "
		"move=%llu/%llu/%llu/%llu/%llu/%llu/%llu samples=%u\n",
		(unsigned long long)frame_number,
		(unsigned long long)gl_repair_y[0],
		(unsigned long long)gl_repair_y[1],
		(unsigned long long)gl_repair_y[2],
		(unsigned long long)gl_repair_y[3],
		(unsigned long long)gl_repair_gouraud[0],
		(unsigned long long)gl_repair_gouraud[1],
		(unsigned long long)gl_repair_gouraud[2],
		(unsigned long long)gl_repair_gouraud[3],
		(unsigned long long)gl_repair_packet[0],
		(unsigned long long)gl_repair_packet[1],
		(unsigned long long)gl_repair_packet[2],
		(unsigned long long)gl_repair_packet[3],
		(unsigned long long)gl_repair_packet[4],
		(unsigned long long)gl_repair_packet[5],
		(unsigned long long)gl_repair_move_bins[0],
		(unsigned long long)gl_repair_move_bins[1],
		(unsigned long long)gl_repair_move_bins[2],
		(unsigned long long)gl_repair_move_bins[3],
		(unsigned long long)gl_repair_move_bins[4],
		(unsigned long long)gl_repair_move_bins[5],
		(unsigned long long)gl_repair_move_bins[6],
		gl_repair_samples);
	{
		uint8_t selected_edge[4];
		uint8_t selected_point[4];
		unsigned rank;
		memset(selected_edge, 0xff, sizeof(selected_edge));
		memset(selected_point, 0xff, sizeof(selected_point));
		for (rank = 0; rank < 4; rank++)
		{
			uint64_t best = 0;
			unsigned best_edge = 0;
			unsigned best_point = 0;
			unsigned edge_opcode;
			unsigned point_opcode;
			for (edge_opcode = 0; edge_opcode < 32; edge_opcode++)
				for (point_opcode = 0; point_opcode < 32; point_opcode++)
				{
					unsigned previous;
					int used = 0;
					for (previous = 0; previous < rank; previous++)
						if (selected_edge[previous] == edge_opcode &&
						    selected_point[previous] == point_opcode)
							used = 1;
					if (!used &&
					    gl_repair_opcode[edge_opcode][point_opcode] > best)
					{
						best = gl_repair_opcode[edge_opcode][point_opcode];
						best_edge = edge_opcode;
						best_point = point_opcode;
					}
				}
			if (!best)
				break;
			selected_edge[rank] = (uint8_t)best_edge;
			selected_point[rank] = (uint8_t)best_point;
			log_cb(RETRO_LOG_INFO,
				"[pgxp_gl_repair_opcode] f=%llu rank=%u "
				"edge=%02x point=%02x count=%llu\n",
				(unsigned long long)frame_number, rank + 1,
				0x20u | best_edge, 0x20u | best_point,
				(unsigned long long)best);
		}
	}
	{
		uint8_t selected_edge[8];
		uint8_t selected_point[8];
		unsigned rank;
		memset(selected_edge, 0xff, sizeof(selected_edge));
		memset(selected_point, 0xff, sizeof(selected_point));
		for (rank = 0; rank < 8; rank++)
		{
			uint64_t best = 0;
			unsigned best_edge = 0;
			unsigned best_point = 0;
			unsigned edge_opcode;
			unsigned point_opcode;
			for (edge_opcode = 0; edge_opcode < 32; edge_opcode++)
				for (point_opcode = 0; point_opcode < 32; point_opcode++)
				{
					unsigned previous;
					int used = 0;
					for (previous = 0; previous < rank; previous++)
						if (selected_edge[previous] == edge_opcode &&
						    selected_point[previous] == point_opcode)
							used = 1;
					if (!used && submit_risk_opcode[edge_opcode][point_opcode] > best)
					{
						best = submit_risk_opcode[edge_opcode][point_opcode];
						best_edge = edge_opcode;
						best_point = point_opcode;
					}
				}
			if (!best)
				break;
			selected_edge[rank] = (uint8_t)best_edge;
			selected_point[rank] = (uint8_t)best_point;
			log_cb(RETRO_LOG_INFO,
				"[pgxp_submit_opcode] f=%llu rank=%u edge=%02x point=%02x "
				"count=%llu\n", (unsigned long long)frame_number, rank + 1,
				0x20u | best_edge, 0x20u | best_point,
				(unsigned long long)best);
		}
	}
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
			log_cb(RETRO_LOG_INFO,
				"[pgxp_tjunction_kind] f=%llu kind=%s "
				"topology=%llu/%llu/%llu "
				"perp=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"predicted=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"packet=%llu/%llu/%llu/%llu/%llu/%llu "
				"y=%llu/%llu/%llu/%llu "
				"steps=%llu/%llu/%llu/%llu/%llu/%llu/%llu "
				"side=on/toward_edge/toward_neighbor=%llu/%llu/%llu risk=%llu\n",
				(unsigned long long)frame_number, edge_kind_name[kind],
				(unsigned long long)tj_topology[kind][0],
				(unsigned long long)tj_topology[kind][1],
				(unsigned long long)tj_topology[kind][2],
				(unsigned long long)tj_perp_bins[kind][0],
				(unsigned long long)tj_perp_bins[kind][1],
				(unsigned long long)tj_perp_bins[kind][2],
				(unsigned long long)tj_perp_bins[kind][3],
				(unsigned long long)tj_perp_bins[kind][4],
				(unsigned long long)tj_perp_bins[kind][5],
				(unsigned long long)tj_perp_bins[kind][6],
				(unsigned long long)tj_predicted_bins[kind][0],
				(unsigned long long)tj_predicted_bins[kind][1],
				(unsigned long long)tj_predicted_bins[kind][2],
				(unsigned long long)tj_predicted_bins[kind][3],
				(unsigned long long)tj_predicted_bins[kind][4],
				(unsigned long long)tj_predicted_bins[kind][5],
				(unsigned long long)tj_predicted_bins[kind][6],
				(unsigned long long)tj_packet_bins[kind][0],
				(unsigned long long)tj_packet_bins[kind][1],
				(unsigned long long)tj_packet_bins[kind][2],
				(unsigned long long)tj_packet_bins[kind][3],
				(unsigned long long)tj_packet_bins[kind][4],
				(unsigned long long)tj_packet_bins[kind][5],
				(unsigned long long)tj_y_band[kind][0],
				(unsigned long long)tj_y_band[kind][1],
				(unsigned long long)tj_y_band[kind][2],
				(unsigned long long)tj_y_band[kind][3],
				(unsigned long long)tj_step_bins[kind][0],
				(unsigned long long)tj_step_bins[kind][1],
				(unsigned long long)tj_step_bins[kind][2],
				(unsigned long long)tj_step_bins[kind][3],
				(unsigned long long)tj_step_bins[kind][4],
				(unsigned long long)tj_step_bins[kind][5],
				(unsigned long long)tj_step_bins[kind][6],
				(unsigned long long)tj_offset_side[kind][0],
				(unsigned long long)tj_offset_side[kind][1],
				(unsigned long long)tj_offset_side[kind][2],
				(unsigned long long)tj_risk[kind]);
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
	tj_edges_recorded = 0;
	tj_lattice_edges = 0;
	tj_long_edges = 0;
	tj_interior_points = 0;
	memset(tj_matches, 0, sizeof(tj_matches));
	memset(tj_topology, 0, sizeof(tj_topology));
	memset(tj_perp_bins, 0, sizeof(tj_perp_bins));
	memset(tj_predicted_bins, 0, sizeof(tj_predicted_bins));
	memset(tj_packet_bins, 0, sizeof(tj_packet_bins));
	memset(tj_y_band, 0, sizeof(tj_y_band));
	memset(tj_step_bins, 0, sizeof(tj_step_bins));
	memset(tj_risk, 0, sizeof(tj_risk));
	memset(tj_offset_side, 0, sizeof(tj_offset_side));
	memset(tj_risk_invalid_w, 0, sizeof(tj_risk_invalid_w));
	memset(tj_risk_gouraud, 0, sizeof(tj_risk_gouraud));
	memset(tj_risk_packet, 0, sizeof(tj_risk_packet));
	memset(tj_risk_y, 0, sizeof(tj_risk_y));
	memset(tj_context_invalid_w, 0, sizeof(tj_context_invalid_w));
	memset(tj_context_gouraud, 0, sizeof(tj_context_gouraud));
	memset(tj_context_semi, 0, sizeof(tj_context_semi));
	tj_projected_outside = 0;
	tj_degenerate_precise_edge = 0;
	tj_vertex_overflow = 0;
	tj_edge_overflow = 0;
	tj_observation_evictions = 0;
	tj_window_samples = 0;
	submit_primitives = 0;
	submit_quads = 0;
	submit_triangle_overflow = 0;
	submit_transport_calls = 0;
	submit_transport_vertices = 0;
	submit_transport_mismatch_calls = 0;
	submit_transport_mismatch_vertices = 0;
	submit_transport_orphans = 0;
	submit_edges = 0;
	submit_lattice_edges = 0;
	submit_long_edges = 0;
	submit_interior_points = 0;
	submit_matches = 0;
	memset(submit_topology, 0, sizeof(submit_topology));
	submit_risk = 0;
	memset(submit_risk_y, 0, sizeof(submit_risk_y));
	memset(submit_risk_packet, 0, sizeof(submit_risk_packet));
	memset(submit_risk_gouraud, 0, sizeof(submit_risk_gouraud));
	memset(submit_risk_opcode, 0, sizeof(submit_risk_opcode));
	submit_raster_pairs = 0;
	submit_raster_native_pixels = 0;
	memset(submit_raster_hole_pairs, 0, sizeof(submit_raster_hole_pairs));
	memset(submit_raster_hole_pixels, 0, sizeof(submit_raster_hole_pixels));
	submit_raster_snap_improved = 0;
	submit_raster_snap_closed = 0;
	submit_raster_bbox_pixels = 0;
	submit_raster_bbox_skips = 0;
	submit_raster_degenerate = 0;
	memset(submit_link_candidates, 0, sizeof(submit_link_candidates));
	memset(submit_link_tested, 0, sizeof(submit_link_tested));
	memset(submit_link_improved, 0, sizeof(submit_link_improved));
	memset(submit_link_closed, 0, sizeof(submit_link_closed));
	memset(submit_link_worse, 0, sizeof(submit_link_worse));
	memset(submit_link_raw_pixels, 0, sizeof(submit_link_raw_pixels));
	memset(submit_link_snap_pixels, 0, sizeof(submit_link_snap_pixels));
	memset(submit_link_w_error, 0, sizeof(submit_link_w_error));
	memset(submit_link_uv_error, 0, sizeof(submit_link_uv_error));
	submit_window_samples = 0;
	pgxp_diag_gl_reset_window();
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

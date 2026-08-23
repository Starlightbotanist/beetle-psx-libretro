#ifndef _PGXP_DIAG_H_
#define _PGXP_DIAG_H_

#include "pgxp_types.h"

enum PGXP_diag_vertex_source
{
	PGXP_DIAG_VERTEX_TRACKED = 0,
	PGXP_DIAG_VERTEX_CACHE,
	PGXP_DIAG_VERTEX_NATIVE
};

enum PGXP_diag_trace_stage
{
	PGXP_TRACE_NONE = 0,
	PGXP_TRACE_GTE,
	PGXP_TRACE_MFC2,
	PGXP_TRACE_SLL5,
	PGXP_TRACE_SRA5,
	PGXP_TRACE_MEMORY,
	PGXP_TRACE_FIFO,
	PGXP_TRACE_CB,
	PGXP_TRACE_VERTEX
};

/* Runtime-selectable OpenGL seam experiments.  These are exposed only by
 * PGXP_DIAG builds; normal builds compile PGXP_DiagGLSetMode() away. */
enum PGXP_diag_gl_test_mode
{
	PGXP_DIAG_GL_TEST_OFF = 0,
	PGXP_DIAG_GL_TEST_NATIVE_BOUNDARY,
	PGXP_DIAG_GL_TEST_BROAD_REPLAY,
	PGXP_DIAG_GL_TEST_NATIVE_T_IMPROVED,
	PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED,
	PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_ENDPOINT,
	PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_INTERIOR,
	PGXP_DIAG_GL_TEST_NATIVE_T_CLOSED_MATERIAL,
	PGXP_DIAG_GL_TEST_PERP_IMPROVED,
	PGXP_DIAG_GL_TEST_PERP_CLOSED,
	PGXP_DIAG_GL_TEST_PERP_CLOSED_ENDPOINT,
	PGXP_DIAG_GL_TEST_PERP_CLOSED_INTERIOR,
	PGXP_DIAG_GL_TEST_PERP_POINT_CLOSED,
	PGXP_DIAG_GL_TEST_PERP_IMPROVED_MATERIAL,
	PGXP_DIAG_GL_TEST_PERP_CLOSED_MATERIAL,
	/* Backend-parity probes.  Unlike modes 1..14 these never alter the
	 * submitted vertex stream; rhi_lib_gl.c applies the selected raster
	 * transform uniformly in the vertex shader/fixed-function state. */
	PGXP_DIAG_GL_TEST_SWAN_Y_POS,
	PGXP_DIAG_GL_TEST_SWAN_Y_NEG,
	PGXP_DIAG_GL_TEST_SUBPIXEL_NEAREST,
	PGXP_DIAG_GL_TEST_SUBPIXEL_FLOOR,
	PGXP_DIAG_GL_TEST_SUBPIXEL_Y_PHASE,
	PGXP_DIAG_GL_TEST_UPPER_LEFT,
	PGXP_DIAG_GL_TEST_UPPER_LEFT_SWAN,
	PGXP_DIAG_GL_TEST_UPPER_LEFT_NEAREST,
	/* Known-positive native-coverage control and categorical splits.  These
	 * operate only on final opaque-textured GL triangles whose PGXP/native
	 * sidecar transport matched exactly. */
	PGXP_DIAG_GL_TEST_NATIVE_OT_ALL,
	PGXP_DIAG_GL_TEST_NATIVE_OT_FLAT_TRI,
	PGXP_DIAG_GL_TEST_NATIVE_OT_FLAT_QUAD,
	PGXP_DIAG_GL_TEST_NATIVE_OT_GOURAUD_TRI,
	PGXP_DIAG_GL_TEST_NATIVE_OT_GOURAUD_QUAD,
	PGXP_DIAG_GL_TEST_NATIVE_OT_X_ONLY,
	PGXP_DIAG_GL_TEST_NATIVE_OT_Y_ONLY,
	PGXP_DIAG_GL_TEST_NATIVE_OT_DELTA_SMALL,
	PGXP_DIAG_GL_TEST_NATIVE_OT_DELTA_LARGE,
	PGXP_DIAG_GL_TEST_NATIVE_OT_VALID_W,
	PGXP_DIAG_GL_TEST_NATIVE_OT_INVALID_W,
	/* Continuity-preserving follow-ups to the native-Y positive result.
	 * VULKAN_CLIP_MATH is shader-only.  The remaining modes operate at the
	 * final opaque-textured handoff, but make a deterministic per-vertex
	 * decision so duplicate vertices cannot be split by primitive class. */
	PGXP_DIAG_GL_TEST_VULKAN_CLIP_MATH,
	PGXP_DIAG_GL_TEST_OT_Y_BLEND_50,
	PGXP_DIAG_GL_TEST_OT_Y_BLEND_75,
	PGXP_DIAG_GL_TEST_OT_Y_BLEND_875,
	PGXP_DIAG_GL_TEST_OT_Y_GT_HALF,
	PGXP_DIAG_GL_TEST_OT_Y_GT_QUARTER,
	PGXP_DIAG_GL_TEST_OT_Y_LE_HALF,
	PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_25,
	PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_50,
	PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_BLEND_75,
	PGXP_DIAG_GL_TEST_OT_Y_NATIVE_X_GT_HALF,
	/* OpenGL-renderer coverage probes.  These leave PGXP provenance and
	 * coordinates alone until the final GL handoff. */
	PGXP_DIAG_GL_TEST_CONSERVATIVE_RASTER,
	PGXP_DIAG_GL_TEST_COVERAGE_EXPAND_QUARTER,
	PGXP_DIAG_GL_TEST_COVERAGE_EXPAND_HALF,
	PGXP_DIAG_GL_TEST_COVERAGE_EXPAND_ONE,
	PGXP_DIAG_GL_TEST_COVERAGE_EXPAND_TWO,
	/* Fragment-stage controls that distinguish absent raster coverage from
	 * fragments discarded after sampling a transparent PS1 texel. */
	PGXP_DIAG_GL_TEST_TEXTURE_SOLID_OPAQUE,
	PGXP_DIAG_GL_TEST_TEXTURE_TRANSPARENT_MARKER,
	/* Larger coverage probes retain the original 8-epsilon cap first, then
	 * repeat 2/3/4 subpixels with a tighter 4-epsilon acute-vertex cap. */
	PGXP_DIAG_GL_TEST_COVERAGE_EXPAND_THREE,
	PGXP_DIAG_GL_TEST_COVERAGE_EXPAND_FOUR,
	PGXP_DIAG_GL_TEST_COVERAGE_EXPAND_TWO_CAP4,
	PGXP_DIAG_GL_TEST_COVERAGE_EXPAND_THREE_CAP4,
	PGXP_DIAG_GL_TEST_COVERAGE_EXPAND_FOUR_CAP4,
	/* Provenance-gated coverage: expand only primitives that reached the
	 * renderer with a valid PGXP W, excluding sprites/native 2D fallback. */
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_TWO,
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_THREE,
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_FOUR,
	/* Follow the valid-W result to its plateau and separate requested edge
	 * growth from the acute-triangle displacement cap. */
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_FIVE,
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_FOUR_CAP16,
	/* Vulkan's raster precision is a read-only device limit.  These controls
	 * snap valid-W PGXP inputs to OpenGL's observed 4-bit framebuffer grid;
	 * Vulkan still evaluates edges with its advertised native precision. */
	PGXP_DIAG_GL_TEST_VK_VALID_W_SNAP_NEAREST_4BIT,
	PGXP_DIAG_GL_TEST_VK_VALID_W_SNAP_FLOOR_4BIT,
	/* High-cap GL coverage with a second, relative-growth safety bound. */
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_FOUR_SCALE_128,
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_FOUR_SCALE_64,
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_FOUR_SCALE_32,
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_FOUR_SCALE_16,
	/* Intermediate absolute caps between the safe cap-8 and perfect cap-16
	 * controls, retaining the four-subpixel target. */
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_FOUR_CAP12,
	PGXP_DIAG_GL_TEST_COVERAGE_VALID_W_FOUR_CAP14,
	/* Preserve the original homogeneous interpolation planes while testing
	 * the known cap-8/12/14/16 coverage geometries. */
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP8,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP12,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP14,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP16,
	/* Reduce the ordinary edge margin while holding mode 70's one-coordinate
	 * maximum acute-vertex displacement constant. */
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_ONE_MAX1,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_TWO_MAX1,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_THREE_MAX1,
	/* Separate opaque and semi-transparent textured coverage.  The latter is
	 * composited in two OpenGL passes, so expanding it can change overdraw
	 * even when the original homogeneous surface is preserved exactly. */
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP8_OPAQUE,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP8_SEMITRANS,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP8_OPAQUE_SEMI_ONE,
	/* Mark only transparent-texel discards in the newly covered skirt, then
	 * vary ordinary edge width and acute displacement independently. */
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP8_OPAQUE_SKIRT_MARKER,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP8_OPAQUE_ONE,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP8_OPAQUE_TWO,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP8_OPAQUE_THREE,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP4_OPAQUE_FOUR,
	PGXP_DIAG_GL_TEST_COVERAGE_PRESERVE_CAP2_OPAQUE_FOUR,
	/* Use expansion only to make candidate fragments available, then apply
	 * an explicit fragment-stage inside test for either the original precise
	 * triangle or that triangle snapped to an 8-bit framebuffer grid. */
	PGXP_DIAG_GL_TEST_COVERAGE_CLIP_ORIGINAL_OPAQUE,
	PGXP_DIAG_GL_TEST_COVERAGE_CLIP_SNAP8_OPAQUE,
	/* Preserve the ordinary GL draw, then add only the outside portion of a
	 * second opaque, valid-W, surface-preserving coverage copy. */
	PGXP_DIAG_GL_TEST_COVERAGE_UNION_SKIRT_OPAQUE,
	/* Restrict that outside repair copy to edges which have an exact native
	 * endpoint-pair match on the opposite side of another opaque PGXP
	 * triangle.  Modes 89-91 measure the required subpixel width with a
	 * same-material gate; 92 relaxes material continuity, while 93 and 94
	 * tighten it with packet proximity and endpoint UV continuity. */
	PGXP_DIAG_GL_TEST_ADJACENCY_MATERIAL_ONE,
	PGXP_DIAG_GL_TEST_ADJACENCY_MATERIAL_TWO,
	PGXP_DIAG_GL_TEST_ADJACENCY_MATERIAL_FOUR,
	PGXP_DIAG_GL_TEST_ADJACENCY_ANY_FOUR,
	PGXP_DIAG_GL_TEST_ADJACENCY_NEAR_FOUR,
	PGXP_DIAG_GL_TEST_ADJACENCY_UV_FOUR,
	/* Generalize exact whole-edge adjacency to native-collinear partial
	 * coverage.  Containment modes target the short edge alone or both sides
	 * of a long-edge/short-edge T-junction; the final mode also admits
	 * positive partial overlaps where neither interval contains the other. */
	PGXP_DIAG_GL_TEST_PARTIAL_SHORT_MATERIAL_FOUR,
	PGXP_DIAG_GL_TEST_PARTIAL_SHORT_ANY_FOUR,
	PGXP_DIAG_GL_TEST_PARTIAL_BOTH_MATERIAL_FOUR,
	PGXP_DIAG_GL_TEST_PARTIAL_BOTH_ANY_FOUR,
	PGXP_DIAG_GL_TEST_OVERLAP_BOTH_ANY_FOUR,
	PGXP_DIAG_GL_TEST_COUNT
};

typedef struct PGXP_diag_primitive_vertex_Tag
{
	int32_t native_x;
	int32_t native_y;
	float precise_before_x;
	float precise_before_y;
	float precise_before_w;
	float precise_after_x;
	float precise_after_y;
	float precise_after_w;
	uint16_t u;
	uint16_t v;
} PGXP_diag_primitive_vertex;

#if PGXP_DIAG

void PGXP_DiagInit(void);
void PGXP_DiagResetRecovery(void);
void PGXP_DiagFrame(int backend);
void PGXP_DiagMemoryRead(uint32_t addr, uint32_t value, int valid_address);
void PGXP_DiagMemoryWrite(uint32_t addr, const PGXP_value* value,
		int valid_address, int full_word);
void PGXP_DiagCPULoad(uint32_t instr, uint32_t addr, uint32_t value,
		const PGXP_value* result, int memory_state);
uint32_t PGXP_DiagCPUInvalidMask(void);
void PGXP_DiagCPUDispatch(uint32_t instr, uint32_t addr, unsigned dest,
		uint32_t before_mask, uint32_t before_flags,
		uint16_t before_gflags);
void PGXP_DiagGTEVertex(float x, float y, float z, uint32_t value);
void PGXP_DiagProjectionZ(double raw_z, float precise_z,
		uint16_t architectural_z, uint16_t h);
int PGXP_DiagVertexWEligible(unsigned slot, const PGXP_value* shadow);
void PGXP_DiagTraceGTE(PGXP_value* value);
int PGXP_DiagRecoverVertex(uint32_t value, const PGXP_value* stale,
		unsigned slot,
		float* x, float* y, float* z);
void PGXP_DiagTraceMFC2(uint32_t instr, PGXP_value* value);
void PGXP_DiagTraceShift(uint32_t instr, uint32_t before, uint32_t after,
		int arithmetic, unsigned reason, PGXP_value* value);
void PGXP_DiagMFC2(uint32_t instr, uint32_t value);
void PGXP_DiagShift(uint32_t instr, uint32_t before, uint32_t after,
		int arithmetic);
int PGXP_DiagPreserveShift(uint32_t instr, uint32_t before,
		uint32_t after, int arithmetic);
void PGXP_DiagIdentityMove(unsigned dest, unsigned source,
		uint32_t before, uint32_t after);
void PGXP_DiagBeginInstruction(uint32_t instr, const uint32_t* gpr);
void PGXP_DiagObserveInstruction(uint32_t instr, const uint32_t* gpr);
void PGXP_DiagLineageStore(uint32_t instr, uint32_t value, uint32_t addr);
void PGXP_DiagStore8(uint32_t addr, uint8_t value,
		uint32_t invalid_count, const PGXP_value* shadow);
void PGXP_DiagFIFOWrite(unsigned pos, uint32_t addr, uint32_t value,
		const PGXP_value* shadow);
void PGXP_DiagCBWrite(unsigned slot, unsigned fifo_pos);
void PGXP_DiagPacket(uint8_t opcode, unsigned words, unsigned abr,
		unsigned tex_mode, int mask_eval);
void PGXP_DiagPrimitive(const PGXP_diag_primitive_vertex vertices[3],
		int invalid_w, int tolerance, unsigned upscale_shift);
void PGXP_DiagSubmitPrimitive(const PGXP_diag_primitive_vertex* vertices,
		unsigned count, int invalid_w, unsigned upscale_shift);
void PGXP_DiagGLPrimitive(const void* vertices, unsigned count,
		unsigned stride_bytes, uint64_t material_key);
void PGXP_DiagGLRepair(void* vertices, unsigned count,
		unsigned stride_bytes);
unsigned PGXP_DiagGLSharedEdgeMask(unsigned triangle_start);
void PGXP_DiagGLSetMode(unsigned mode);
unsigned PGXP_DiagGLGetMode(void);
void PGXP_DiagGLRasterCaps(unsigned subpixel_bits);
void PGXP_DiagGPUPrimitive(const PGXP_diag_primitive_vertex vertices[3],
		unsigned quad_part, int invalid_w, unsigned upscale_shift);
void PGXP_DiagLineHack(int32_t average_y, int rejected_w,
		float w0, float w1, float w2);
void PGXP_DiagVertex(enum PGXP_diag_vertex_source source,
		unsigned slot, uint32_t value, const PGXP_value* shadow,
		float x, float y, float w, int32_t native_x, int32_t native_y,
		int valid_w,
		int valid_xy, int value_match);
void PGXP_DiagNCLIP(int32_t native_value, int32_t precise_value,
		int32_t reference_value, int32_t applied_value);
void PGXP_DiagNCLIPValidity(unsigned invalid_mask, unsigned mismatch_mask,
		uint32_t sxy0, uint32_t sxy1, uint32_t sxy2,
		const PGXP_value* shadow0, const PGXP_value* shadow1,
		const PGXP_value* shadow2);

#define PGXP_DIAG_PRIMITIVE_DECLARE(name) PGXP_diag_primitive_vertex name[3]
#define PGXP_DIAG_PRIMITIVE_BEFORE(name, i, vertex) do { \
	(name)[i].native_x = (vertex).x; \
	(name)[i].native_y = (vertex).y; \
	(name)[i].precise_before_x = (vertex).precise[0]; \
	(name)[i].precise_before_y = (vertex).precise[1]; \
	(name)[i].precise_before_w = (vertex).precise[2]; \
	(name)[i].u = (vertex).u; \
	(name)[i].v = (vertex).v; \
} while (0)
#define PGXP_DIAG_PRIMITIVE_AFTER(name, i, vertex) do { \
	(name)[i].precise_after_x = (vertex).precise[0]; \
	(name)[i].precise_after_y = (vertex).precise[1]; \
	(name)[i].precise_after_w = (vertex).precise[2]; \
} while (0)

#else

#define PGXP_DiagInit() ((void)0)
#define PGXP_DiagResetRecovery() ((void)0)
#define PGXP_DiagFrame(backend) ((void)0)
#define PGXP_DiagMemoryRead(addr, value, valid_address) ((void)0)
#define PGXP_DiagMemoryWrite(addr, value, valid_address, full_word) ((void)0)
#define PGXP_DiagCPULoad(instr, addr, value, result, memory_state) ((void)0)
#define PGXP_DiagCPUInvalidMask() UINT32_C(0)
#define PGXP_DiagCPUDispatch(instr, addr, dest, before_mask, before_flags, before_gflags) ((void)0)
#define PGXP_DiagGTEVertex(x, y, z, value) ((void)0)
#define PGXP_DiagProjectionZ(raw_z, precise_z, architectural_z, h) ((void)0)
#define PGXP_DiagVertexWEligible(slot, shadow) 1
#define PGXP_DiagTraceGTE(value) ((void)0)
#define PGXP_DiagRecoverVertex(value, stale, slot, x, y, z) 0
#define PGXP_DiagTraceMFC2(instr, value) ((void)0)
#define PGXP_DiagTraceShift(instr, before, after, arithmetic, reason, value) ((void)0)
#define PGXP_DiagMFC2(instr, value) ((void)0)
#define PGXP_DiagShift(instr, before, after, arithmetic) ((void)0)
#define PGXP_DiagPreserveShift(instr, before, after, arithmetic) 0
#define PGXP_DiagIdentityMove(dest, source, before, after) ((void)0)
#define PGXP_DiagBeginInstruction(instr, gpr) ((void)0)
#define PGXP_DiagObserveInstruction(instr, gpr) ((void)0)
#define PGXP_DiagLineageStore(instr, value, addr) ((void)0)
#define PGXP_DiagStore8(addr, value, invalid_count, shadow) ((void)0)
#define PGXP_DiagFIFOWrite(pos, addr, value, shadow) ((void)0)
#define PGXP_DiagCBWrite(slot, fifo_pos) ((void)0)
#define PGXP_DiagPacket(opcode, words, abr, tex_mode, mask_eval) ((void)0)
#define PGXP_DiagPrimitive(vertices, invalid_w, tolerance, upscale_shift) ((void)0)
#define PGXP_DiagSubmitPrimitive(vertices, count, invalid_w, upscale_shift) ((void)0)
#define PGXP_DiagGLPrimitive(vertices, count, stride_bytes, material_key) ((void)0)
#define PGXP_DiagGLRepair(vertices, count, stride_bytes) ((void)0)
#define PGXP_DiagGLSharedEdgeMask(triangle_start) 0u
#define PGXP_DiagGLSetMode(mode) ((void)0)
#define PGXP_DiagGLGetMode() PGXP_DIAG_GL_TEST_OFF
#define PGXP_DiagGLRasterCaps(subpixel_bits) ((void)0)
#define PGXP_DiagGPUPrimitive(vertices, quad_part, invalid_w, upscale_shift) ((void)0)
#define PGXP_DiagLineHack(average_y, rejected_w, w0, w1, w2) ((void)0)
#define PGXP_DiagVertex(source, slot, value, shadow, x, y, w, native_x, native_y, valid_w, valid_xy, value_match) ((void)0)
#define PGXP_DiagNCLIP(native_value, precise_value, reference_value, applied_value) ((void)0)
#define PGXP_DiagNCLIPValidity(invalid_mask, mismatch_mask, sxy0, sxy1, sxy2, shadow0, shadow1, shadow2) ((void)0)
#define PGXP_DIAG_PRIMITIVE_DECLARE(name)
#define PGXP_DIAG_PRIMITIVE_BEFORE(name, i, vertex) ((void)0)
#define PGXP_DIAG_PRIMITIVE_AFTER(name, i, vertex) ((void)0)

#endif

#endif /* _PGXP_DIAG_H_ */

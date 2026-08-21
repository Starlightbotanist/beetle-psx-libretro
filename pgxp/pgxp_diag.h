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
void PGXP_DiagProjectionZ(double raw_z, float precise_z, uint16_t h);
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
		int invalid_w, int tolerance);
void PGXP_DiagGPUPrimitive(const PGXP_diag_primitive_vertex vertices[3],
		unsigned quad_part, int invalid_w, unsigned upscale_shift);
void PGXP_DiagVertex(enum PGXP_diag_vertex_source source,
		unsigned slot, uint32_t value, const PGXP_value* shadow,
		float x, float y, float w, int32_t native_x, int32_t native_y,
		int valid_w,
		int valid_xy, int value_match);
void PGXP_DiagNCLIP(int32_t native_value, int32_t precise_value,
		int32_t applied_value);

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
#define PGXP_DiagFrame(backend) ((void)0)
#define PGXP_DiagMemoryRead(addr, value, valid_address) ((void)0)
#define PGXP_DiagMemoryWrite(addr, value, valid_address, full_word) ((void)0)
#define PGXP_DiagCPULoad(instr, addr, value, result, memory_state) ((void)0)
#define PGXP_DiagCPUInvalidMask() UINT32_C(0)
#define PGXP_DiagCPUDispatch(instr, addr, dest, before_mask, before_flags, before_gflags) ((void)0)
#define PGXP_DiagGTEVertex(x, y, z, value) ((void)0)
#define PGXP_DiagProjectionZ(raw_z, precise_z, h) ((void)0)
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
#define PGXP_DiagPrimitive(vertices, invalid_w, tolerance) ((void)0)
#define PGXP_DiagGPUPrimitive(vertices, quad_part, invalid_w, upscale_shift) ((void)0)
#define PGXP_DiagVertex(source, slot, value, shadow, x, y, w, native_x, native_y, valid_w, valid_xy, value_match) ((void)0)
#define PGXP_DiagNCLIP(native_value, precise_value, applied_value) ((void)0)
#define PGXP_DIAG_PRIMITIVE_DECLARE(name)
#define PGXP_DIAG_PRIMITIVE_BEFORE(name, i, vertex) ((void)0)
#define PGXP_DIAG_PRIMITIVE_AFTER(name, i, vertex) ((void)0)

#endif

#endif /* _PGXP_DIAG_H_ */

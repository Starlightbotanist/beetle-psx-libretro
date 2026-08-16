#ifndef _PGXP_DIAG_H_
#define _PGXP_DIAG_H_

#include "pgxp_types.h"

enum PGXP_diag_vertex_source
{
	PGXP_DIAG_VERTEX_TRACKED = 0,
	PGXP_DIAG_VERTEX_CACHE,
	PGXP_DIAG_VERTEX_NATIVE
};

#if PGXP_DIAG

void PGXP_DiagInit(void);
void PGXP_DiagFrame(int backend);
void PGXP_DiagMemoryRead(uint32_t addr, uint32_t value, int valid_address);
void PGXP_DiagMemoryWrite(uint32_t addr, uint32_t value, int valid_address);
void PGXP_DiagCPULoad(uint32_t instr, uint32_t addr, uint32_t value,
		const PGXP_value* result, int memory_state);
uint32_t PGXP_DiagCPUInvalidMask(void);
void PGXP_DiagCPUDispatch(uint32_t instr, uint32_t addr, unsigned dest,
		uint32_t before_mask, uint32_t before_flags,
		uint16_t before_gflags);
void PGXP_DiagGTEVertex(float x, float y, float z, uint32_t value);
void PGXP_DiagMFC2(uint32_t instr, uint32_t value);
void PGXP_DiagShift(uint32_t instr, uint32_t before, uint32_t after,
		int arithmetic);
int PGXP_DiagPreserveShift(uint32_t instr, uint32_t before,
		uint32_t after, int arithmetic);
void PGXP_DiagLineageStore(uint32_t instr, uint32_t value, uint32_t addr);
void PGXP_DiagStore8(uint32_t addr, uint8_t value,
		uint32_t invalid_count, const PGXP_value* shadow);
void PGXP_DiagFIFOWrite(unsigned pos, uint32_t addr, uint32_t value,
		const PGXP_value* shadow);
void PGXP_DiagCBWrite(unsigned slot, unsigned fifo_pos);
void PGXP_DiagVertex(enum PGXP_diag_vertex_source source,
		unsigned slot, uint32_t value, const PGXP_value* shadow,
		float x, float y, float w, int valid_w,
		int valid_xy, int value_match);
void PGXP_DiagNCLIP(int32_t native_value, int32_t precise_value);

#else

#define PGXP_DiagInit() ((void)0)
#define PGXP_DiagFrame(backend) ((void)0)
#define PGXP_DiagMemoryRead(addr, value, valid_address) ((void)0)
#define PGXP_DiagMemoryWrite(addr, value, valid_address) ((void)0)
#define PGXP_DiagCPULoad(instr, addr, value, result, memory_state) ((void)0)
#define PGXP_DiagCPUInvalidMask() UINT32_C(0)
#define PGXP_DiagCPUDispatch(instr, addr, dest, before_mask, before_flags, before_gflags) ((void)0)
#define PGXP_DiagGTEVertex(x, y, z, value) ((void)0)
#define PGXP_DiagMFC2(instr, value) ((void)0)
#define PGXP_DiagShift(instr, before, after, arithmetic) ((void)0)
#define PGXP_DiagPreserveShift(instr, before, after, arithmetic) 0
#define PGXP_DiagLineageStore(instr, value, addr) ((void)0)
#define PGXP_DiagStore8(addr, value, invalid_count, shadow) ((void)0)
#define PGXP_DiagFIFOWrite(pos, addr, value, shadow) ((void)0)
#define PGXP_DiagCBWrite(slot, fifo_pos) ((void)0)
#define PGXP_DiagVertex(source, slot, value, shadow, x, y, w, valid_w, valid_xy, value_match) ((void)0)
#define PGXP_DiagNCLIP(native_value, precise_value) ((void)0)

#endif

#endif /* _PGXP_DIAG_H_ */

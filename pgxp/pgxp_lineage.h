#ifndef _PGXP_LINEAGE_H_
#define _PGXP_LINEAGE_H_

#include <stdint.h>

#include "pgxp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Production transport for exact GTE screen-coordinate provenance.
 *
 * The architectural MIPS word always travels through the ordinary emulator
 * state.  This sidecar carries only the original precise GTE payload and an
 * exact stage proof.  It never participates in CPU arithmetic and is
 * recovered only after a proven MFC2 -> SLL5 -> SRA5 packing lineage reaches
 * a GPU command vertex. */
void PGXP_LineageInit(void);
void PGXP_LineageReset(void);
void PGXP_LineageMFC2(uint32_t instr, uint32_t value,
		const PGXP_value* precise);
void PGXP_LineageShift(uint32_t instr, uint32_t before,
		uint32_t after, int arithmetic);
void PGXP_LineageIdentityMove(unsigned dest, unsigned source,
		uint32_t before, uint32_t after);
void PGXP_LineageObserveInstruction(uint32_t instr);
void PGXP_LineageLoad(uint32_t instr, uint32_t addr, uint32_t value);
void PGXP_LineageMemoryWrite(uint32_t addr);
void PGXP_LineageStore(uint32_t instr, uint32_t value, uint32_t addr);
void PGXP_LineageFIFOWrite(unsigned pos, uint32_t addr, uint32_t value);
void PGXP_LineageCBWrite(unsigned slot, unsigned fifo_pos);
int PGXP_LineageRecoverVertex(unsigned slot, uint32_t value,
		float* x, float* y, float* z, int* valid_w);

#ifdef __cplusplus
}
#endif

#endif /* _PGXP_LINEAGE_H_ */

#ifndef _PGXP_LINEAGE_H_
#define _PGXP_LINEAGE_H_

#include <stdint.h>

#include "pgxp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exact provenance for projected screen coordinates that are temporarily
 * repacked as integers. The architectural word remains in ordinary emulated
 * state; this sidecar only carries the original projected payload and proof
 * of the recognized transform sequence. */
void PGXP_LineageReset(void);
/* Allocate the direct-address sidecar on enable and release it on disable.
 * Allocation failure leaves exact recovery disabled rather than accepting
 * provenance without storage. */
int PGXP_LineageSetEnabled(int enabled);
void PGXP_LineageMFC2(uint32_t instr, uint32_t value,
		const PGXP_value* precise);
void PGXP_LineageShift(uint32_t instr, uint32_t before,
		uint32_t after, int arithmetic);
void PGXP_LineageTaggedAdd(uint32_t instr, uint32_t before,
		uint32_t after);
void PGXP_LineageIdentityMove(unsigned dest, unsigned source,
		uint32_t before, uint32_t after);
void PGXP_LineageObserveRegisterWrite(unsigned dest);
void PGXP_LineageLoad(uint32_t instr, uint32_t addr, uint32_t value);
void PGXP_LineageMemoryWrite(uint32_t addr);
/* Invalidate every architectural word touched by a raw memory write. */
void PGXP_LineageMemoryWriteRange(uint32_t addr, uint32_t size);
void PGXP_LineageStore(uint32_t instr, uint32_t value, uint32_t addr);
void PGXP_LineageFIFOWrite(unsigned pos, uint32_t addr, uint32_t value);
void PGXP_LineageCBWrite(unsigned slot, unsigned fifo_pos);
int PGXP_LineageRecoverVertex(unsigned slot, uint32_t value,
		float* x, float* y, float* z, int* valid_w);

#ifdef __cplusplus
}
#endif

#endif /* _PGXP_LINEAGE_H_ */

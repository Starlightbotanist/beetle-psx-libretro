#include "pgxp_main.h"
#include "pgxp_cpu.h"
#include "pgxp_diag.h"
#include "pgxp_gpu.h"
#include "pgxp_lineage.h"
#include "pgxp_mem.h"
#include "pgxp_gte.h"

/* Hot-path: gMode is read on every CPU instruction with a PGXP
 * branch (see the 50+ PGXP_GetModes() call sites in mednafen/psx/
 * cpu.c).  Make it externally visible so PGXP_GetModes can live
 * in the header as a static inline - skipping the cross-TU call
 * setup at every site - while keeping all writes routed through
 * apply_modes() here so the vertex-cache free still happens. */
uint32_t gMode = 0;
uint32_t gExperimentMask = PGXP_FEATURE_ALL;

void PGXP_Init(void)
{
	PGXP_InitMem();
	PGXP_InitCPU();
	PGXP_InitGTE();
	PGXP_LineageInit();
	PGXP_DiagInit();
}

void PGXP_Shutdown(void)
{
	gMode = 0;
	PGXP_FreeVertexCache();
}

/* Apply a mode transition.  If the PGXP_VERTEX_CACHE bit is being
 * cleared (user toggled the vertex-cache core option off at runtime),
 * the 448 MB heap allocation backing the cache is freed immediately
 * rather than waiting for retro_deinit. */
static void apply_modes(uint32_t new_modes)
{
	uint32_t old_modes = gMode;
	gMode = new_modes;
	if ((old_modes & PGXP_VERTEX_CACHE) && !(new_modes & PGXP_VERTEX_CACHE))
		PGXP_FreeVertexCache();
	/* Off to on: TransformXY skips the vertex push entirely while gMode is
	 * zero, so the screen-XY FIFO holds entries from before PGXP was last
	 * disabled. Retire them rather than let a stale one match by chance. */
	if (!old_modes && new_modes)
		PGXP_InvalidateVertexFIFO();
	/* The exact-lineage sidecar is not architectural state. Any transition
	 * that changes how CPU/GTE memory tracking runs retires the old proof. */
	if ((old_modes ^ new_modes) &
	    (PGXP_MODE_MEMORY | PGXP_MODE_CPU | PGXP_MODE_GTE))
		PGXP_LineageReset();
}

void PGXP_SetModes(uint32_t modes)
{
	apply_modes(modes);
}

void PGXP_EnableModes(uint32_t modes)
{
	apply_modes(gMode | modes);
}

void PGXP_DisableModes(uint32_t modes)
{
	apply_modes(gMode & ~modes);
}

void PGXP_SetExperimentMask(uint32_t mask)
{
	uint32_t new_mask = mask & PGXP_FEATURE_ALL;
	uint32_t changed = gExperimentMask ^ new_mask;
	gExperimentMask = new_mask;

	/* Runtime option changes must not make stale, feature-specific history
	 * eligible when a layer is re-enabled.  These resets do not disturb the
	 * ordinary PGXP shadow registers or vertex cache. */
	if (changed & (PGXP_FEATURE_IDENTITY_MOVE |
	               PGXP_FEATURE_EXACT_LINEAGE))
		PGXP_LineageReset();
}

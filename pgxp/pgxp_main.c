#include "pgxp_main.h"
#include "pgxp_cpu.h"
#include "pgxp_gpu.h"
#include "pgxp_lineage.h"
#include "pgxp_mem.h"
#include "pgxp_gte.h"

#include <libretro.h>

extern retro_log_printf_t log_cb;

/* Hot-path: gMode is read on every CPU instruction with a PGXP
 * branch (see the 50+ PGXP_GetModes() call sites in mednafen/psx/
 * cpu.c).  Make it externally visible so PGXP_GetModes can live
 * in the header as a static inline - skipping the cross-TU call
 * setup at every site - while keeping all writes routed through
 * apply_modes() here so the vertex-cache free still happens. */
uint32_t gMode = 0;

void PGXP_Init(void)
{
	PGXP_InitMem();
	PGXP_InitCPU();
	PGXP_InitGTE();
	PGXP_LineageReset();
}

void PGXP_Shutdown(void)
{
	gMode = 0;
	PGXP_FreeVertexCache();
	PGXP_LineageSetEnabled(0);
}

/* Apply a mode transition.  If the PGXP_VERTEX_CACHE bit is being
 * cleared (user toggled the vertex-cache core option off at runtime),
 * the 448 MB heap allocation backing the cache is freed immediately
 * rather than waiting for retro_deinit. */
static void apply_modes(uint32_t new_modes)
{
	uint32_t old_modes = gMode;
	uint32_t lineage_modes = PGXP_MODE_MEMORY | PGXP_MODE_GTE;
	gMode = new_modes;
	if (!PGXP_LineageSetEnabled((new_modes & lineage_modes) != 0) && log_cb)
		log_cb(RETRO_LOG_ERROR,
			"PGXP: exact-lineage allocation failed; recovery disabled\n");
	if ((old_modes & PGXP_VERTEX_CACHE) && !(new_modes & PGXP_VERTEX_CACHE))
		PGXP_FreeVertexCache();
	/* Off to on: TransformXY skips the vertex push entirely while gMode is
	 * zero, so the screen-XY FIFO holds entries from before PGXP was last
	 * disabled. Retire them rather than let a stale one match by chance. */
	if (!old_modes && new_modes)
		PGXP_InvalidateVertexFIFO();
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

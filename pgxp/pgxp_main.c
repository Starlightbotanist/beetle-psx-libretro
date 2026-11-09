#include "pgxp_main.h"
#include "pgxp_cpu.h"
#include "pgxp_gpu.h"
#include "pgxp_mem.h"
#include "pgxp_gte.h"

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
	PGXP_InitGPU();
	PGXP_GTE_ResetColorCount();
}

void PGXP_Shutdown(void)
{
	gMode = 0;
	PGXP_InitMem();
	PGXP_FreeVertexCache();
}

/* Apply a mode transition.  If the PGXP_VERTEX_CACHE bit is being
 * cleared (user toggled the vertex-cache core option off at runtime),
 * the 256 MiB heap allocation backing the cache is freed immediately
 * rather than waiting for retro_deinit. */
static void apply_modes(uint32_t new_modes)
{
	uint32_t old_modes = gMode;
	uint32_t tracking_modes = PGXP_MODE_MEMORY | PGXP_MODE_CPU | PGXP_MODE_GTE |
		PGXP_TEXTURE_CORRECTION | PGXP_DEPTH_BUFFER | PGXP_DEPTH_NORMALIZATION;

	/* Disabled tracking cannot describe intervening native writes. Texture
	 * correction and normalization also change the meaning of shadow W. */
	if ((old_modes ^ new_modes) & tracking_modes)
		PGXP_Init();
	gMode = new_modes;
	if ((old_modes & PGXP_VERTEX_CACHE) && !(new_modes & PGXP_VERTEX_CACHE))
		PGXP_FreeVertexCache();
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

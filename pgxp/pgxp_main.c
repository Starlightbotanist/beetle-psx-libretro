#include "pgxp_state.h"
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

#include <stdlib.h>

typedef struct
{
   uint8_t *data;
   void *memory;
   void *vertices;
   uint32_t modes;
   bool captured;
} PGXPState;

void *PGXP_StateCreate(void)
{
   PGXPState *state;
   size_t bytes;
   if (!gMode)
      return NULL;
   state = (PGXPState *)calloc(1, sizeof(*state));
   if (!state)
      return NULL;
   bytes = PGXP_CPUStateSize() +
         PGXP_GTEStateSize() + PGXP_GPUStateSize();
   state->data = (uint8_t *)malloc(bytes);
   state->memory = PGXP_MemoryStateCreate();
   if (gMode & PGXP_VERTEX_CACHE)
      state->vertices = PGXP_VertexStateCreate();
   state->modes = gMode;
   if (!state->data || !state->memory || ((gMode & PGXP_VERTEX_CACHE) && !state->vertices))
   {
      PGXP_StateFree(state);
      return NULL;
   }
   return state;
}

void PGXP_StateFree(void *snapshot)
{
   PGXPState *state = (PGXPState *)snapshot;
   if (!state)
      return;
   PGXP_MemoryStateFree(state->memory);
   PGXP_VertexStateFree(state->vertices);
   free(state->data);
   free(state);
}

bool PGXP_StateMatches(const void *snapshot)
{
   const PGXPState *state = (const PGXPState *)snapshot;
   return state && state->data && state->memory && state->modes == gMode;
}

static void pgxp_state_transfer(PGXPState *state, bool load)
{
   uint8_t *p = state->data;
   PGXP_CPUState(p, load); p += PGXP_CPUStateSize();
   PGXP_GTEState(p, load); p += PGXP_GTEStateSize();
   PGXP_GPUState(p, load);
}

bool PGXP_StateSave(void *snapshot)
{
   PGXPState *state = (PGXPState *)snapshot;
   if (!PGXP_StateMatches(state) || !PGXP_MemoryStateSave(state->memory))
      return false;
   if (state->vertices && !PGXP_VertexStateSave(state->vertices))
      return false;
   pgxp_state_transfer(state, false);
   state->captured = true;
   return true;
}

bool PGXP_StatePrepare(void *snapshot)
{
   PGXPState *state = (PGXPState *)snapshot;
   return PGXP_StateMatches(state) &&
      (!state->vertices || PGXP_VertexStatePrepare(state->vertices));
}

bool PGXP_StateLoad(const void *snapshot)
{
   PGXPState *state = (PGXPState *)snapshot;
   if (!PGXP_StateMatches(state) || !state->captured || !PGXP_StatePrepare(state))
      return false;
   if (state->vertices)
      PGXP_VertexStateLoad(state->vertices);
   PGXP_MemoryStateLoad(state->memory);
   pgxp_state_transfer(state, true);
   return true;
}

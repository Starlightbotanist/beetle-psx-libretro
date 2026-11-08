/* The harnesses link PGXP without libretro.c, which owns log_cb. NULL
 * here; pgxp_gpu.c null-checks before calling, so the instrumentation
 * line is simply inert offline. */
#include <libretro.h>
retro_log_printf_t log_cb = NULL;

/* These harnesses inject projected vertices rather than executing the GTE.
 * Use an identity depth row; native-GTE tests link the real register reader. */
uint32_t GTE_ReadCR(unsigned int which)
{
   return which == 4 ? 4096 : 0;
}

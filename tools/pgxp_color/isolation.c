/* Memory-domain ownership during Lightrec's temporary isolated RAM window. */
#include <stdio.h>
#include <string.h>
#include "../../pgxp/pgxp_cpu.h"
#include "../../pgxp/pgxp_gpu.h"
#include "../../pgxp/pgxp_gte.h"
#include "../../pgxp/pgxp_main.h"
#include "../../pgxp/pgxp_mem.h"
#include "../../pgxp/pgxp_value.h"

#ifndef HAVE_LIGHTREC
#error This test needs HAVE_LIGHTREC for the isolated RAM window.
#endif

static unsigned checks, failures;
static uint32_t word = 0x0022000c;

static void check(int condition, const char *name)
{
   checks++;
   if (!condition) { failures++; fprintf(stderr, "FAIL %s\n", name); }
}

static void vertex(uint32_t address, float x, int depth, const char *name)
{
   OGLVertex output;
   PGXP_WriteCB(ReadMem(address), 0);
   PGXP_GetVertex(0, &word, &output, 0, 0);
   check(output.x == x && output.valid_w == depth, name);
}

int main(void)
{
   PGXP_value original = PGXP_value_zero, temporary;
   OGLVertex output;
   unsigned i;
   PGXP_Init();
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU | PGXP_MODE_GTE);
   SetValue(&original, word);
   original.x = 12.25f; original.y = 34.25f; original.z = 8;
   original.flags = VALID_012;
   temporary = original; temporary.x = 12.75f;
   WriteMem(&original, 0x1000);
   WriteMem(&original, 0xfffc);
   PGXP_CacheIsolation(1);
   WriteMem(&temporary, 0x1000);
   WriteMem(&temporary, 0xfffc);
   WriteMem(&temporary, 0x10000);
   WriteMem(&temporary, 0x1f800000);
   WriteMem(&temporary, 0x1f801810);
   PGXP_CPU_LW(1u << 16, word, 0x1000);
   PGXP_GTE_LWC2(14u << 16, word, 0x1000);
   PGXP_WriteCB(&CPU_reg[1], 0);
   PGXP_CacheIsolation(0);
   PGXP_GetVertex(0, &word, &output, 0, 0);
   check(output.x == 12.75f && output.valid_w,
         "already consumed command/register copy survives RAM restoration");
   check(CPU_reg[1].x == 12.75f, "already consumed CPU register survives");
   PGXP_GTE_SWC2(14u << 16, word, 0x11000);
   vertex(0x11000, 12.75f, 1, "already consumed GTE owner survives");
   vertex(0x1000, 12.25f, 1, "original RAM owner restored");
   vertex(0xfffc, 12.25f, 1, "last backed word restored");
   vertex(0x10000, 12.75f, 1, "first unbacked word remains updated");
   vertex(0x1f800000, 12.75f, 1, "scratchpad is a different domain");
   vertex(0x1f801810, 12.75f, 1, "register shadow is a different domain");
   for (i = 0; i < 3; i++)
   {
      PGXP_CacheIsolation(1); WriteMem(&temporary, 0x1000); PGXP_CacheIsolation(0);
      vertex(0x1000, 12.25f, 1, "repeated isolation preserves original owner");
   }
   PGXP_InitMem();
   PGXP_CacheIsolation(1); WriteMem(&temporary, 0x1000); PGXP_CacheIsolation(0);
   vertex(0x1000, 12.f, 0, "logically uninitialized original cannot revive stale bytes");
   WriteMem(&original, 0x1000);
   PGXP_CacheIsolation(1); WriteMem(&temporary, 0x1000);
   PGXP_InitMem();
   WriteMem(&temporary, 0x1000); WriteMem(&temporary, 0x10000);
   PGXP_CacheIsolation(0);
   vertex(0x1000, 12.f, 0, "reset invalidates the saved timeline");
   vertex(0x10000, 12.75f, 1, "unrelated fresh owner survives fallback");
   WriteMem(&original, 0x1000);
   PGXP_CacheIsolation(1);
   PGXP_SetModes(0); PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU);
   WriteMem(&temporary, 0x1000); PGXP_CacheIsolation(0);
   vertex(0x1000, 12.f, 0, "mode change cannot restore an old owner");
   WriteMem(&original, 0x1000);
   PGXP_CacheIsolation(1); WriteMem(&temporary, 0x1000); PGXP_Shutdown();
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU); PGXP_CacheIsolation(0);
   vertex(0x1000, 12.f, 0, "shutdown cannot restore an old owner");
   /* External writers invalidate both domains; untouched neighbors survive. */
   WriteMem(&original, 0x1000); WriteMem(&original, 0x1004);
   PGXP_CacheIsolation(1); WriteMem(&temporary, 0x1000);
   PGXP_InvalidateRAM(0x1000, 1, 0); PGXP_CacheIsolation(0);
   vertex(0x1000, 12.f, 0, "DMA range cannot revive a saved owner");
   vertex(0x1004, 12.25f, 1, "DMA range preserves saved neighboring owner");
   WriteMem(&original, 0x1000);
   PGXP_CacheIsolation(1); PGXP_InvalidateRAMWord(0x1000); PGXP_CacheIsolation(0);
   vertex(0x1000, 12.f, 0, "scattered DMA cannot revive a saved owner");
   vertex(0x1004, 12.25f, 1, "scattered DMA preserves saved neighboring owner");
   WriteMem(&original, 0xfff8); WriteMem(&original, 0xfffc); WriteMem(&original, 0x10000);
   PGXP_CacheIsolation(1); PGXP_InvalidateRAM(0xfffc, 2, 0); PGXP_CacheIsolation(0);
   vertex(0xfff8, 12.25f, 1, "boundary DMA preserves saved preceding owner");
   vertex(0xfffc, 12.f, 0, "boundary DMA clips the saved bitmap at64KB");
   vertex(0x10000, 12.f, 0, "boundary DMA still retires the active outside owner");
   WriteMem(&original, 0); WriteMem(&original, 0xfffc);
   PGXP_CacheIsolation(1); PGXP_InvalidateRAM(0, 2, 1); PGXP_CacheIsolation(0);
   vertex(0, 12.f, 0, "wrapped reverse DMA retires the saved first word");
   vertex(0xfffc, 12.25f, 1, "wrapped reverse DMA preserves the saved last word");
   WriteMem(&original, 0x1000);
   PGXP_CacheIsolation(1); PGXP_InvalidateRAM(0x1000, 0, 0); PGXP_CacheIsolation(0);
   vertex(0x1000, 12.25f, 1, "empty DMA preserves saved owners");
   PGXP_CacheIsolation(1); PGXP_InvalidateRAM(0, PGXP_RAM_WORDS, 0); PGXP_CacheIsolation(0);
   vertex(0x1000, 12.f, 0, "whole RAM DMA cannot revive any saved owner");
   PGXP_SetModes(0); PGXP_CacheIsolation(1);
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_CPU);
   WriteMem(&temporary, 0x1000); WriteMem(&temporary, 0x10000); PGXP_CacheIsolation(0);
   vertex(0x1000, 12.f, 0, "enabling tracking during isolation has no original snapshot");
   vertex(0x10000, 12.75f, 1, "new unrelated owner survives missing-snapshot fallback");
   WriteMem(&original, 0x1000);
   PGXP_CacheIsolation(1); WriteMem(&temporary, 0x1000); PGXP_CacheIsolation(0);
   vertex(0x1000, 12.25f, 1, "fresh snapshot recovers after missing-snapshot fallback");
   PGXP_Shutdown();
   printf("isolation checks=%u failures=%u\n", checks, failures);
   return failures != 0;
}

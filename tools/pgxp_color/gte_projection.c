/* Run the real GTE and consume its PGXP vertex through a command buffer. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libretro.h>
#include "../../mednafen/psx/gte.h"
#include "../../pgxp/pgxp_gpu.h"
#include "../../pgxp/pgxp_gte.h"
#include "../../pgxp/pgxp_main.h"
#include "../../pgxp/pgxp_mem.h"

retro_log_printf_t log_cb;
bool psx_gte_overclock;
uint8_t widescreen_hack;
uint8_t widescreen_hack_aspect_ratio_setting;

int MDFNSS_StateAction(void *st, int load, int data_only,
      SFORMAT *sf, const char *name)
{
   abort();
}

static OGLVertex project(uint32_t op, uint32_t mode, uint32_t *word)
{
   OGLVertex vertex = {0};
   unsigned i;

   GTE_Power();
   PGXP_SetModes(PGXP_MODE_MEMORY | PGXP_MODE_GTE |
         PGXP_TEXTURE_CORRECTION | mode);
   GTE_WriteCR(0, 2048);
   GTE_WriteCR(2, 2048);
   GTE_WriteCR(4, 4096);
   GTE_WriteCR(26, 320);
   for (i = 0; i < 6; i += 2)
   {
      GTE_WriteDR(i, 0xfffd0003u);
      GTE_WriteDR(i + 1, 256);
   }

   GTE_Instruction((1u << 19) | op);
   *word = GTE_ReadDR(14);
   PGXP_GTE_SWC2(14u << 16, *word, 0x10000);
   PGXP_WriteCB(ReadMem(0x10000), 0);
   PGXP_GetVertex(0, word, &vertex, 0, 0);
   return vertex;
}

int main(void)
{
   static const uint32_t operations[] = {0x01, 0x30};
   unsigned i;
   int failed = 0;

   GTE_Init();
   PGXP_Init();
   for (i = 0; i < sizeof(operations) / sizeof(operations[0]); i++)
   {
      uint32_t off_word, on_word;
      const OGLVertex off = project(operations[i], 0, &off_word);
      const OGLVertex on = project(operations[i], PGXP_DEPTH_BUFFER, &on_word);

      if (off_word != on_word || !off.valid_w || !on.valid_w ||
          off.x != 1.25f || off.y != -2.5f || off.w != 256.f ||
          on.x != off.x || on.y != off.y || on.w != off.w)
      {
         fprintf(stderr, "GTE op %02x changes projection with depth mode\n",
               (unsigned)operations[i]);
         failed = 1;
      }
   }
   PGXP_Shutdown();
   return failed;
}

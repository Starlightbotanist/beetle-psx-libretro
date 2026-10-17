/* Exercise the shipping GPU state table and serializer without a frontend.
 * Only device/renderer notifications are stubbed; no state fields or codec
 * implementation are copied into this test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mednafen/psx/gpu.h"
#include "mednafen/psx/irq.h"
#include "mednafen/psx/timer.h"
#include "mednafen/psx/psx_events.h"
#include "rhi/rhi_intf.h"

extern PS_GPU GPU;
extern bool FastSaveStates;
static uint16_t vram[1024 * 512];
static unsigned cases, assertions, failures;

static void check(const char *name, bool passed)
{
   assertions++;
   if (!passed)
   {
      if (failures < 12)
         fprintf(stderr, "%s: fast=%d case=%u\n", name, FastSaveStates, cases);
      failures++;
   }
}

static uint32_t read_u32(const uint8_t *data)
{
   return data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
          (uint32_t)data[3] << 24;
}

static void write_u32(uint8_t *data, uint32_t value)
{
   unsigned i;
   for (i = 0; i < 4; i++)
      data[i] = value >> (i * 8);
}

/* Strip appended entries from a real GPU section. All older
 * entries stay byte-for-byte unchanged, including the last old sentinel.
 * Do not use a second SFORMAT table that could disagree with production. */
static StateMem legacy_chunk(const StateMem *saved, unsigned removed)
{
   static const char *names[7] = {"InQuad_off_u", "InQuad_off_v", "InQuad_may_be_2d",
                                "CLUT_Cache", "CLUT_Cache_VB", "TexCache_Tag", "TexCache_Data"};
   static const unsigned sizes[7] = {2, 2, 1, 512, 4, 1024, 2048};
   uint32_t last[7] = {0};
   uint32_t cursor = 36;
   unsigned i;
   StateMem legacy = {0};

   check("GPU section header", saved->len >= 36 && !memcmp(saved->data, "GPU\0", 4) &&
                                   read_u32(saved->data + 32) == saved->len - 36);
   while (cursor < saved->len)
   {
      uint32_t size_at = cursor;
      for (i = 0; i < 6; i++)
         last[i] = last[i + 1];
      last[6] = cursor;
      if (!FastSaveStates)
         size_at += 1 + saved->data[cursor];
      if (size_at > saved->len || saved->len - size_at < 4)
         exit(2);
      cursor = size_at + 4 + read_u32(saved->data + size_at);
      if (cursor > saved->len)
         exit(2);
   }
   for (i = 0; i < 7; i++)
   {
      uint32_t size_at = last[i];
      if (!FastSaveStates)
      {
         unsigned length = saved->data[size_at++];
         check("appended field name",
               length == strlen(names[i]) &&
                   !memcmp(saved->data + size_at, names[i], length));
         size_at += length;
      }
      check("appended field size", read_u32(saved->data + size_at) == sizes[i]);
   }
   legacy.len = legacy.malloced = last[7 - removed];
   legacy.data = (uint8_t *)malloc(legacy.len);
   if (!legacy.data)
      exit(2);
   memcpy(legacy.data, saved->data, legacy.len);
   write_u32(legacy.data + 32, legacy.len - 36);
   return legacy;
}

static void poison(void)
{
   GPU.off_u = 0xaaaa;
   GPU.off_v = 0xbbbb;
   GPU.may_be_2d = true;
   GPU.DrawTimeAvail = -1;
   GPU.InQuad_F3Vertices[0].precise[2] = -1.0f;
   vram[123] = 0;
   memset(GPU.CLUT_Cache, 0xAA, sizeof(GPU.CLUT_Cache));
   GPU.CLUT_Cache_VB = 0x1234;
   memset(GPU.TexCache, 0xBB, sizeof(GPU.TexCache));
   GPU.last_tex_line = 0;
   GPU.last_tex_c = &GPU.TexCache[0];
}

static void check_caches(bool restored)
{
   unsigned i, j;
   check("CLUT tag", GPU.CLUT_Cache_VB == (restored ? 0x4567 : ~0U));
   check("derived texture pointer invalidated", GPU.last_tex_line == ~0U && !GPU.last_tex_c);
   for (i = 0; i < 256; i++)
   {
      check("texture tag", GPU.TexCache[i].Tag == (restored ? i * 4 : ~0U));
      if (restored)
      {
         check("latched palette", GPU.CLUT_Cache[i] == (0x1111 ^ i));
         for (j = 0; j < 4; j++)
            check("latched texture", GPU.TexCache[i].Data[j] == (0x2222 ^ (i * 4 + j)));
      }
   }
}

static void check_restored(unsigned pending, unsigned u, unsigned v, bool possible_2d)
{
   check("pending command", GPU.InCmd == (pending ? INCMD_QUAD : INCMD_NONE));
   check("U offset", GPU.off_u == u);
   check("V offset", GPU.off_v == v);
   check("possible 2D", GPU.may_be_2d == possible_2d);
   check("last older entry", GPU.DrawTimeAvail == 123456);
   check("saved PGXP W", GPU.InQuad_F3Vertices[0].precise[2] == 12.5f);
   check("VRAM payload", vram[123] == 0x4321);
}

int main(void)
{
   unsigned fast, pending, bits, i, j;
   for (fast = 0; fast < 2; fast++)
      for (pending = 0; pending < 2; pending++)
         for (bits = 0; bits < 8; bits++)
         {
            StateMem saved = {0}, legacy, cache_legacy;
            FastSaveStates = fast != 0;
            memset(&GPU, 0, sizeof(GPU));
            GPU.vram = vram;
            GPU.InCmd = pending ? INCMD_QUAD : INCMD_NONE;
            GPU.off_u = bits & 1;
            GPU.off_v = (bits >> 1) & 1;
            GPU.may_be_2d = (bits & 4) != 0;
            GPU.DrawTimeAvail = 123456;
            GPU.InQuad_F3Vertices[0].precise[2] = 12.5f;
            vram[123] = 0x4321;
            GPU.CLUT_Cache_VB = 0x4567;
            for (i = 0; i < 256; i++)
            {
               GPU.CLUT_Cache[i] = 0x1111 ^ i;
               GPU.TexCache[i].Tag = i * 4;
               for (j = 0; j < 4; j++)
                  GPU.TexCache[i].Data[j] = 0x2222 ^ (i * 4 + j);
            }
            if (!GPU_StateAction(&saved, 0, 0))
               return 2;
            legacy = legacy_chunk(&saved, 7);
            cache_legacy = legacy_chunk(&saved, 4);

            poison();
            saved.loc = 0;
            cases++;
            check("current round trip", GPU_StateAction(&saved, 1, 0) != 0);
            check_restored(pending, pending ? bits & 1 : 0, pending ? (bits >> 1) & 1 : 0,
                           pending && (bits & 4));
            check_caches(true);

            poison();
            cases++;
            check("pre-cache chunk loads", GPU_StateAction(&cache_legacy, 1, 0) != 0);
            check_restored(pending, pending ? bits & 1 : 0, pending ? (bits >> 1) & 1 : 0,
                           pending && (bits & 4));
            check_caches(false);

            /* A successful earlier restore must not supply missing fields. */
            poison();
            cases++;
            check("older chunk loads", GPU_StateAction(&legacy, 1, 0) != 0);
            check_restored(pending, 0, 0, false);
            check_caches(false);

            /* A truncated cache payload must fail without committing the
             * pending-quad locals. */
            poison();
            saved.loc = 0;
            saved.len--;
            cases++;
            check("truncated chunk fails", GPU_StateAction(&saved, 1, 0) == 0);
            check("failed load keeps U", GPU.off_u == 0xaaaa);
            check("failed load keeps V", GPU.off_v == 0xbbbb);
            check("failed load keeps flag", GPU.may_be_2d);

            free(legacy.data);
            free(cache_legacy.data);
            free(saved.data);
         }

   poison();
   GPU_Power();
   cases++;
   check("power clears command", GPU.InCmd == INCMD_NONE);
   check("power clears U", GPU.off_u == 0);
   check("power clears V", GPU.off_v == 0);
   check("power clears flag", !GPU.may_be_2d);
   check_caches(false);
   printf("GPU UV serialization: %u cases, %u assertions, %u failures\n", cases,
          assertions, failures);
   return failures ? 1 : 0;
}

/* No GPU context is created. These callbacks are leaf notifications only. */
bool rhi_intf_has_software_renderer(void)
{
   return true;
}
void rhi_intf_set_tex_window(uint8_t w, uint8_t h, uint8_t x, uint8_t y)
{
   (void)w;
   (void)h;
   (void)x;
   (void)y;
}
void rhi_intf_invalidate_clut_cache(void)
{
}
void rhi_intf_toggle_display(bool off)
{
   (void)off;
}
void rhi_intf_set_draw_area(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
   (void)x0;
   (void)y0;
   (void)x1;
   (void)y1;
}
void rhi_intf_load_image(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *pixels,
                         bool mask, bool set_mask)
{
   (void)x;
   (void)y;
   (void)w;
   (void)h;
   (void)pixels;
   (void)mask;
   (void)set_mask;
}
bool rhi_intf_read_vram(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *pixels)
{
   (void)x;
   (void)y;
   (void)w;
   (void)h;
   (void)pixels;
   return true;
}
void rhi_intf_set_vram_framebuffer_coords(uint32_t x, uint32_t y)
{
   (void)x;
   (void)y;
}
void rhi_intf_set_horizontal_display_range(uint16_t x, uint16_t y)
{
   (void)x;
   (void)y;
}
void rhi_intf_set_vertical_display_range(uint16_t x, uint16_t y)
{
   (void)x;
   (void)y;
}
void rhi_intf_set_display_mode(bool depth, bool pal, bool interlaced, int width)
{
   (void)depth;
   (void)pal;
   (void)interlaced;
   (void)width;
}
void PSX_EventVirtual(const int type, bool virt)
{
   (void)type;
   (void)virt;
}
void IRQ_Assert(int which, bool asserted)
{
   (void)which;
   (void)asserted;
}
void MDFN_FASTCALL TIMER_SetVBlank(bool blank)
{
   (void)blank;
}

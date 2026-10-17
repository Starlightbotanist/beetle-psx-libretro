#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mednafen/psx/gpu.h"
#include "rhi/rhi_intf.h"

uint8_t psx_gpu_upscale_shift_hw;
int line_render_mode;
static enum rhi_renderer_type renderer;
static unsigned failures, assertions, cases, varying_depth;
static bool dump_matrix;
static int32_t result[13], first_result[3];
static unsigned last_kind, last_order, last_scale, last_flip;

static void check(const char *name, int32_t actual, int32_t expected)
{
   assertions++;
   if (actual != expected)
   {
      if (failures < 12)
         fprintf(stderr, "%s kind=%u order=%u scale=%u flip=%u: got %d, expected %d\n",
                 name, last_kind, last_order, last_scale, last_flip, actual, expected);
      failures++;
   }
}

/* Kinds 0..12 and their iteration order preserve the original 12,736-case
 * differential fixture. The additional kinds exercise collapsed UV mapping.
 * 0 rectangle, 1 one-pixel artwork, 2/3 oblique, 4/5 culled halves,
 * 6/7 degenerate halves, 8 collapsed second UV, 9 varying saved depth,
 * 10/11 oblique degenerate halves, 12 oblique culled half,
 * 13 nonseparable first UV + collapsed second, 14 both UVs collapsed,
 * 15 one-pixel artwork + collapsed second UV. */
enum rhi_renderer_type rhi_intf_is_type(void)
{
   return renderer;
}
void Calc_UVOffsets_Adjust_Verts(PS_GPU *, tri_vertex *, unsigned);
void Reset_UVLimits(PS_GPU *);
void Extend_UVLimits(PS_GPU *, tri_vertex *, unsigned);
void Finalise_UVLimits(PS_GPU *);

static void test(unsigned id, unsigned kind, unsigned scale, unsigned flip,
                 unsigned order, unsigned window, unsigned stale)
{
   PS_GPU gpu;
   tri_vertex quad[4], first[3], second[3];
   unsigned j;
   static const unsigned permutation[8][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {2, 3, 0, 1},
                                              {3, 2, 1, 0}, {0, 2, 1, 3}, {2, 0, 3, 1},
                                              {1, 3, 0, 2}, {3, 1, 2, 0}};
   last_kind = kind;
   last_order = order;
   last_scale = scale;
   last_flip = flip;
   memset(&gpu, 0, sizeof(gpu));
   memset(quad, 0, sizeof(quad));
   gpu.upscale_shift = renderer == RHI_SOFTWARE ? scale : 0;
   psx_gpu_upscale_shift_hw = scale;
   gpu.SUCV.TWX_AND = gpu.SUCV.TWY_AND = window ? 0xf7 : 0xff;
   for (j = 0; j < 4; j++)
   {
      unsigned k = permutation[order][j];
      quad[j].x = (16 + (k & 1) * 31) << gpu.upscale_shift;
      quad[j].y = (32 + (k >> 1) * ((kind == 1 || kind == 15) ? 1 : 23))
                  << gpu.upscale_shift;
      quad[j].u = ((k & 1) != (flip & 1) ? 31 : 0);
      quad[j].v = ((k >> 1) != ((flip >> 1) & 1) ? 31 : 0);
      if (kind == 2 || kind == 3 || (kind >= 10 && kind <= 12))
         quad[j].x += (k >> 1) * (kind == 2 ? 3 : 1) << gpu.upscale_shift;
      quad[j].precise[0] = quad[j].x;
      quad[j].precise[1] = quad[j].y;
      quad[j].precise[2] = varying_depth == j + 1 ? 2.0f : 1.0f;
   }
   if (kind == 4 || kind == 12)
   {
      quad[3].x = -(1024 << gpu.upscale_shift);
   }
   if (kind == 5)
   {
      quad[0].x = -(1024 << gpu.upscale_shift);
      gpu.killQuadPart = 1;
   }
   if (kind == 6)
   {
      quad[3].x = quad[2].x;
      quad[3].y = quad[2].y;
   }
   if (kind == 7)
   {
      quad[0].x = quad[1].x;
      quad[0].y = quad[1].y;
   }
   if (kind == 13)
   {
      quad[2].u += 3;
   }
   if (kind == 8 || kind == 13 || kind == 14 || kind == 15)
   {
      quad[3].u = quad[2].u;
      quad[3].v = quad[2].v;
   }
   if (kind == 14)
   {
      for (j = 0; j < 4; j++)
         quad[j].v = 0;
   }
   if (kind == 9)
   {
      quad[0].precise[2] = 2.0f;
   }
   if (kind == 10)
   {
      quad[3] = quad[2];
   }
   if (kind == 11)
   {
      quad[0] = quad[1];
   }
   memcpy(first, quad, sizeof(first));
   memcpy(gpu.InQuad_F3Vertices + 1, quad + 1, sizeof(tri_vertex) * 2);
   Calc_UVOffsets_Adjust_Verts(&gpu, first, 4);
   first_result[0] = gpu.off_u;
   first_result[1] = gpu.off_v;
   first_result[2] = gpu.may_be_2d;
   gpu.InQuad_F3Vertices[0] = first[0];
   gpu.InCmd = INCMD_QUAD;
   if (stale)
   {
      gpu.off_u = stale & 1;
      gpu.off_v = (stale >> 1) & 1;
      gpu.may_be_2d = (stale >> 2) & 1;
   }
   memcpy(second, quad + 1, sizeof(second));
   Calc_UVOffsets_Adjust_Verts(&gpu, second, 4);
   Reset_UVLimits(&gpu);
   Extend_UVLimits(&gpu, gpu.InQuad_F3Vertices, 1);
   Extend_UVLimits(&gpu, second, 3);
   Finalise_UVLimits(&gpu);
   result[0] = gpu.off_u;
   result[1] = gpu.off_v;
   result[2] = gpu.may_be_2d;
   result[3] = gpu.min_u;
   result[4] = gpu.min_v;
   result[5] = gpu.max_u;
   result[6] = gpu.max_v;
   for (j = 0; j < 3; j++)
   {
      result[7 + j * 2] = first[j].v;
      result[8 + j * 2] = second[j].v;
   }
   cases++;
   if (dump_matrix)
   {
      printf("%u %u %u %u %u %u %u %u :", id, kind, renderer, scale, flip, order, window,
             stale);
      for (j = 0; j < 13; j++)
         printf(" %d", result[j]);
      printf("\n");
   }
}

static void check_flags(unsigned flip, bool sprite, unsigned scale)
{
   unsigned u = sprite && (renderer != RHI_VULKAN || scale) && (flip & 1);
   unsigned v = sprite && (renderer != RHI_VULKAN || scale) && (flip & 2);
   check("U offset", result[0], u ? 1 : 0);
   check("V offset", result[1], v ? 1 : 0);
   check("possible 2D", result[2], sprite ? 1 : 0);
}

int main(int argc, char **argv)
{
   unsigned id = 0, kind, scale, flip, order, window, stale, r, depth, j;
   if (argc > 1 && !strcmp(argv[1], "--depth-matrix"))
   {
      for (r = 1; r < 3; r++)
         for (scale = 0; scale <= 2; scale += 2)
            for (kind = 0; kind < 2; kind++)
               for (flip = 0; flip < 4; flip++)
                  for (depth = 0; depth < 5; depth++)
                  {
                     renderer = (enum rhi_renderer_type)r;
                     varying_depth = depth;
                     test(id++, kind ? 8 : 0, scale, flip, 0, 0, 0);
                     printf("%u %u %u %u %u : %d %d %d\n", r, scale, kind, flip, depth,
                            result[0], result[1], result[2]);
                  }
      return 0;
   }
   dump_matrix = argc > 1 && !strcmp(argv[1], "--matrix");
   for (r = 0; r < 3; r++)
      for (scale = 0; scale < 5; scale++)
         for (kind = 0; kind < 13; kind++)
            for (flip = 0; flip < 4; flip++)
               for (order = 0; order < 8; order++)
                  for (window = 0; window < 2; window++)
                  {
                     renderer = (enum rhi_renderer_type)r;
                     test(id++, kind, scale, flip, order, window, 0);
                     if (!dump_matrix && r != RHI_SOFTWARE)
                     {
                        if (kind == 0 || kind == 1 || kind == 8)
                           check_flags(flip, true, scale);
                        if (kind == 2 || kind == 3 || kind == 9 || kind == 10 ||
                            kind == 11 || kind == 12)
                           check_flags(flip, false, scale);
                     }
                  }
   for (r = 1; r < 3; r++)
      for (kind = 0; kind < 4; kind++)
         for (flip = 0; flip < 4; flip++)
            for (stale = 0; stale < 8; stale++)
            {
               renderer = (enum rhi_renderer_type)r;
               test(id++, kind, 2, flip, 0, 0, stale);
               if (!dump_matrix)
                  check_flags(flip, kind == 0 || kind == 1, 2);
            }
   if (dump_matrix)
      return 0;
   for (r = 1; r < 3; r++)
      for (scale = 0; scale <= 2; scale += 2)
         for (kind = 0; kind < 2; kind++)
            for (flip = 0; flip < 4; flip++)
               for (depth = 0; depth < 5; depth++)
               {
                  renderer = (enum rhi_renderer_type)r;
                  varying_depth = depth;
                  test(id++, kind ? 8 : 0, scale, flip, 0, 0, 0);
                  check_flags(flip, depth == 0, scale);
               }
   varying_depth = 0;
   for (r = 1; r < 3; r++)
      for (kind = 8; kind <= 15; kind++)
         if (kind == 8 || kind >= 13)
            for (flip = 0; flip < 4; flip++)
               for (order = 0; order < 8; order++)
               {
                  renderer = (enum rhi_renderer_type)r;
                  test(id++, kind, 2, flip, order, 0, 0);
                  /* The cached first mapping is part of a valid pending command.
                   * Actual serialization continuation is tested with the PS-X fixture. */
                  for (j = 0; j < 3; j++)
                     check("pending first mapping", result[j], first_result[j]);
                  if (kind == 8 || kind == 15)
                     check_flags(flip, true, 2);
                  if (kind == 14)
                     check_flags(flip, false, 2);
               }
   printf("GPU UV geometry: %u cases, %u assertions, %u failures\n", cases, assertions,
          failures);
   return failures ? 1 : 0;
}

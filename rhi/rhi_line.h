#ifndef BEETLE_PSX_RHI_LINE_H
#define BEETLE_PSX_RHI_LINE_H

#include <math.h>
#include <retro_inline.h>

typedef struct
{
   float x;
   float y;
   unsigned source;
} rhi_line_quad_vertex;

/* PS1 lines have endpoint and diagonal coverage rules which native host line
 * primitives do not reproduce consistently. Keep the expansion policy shared
 * so renderer backends cannot drift while retaining their own vertex formats. */
static INLINE void rhi_build_line_quad(rhi_line_quad_vertex output[4],
      float x0, float y0, float x1, float y1)
{
   float dx = x1 - x0;
   float dy = y1 - y0;
   float fill_dx;
   float fill_dy;
   float pad_x0 = 0.0f;
   float pad_x1 = 0.0f;
   float pad_y0 = 0.0f;
   float pad_y1 = 0.0f;

   output[0].source = 0;
   output[1].source = 0;

   if (dx == 0.0f && dy == 0.0f)
   {
      output[2].source = 0;
      output[3].source = 0;
      output[0].x = x0;
      output[0].y = y0;
      output[1].x = x0 + 1.0f;
      output[1].y = y0;
      output[2].x = x1;
      output[2].y = y1 + 1.0f;
      output[3].x = x1 + 1.0f;
      output[3].y = y1 + 1.0f;
      return;
   }

   output[2].source = 1;
   output[3].source = 1;
   if (fabsf(dx) > fabsf(dy))
   {
      float slope = dy / fabsf(dx);
      fill_dx = 0.0f;
      fill_dy = 1.0f;
      if (dx > 0.0f)
      {
         pad_x1 = 1.0f;
         pad_y1 = slope;
      }
      else
      {
         pad_x0 = 1.0f;
         pad_y0 = -slope;
      }
   }
   else
   {
      float slope = dx / fabsf(dy);
      fill_dx = 1.0f;
      fill_dy = 0.0f;
      if (dy > 0.0f)
      {
         pad_y1 = 1.0f;
         pad_x1 = slope;
      }
      else
      {
         pad_y0 = 1.0f;
         pad_x0 = -slope;
      }
   }

   x0 += pad_x0;
   y0 += pad_y0;
   x1 += pad_x1;
   y1 += pad_y1;
   output[0].x = x0;
   output[0].y = y0;
   output[1].x = x0 + fill_dx;
   output[1].y = y0 + fill_dy;
   output[2].x = x1;
   output[2].y = y1;
   output[3].x = x1 + fill_dx;
   output[3].y = y1 + fill_dy;
}

#endif

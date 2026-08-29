#include <stdio.h>

#include "rhi/rhi_line.h"

static int failures;

static void check_vertex(const char* name,
      const rhi_line_quad_vertex* vertex,
      float x, float y, unsigned source)
{
   if (vertex->x == x && vertex->y == y && vertex->source == source)
      return;
   printf("%s: got (%g, %g, %u), expected (%g, %g, %u)\n",
      name, vertex->x, vertex->y, vertex->source, x, y, source);
   failures++;
}

static void check_quad(const char* name,
      float x0, float y0, float x1, float y1,
      const float expected[4][2], const unsigned sources[4])
{
   rhi_line_quad_vertex quad[4];
   unsigned i;

   rhi_build_line_quad(quad, x0, y0, x1, y1);
   for (i = 0; i < 4; i++)
   {
      char label[64];

      snprintf(label, sizeof(label), "%s vertex %u", name, i);
      check_vertex(label, &quad[i], expected[i][0], expected[i][1],
         sources[i]);
   }
}

int main(void)
{
   static const unsigned line_sources[4] = {0, 0, 1, 1};
   static const unsigned point_sources[4] = {0, 0, 0, 0};
   static const float point[4][2] = {
      {3.0f, 4.0f}, {4.0f, 4.0f}, {3.0f, 5.0f}, {4.0f, 5.0f}
   };
   static const float right[4][2] = {
      {0.0f, 0.0f}, {0.0f, 1.0f}, {5.0f, 0.0f}, {5.0f, 1.0f}
   };
   static const float left[4][2] = {
      {5.0f, 0.0f}, {5.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 1.0f}
   };
   static const float down[4][2] = {
      {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 5.0f}, {1.0f, 5.0f}
   };
   static const float up[4][2] = {
      {0.0f, 5.0f}, {1.0f, 5.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}
   };
   static const float diagonal_x[4][2] = {
      {0.0f, 0.0f}, {0.0f, 1.0f}, {5.0f, 2.5f}, {5.0f, 3.5f}
   };
   static const float diagonal_y[4][2] = {
      {0.0f, 0.0f}, {1.0f, 0.0f}, {2.5f, 5.0f}, {3.5f, 5.0f}
   };

   check_quad("point", 3.0f, 4.0f, 3.0f, 4.0f,
      point, point_sources);
   check_quad("right", 0.0f, 0.0f, 4.0f, 0.0f,
      right, line_sources);
   check_quad("left", 4.0f, 0.0f, 0.0f, 0.0f,
      left, line_sources);
   check_quad("down", 0.0f, 0.0f, 0.0f, 4.0f,
      down, line_sources);
   check_quad("up", 0.0f, 4.0f, 0.0f, 0.0f,
      up, line_sources);
   check_quad("diagonal-x", 0.0f, 0.0f, 4.0f, 2.0f,
      diagonal_x, line_sources);
   check_quad("diagonal-y", 0.0f, 0.0f, 2.0f, 4.0f,
      diagonal_y, line_sources);

   if (failures)
   {
      printf("failures=%d\nFAIL\n", failures);
      return 1;
   }
   printf("FAIL count 0\nPASS\n");
   return 0;
}

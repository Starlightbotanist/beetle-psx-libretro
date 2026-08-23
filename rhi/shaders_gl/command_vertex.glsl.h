#include "shaders_common.h"

#undef command_vertex_name_
#if defined(FILTER_SABR) || defined(FILTER_XBR)
#define command_vertex_name_ command_vertex_xbr
#else
#define command_vertex_name_ command_vertex
#endif

static const char * command_vertex_name_ = GLSL_VERTEX(
// Vertex shader for rendering GPU draw commands in the framebuffer
in vec4 position;
in vec3 color;
in vec4 fog;
in uvec2 texture_page;
in uvec2 texture_coord;
in vec2 coverage_texture_coord;
in vec2 coverage_original_barycentric;
in uint coverage_preserve;
in uvec2 clut;
in uint texture_blend_mode;
in uint depth_shift;
in uint dither;
in uint semi_transparent;
in uvec4 texture_window;
in uvec4 texture_limits;
in uint framebuffer_feedback;

// Drawing offset
uniform ivec2 offset;
// Diagnostic-only OpenGL raster parity transform. Mode zero is the exact
// release path. raster_grid is internal-scale * 2^GL_SUBPIXEL_BITS.
uniform uint pgxp_raster_mode;
uniform float pgxp_raster_grid;

out vec3 frag_shading_color;
out vec4 frag_fog;
flat out uvec2 frag_texture_page;
out vec2 frag_texture_coord;
out vec2 frag_coverage_original_barycentric;
flat out uint frag_coverage_preserve;
flat out uvec2 frag_clut;
flat out uint frag_texture_blend_mode;
flat out uint frag_depth_shift;
flat out uint frag_dither;
flat out uint frag_semi_transparent;
flat out uvec4 frag_texture_window;
flat out uvec4 frag_texture_limits;
flat out uint frag_framebuffer_feedback;
)

#if defined(FILTER_SABR) || defined(FILTER_XBR)
STRINGIZE(
out vec2 tc;
out vec4 xyp_1_2_3;
out vec4 xyp_6_7_8;
out vec4 xyp_11_12_13;
out vec4 xyp_16_17_18;
out vec4 xyp_21_22_23;
out vec4 xyp_5_10_15;
out vec4 xyp_9_14_9;
)
#endif
STRINGIZE(
void main() {
   vec2 pos = position.xy + vec2(offset);

   // Modes 17/18/22 explicitly choose how precise PGXP positions land on
   // the fixed-point grid which OpenGL reports through GL_SUBPIXEL_BITS.
   // Mode 19 moves the sample phase by half of one such unit without
   // changing relative geometry.
   if (pgxp_raster_mode == 17U || pgxp_raster_mode == 22U)
      pos = floor(pos * pgxp_raster_grid + vec2(0.5)) / pgxp_raster_grid;
   else if (pgxp_raster_mode == 18U)
      pos = floor(pos * pgxp_raster_grid) / pgxp_raster_grid;
   else if (pgxp_raster_mode == 19U)
      pos.y += 0.5 / pgxp_raster_grid;

   // Convert VRAM coordinates (0;1023, 0;511) into OpenGL coordinates
   // (-1;1, -1;1)
   float wpos = position.w;
   float xpos;
   float ypos;
   // Mode 34 duplicates Vulkan primitive.vert's operation order exactly.
   // It is algebraically equivalent to the ordinary GL path, but keeps a
   // final concrete shader/backend difference measurable on the live driver.
   if (pgxp_raster_mode == 34U)
   {
      xpos = pos.x / 1024.0 * 2.0 - 1.0;
      ypos = pos.y / 512.0 * 2.0 - 1.0;
   }
   else
   {
      xpos = (pos.x / 512.0) - 1.0;
      ypos = (pos.y / 256.0) - 1.0;
   }

   // SwanStation carries a small OpenGL Y epsilon for driver raster parity.
   // The upper-left modes negate clip-space Y while glClipControl changes
   // the origin, leaving the physical framebuffer position unchanged but
   // selecting the API's upper-left edge convention.
   if (pgxp_raster_mode == 20U || pgxp_raster_mode == 21U ||
       pgxp_raster_mode == 22U)
      ypos = -ypos;
   if (pgxp_raster_mode == 15U || pgxp_raster_mode == 21U)
      ypos += 0.00001;
   else if (pgxp_raster_mode == 16U)
      ypos -= 0.00001;

   // position.z increases as the primitives near the camera so we
   // reverse the order to match the common GL convention
   float zpos = 1.0 - (position.z / 32768.);

   gl_Position.xyzw = vec4(xpos * wpos, ypos * wpos, zpos * wpos, wpos);
   //gl_Position.xyzw = vec4(xpos, ypos, zpos, 1.);

   // Glium doesn't support 'normalized' for now
   /* Already in 0..1+ scale (1.0 == 0xFF). May exceed 1.0 when the PGXP
   // precise-colour path supplies the GTE's pre-saturation value. */
   frag_shading_color = color;
   frag_fog = fog;

   // Let OpenGL interpolate the texel position
   frag_texture_coord = (coverage_preserve != 0U ?
         coverage_texture_coord : vec2(texture_coord)) +
      vec2(0.001, 0.001);
   // Smooth interpolation divides attributes by clip W.  Pre-multiply the
   // original-triangle barycentrics so multiplying by gl_FragCoord.w in the
   // fragment shader recovers their affine screen-space values exactly.
   frag_coverage_original_barycentric =
      coverage_original_barycentric * position.w;
   frag_coverage_preserve = coverage_preserve;

   frag_texture_page = texture_page;
   frag_clut = clut;
   frag_texture_blend_mode = texture_blend_mode;
   frag_depth_shift = depth_shift;
   frag_dither = dither;
   frag_semi_transparent = semi_transparent;
   frag_texture_window = texture_window;
   frag_texture_limits = texture_limits;
   frag_framebuffer_feedback = framebuffer_feedback;
)
#if defined(FILTER_SABR) || defined(FILTER_XBR)
STRINGIZE(
	tc = frag_texture_coord.xy;
   xyp_1_2_3    = tc.xxxy + vec4(-1.,  0., 1., -2.);
   xyp_6_7_8    = tc.xxxy + vec4(-1.,  0., 1., -1.);
   xyp_11_12_13 = tc.xxxy + vec4(-1.,  0., 1.,  0.);
   xyp_16_17_18 = tc.xxxy + vec4(-1.,  0., 1.,  1.);
   xyp_21_22_23 = tc.xxxy + vec4(-1.,  0., 1.,  2.);
   xyp_5_10_15  = tc.xyyy + vec4(-2., -1., 0.,  1.);
   xyp_9_14_9   = tc.xyyy + vec4( 2., -1., 0.,  1.);
)
#endif
STRINGIZE(
}
);

#undef command_vertex_name_

#include "shaders_common.h"

static const char *image_load_fragment = GLSL_FRAGMENT(
      uniform highp sampler2D fb_texture;
      uniform uint internal_upscaling;
      // 1: RGB5 upload to q*8; 2: q*8 to RGB5; 3: normalized HDR to RGB5.
      // 4: RGB5 upload to full-range 32-bpp bytes.
      uniform uint color_conversion;
      // Uploads rebuild the derived stencil from the authoritative mask bit.
      // 0: color copy, 1: protected pixels for the stencil-only pass.
      uniform uint mask_pass;
      // Copies retain every scaled texel; uploads address native VRAM words.
      // 0: upload/resolve, 1: copy, 2: copy with the mask bit forced on.
      uniform uint copy_mode;
      uniform ivec2 copy_offset;
      uniform uvec2 copy_upscaling;
      in vec2 frag_fb_coord;
      out vec4 frag_color;

      // Read a pixel in VRAM
      vec4 vram_get_pixel(int x, int y) {
	return texelFetch(fb_texture, ivec2(x, y) * int(internal_upscaling), 0);
      }

      void main() {
      // GLES 5551 note: color reinterpretation can be done here
      if (copy_mode != 0u) {
         ivec2 position = ivec2(gl_FragCoord.xy);
         if (copy_upscaling.x != copy_upscaling.y)
            position = position / int(copy_upscaling.y) * int(copy_upscaling.x);
         frag_color = texelFetch(fb_texture,
               position + copy_offset * int(copy_upscaling.x), 0);
         if (copy_mode == 2u)
            frag_color.a = 1.0;
      }
      else
         frag_color = vram_get_pixel(int(frag_fb_coord.x), int(frag_fb_coord.y));
      if (mask_pass != 0u && frag_color.a <= 0.5)
         discard;
      if (color_conversion == 1u)
         frag_color.rgb = floor(frag_color.rgb * 31.0 + vec3(0.5)) * (8.0 / 255.0);
      if (color_conversion == 4u) {
         highp vec3 color5 = floor(frag_color.rgb * 31.0 + vec3(0.5));
         frag_color.rgb = floor(color5 * (255.0 / 31.0) + vec3(0.5)) / 255.0;
      }
      else if (color_conversion >= 2u) {
         // HDR byte/float targets use the nearest RGB5 rule from CPU readback.
         // Write on that code, rather than near a driver conversion threshold.
         highp float scale = color_conversion == 2u ? 255.0 / 8.0 : 31.0;
         highp float bias = color_conversion == 2u ? 0.001 : 0.5;
         frag_color.rgb = clamp(floor(frag_color.rgb * scale + vec3(bias)),
               vec3(0.0), vec3(31.0)) / 31.0;
      }
      }
);

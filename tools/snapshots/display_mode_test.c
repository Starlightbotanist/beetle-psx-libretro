/* Renderer registers rewind; frontend display policy advances only on retained
 * guest writes. Exercise the shipping dispatcher without graphics drivers. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define HAVE_OPENGL
#define HAVE_VULKAN
#define MEDNAFEN_CORE_GEOMETRY_BASE_H 240
enum { RHI_SOFTWARE, RHI_OPENGL, RHI_VULKAN };
static int rhi_type = RHI_OPENGL;
static unsigned startup_frame_count;
static int image_height = 240, rhi_width_mode, rhi_height_mode;
static bool currently_interlaced = true, interlace_setting_dirty, aspect_ratio_dirty;
static bool restoring, speculative;
static unsigned propagated;
static bool renderer_interlaced;
static int renderer_width;
static bool retro_using_memory_snapshots(void) { return restoring; }
static bool retro_is_speculative(void) { return speculative; }
static void mode(bool depth, bool pal, bool interlaced, int width)
{
   (void)depth; (void)pal;
   propagated++;
   renderer_interlaced = interlaced;
   renderer_width = width;
}
#define rhi_gl_set_display_mode mode
#define rhi_vulkan_set_display_mode mode
#include "build/display_mode_fragment.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"display_mode_test:%u: %s\n",__LINE__,#x); exit(1); } } while (0)
int main(void)
{
   unsigned i;
   restoring = true;
   for (i = 0; i < 100; i++) rhi_intf_set_display_mode(false, false, false, 1);
   CHECK(startup_frame_count == 0 && !interlace_setting_dirty && !aspect_ratio_dirty);
   CHECK(propagated == 100 && renderer_width == 1 && !renderer_interlaced);
   restoring = false; speculative = true; rhi_type = RHI_VULKAN;
   for (i = 0; i < 100; i++) rhi_intf_set_display_mode(false, false, true, 2);
   CHECK(startup_frame_count == 0 && !interlace_setting_dirty && !aspect_ratio_dirty);
   CHECK(propagated == 200 && renderer_width == 2 && renderer_interlaced);
   speculative = false;
   for (i = 0; i < 20; i++) rhi_intf_set_display_mode(false, false, false, 1);
   CHECK(startup_frame_count == 20 && !interlace_setting_dirty);
   rhi_intf_set_display_mode(false, false, false, 1);
   CHECK(!currently_interlaced && interlace_setting_dirty && aspect_ratio_dirty);
   interlace_setting_dirty = aspect_ratio_dirty = false;
   restoring = true;
   rhi_intf_set_display_mode(false, false, true, 2);
   CHECK(!currently_interlaced && rhi_width_mode == 1 && !interlace_setting_dirty && !aspect_ratio_dirty);
   CHECK(renderer_interlaced && renderer_width == 2);
   /* Ordinary loading still publishes its restored mode. */
   restoring = false;
   rhi_intf_set_display_mode(false, false, true, 2);
   CHECK(currently_interlaced && rhi_width_mode == 2 && interlace_setting_dirty && aspect_ratio_dirty);
   rhi_type = RHI_SOFTWARE; image_height = 480;
   rhi_intf_set_display_mode(false, false, false, 2);
   CHECK(currently_interlaced);
   puts("display_mode_test: retained, restored, speculative and ordinary paths passed");
   return 0;
}

/* CPU-only doubles surround helpers extracted from the production source.
 * Renderer deliberately has no gameplay queue or CommandBufferHandle: capture
 * code that starts using either again fails to compile this harness. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define INLINE inline
#define RHI_INLINE static inline
#define VK_ASSERT assert
#define LOGE(...) ((void)0)
#define LOGI(...) ((void)0)
#define VULKAN_NUM_SPEC_CONSTANTS 10
#define VULKAN_NUM_VERTEX_ATTRIBS 16
#define VULKAN_NUM_VERTEX_BUFFERS 16
#define VULKAN_NUM_ATTACHMENTS 8
#define COMPARE_OP_BITS 3
#define BLEND_FACTOR_BITS 5
#define BLEND_OP_BITS 3
#define CULL_MODE_BITS 2
#define COMMAND_BUFFER_DIRTY_STATIC_STATE_BIT 1u
#define COMMAND_BUFFER_DIRTY_STATIC_VERTEX_BIT 2u
#define RHI_CABLE_NONE 0
#define RHI_CABLE_SVIDEO 1
#define RHI_CABLE_COMPOSITE 2
#define RHI_CABLE_RF 3
#define RHI_CABLE_RGB 4
#define RHI_DITHER_OFF 0
#define COMBINER_NEEDS_BLEND_CONSTANT(f) \
   ((f) == VK_BLEND_FACTOR_CONSTANT_COLOR || (f) == VK_BLEND_FACTOR_CONSTANT_ALPHA)

typedef uint64_t Hash;
typedef struct Hasher Hasher;
typedef struct Program Program;
typedef struct State State;
typedef union PipelineState PipelineState;
typedef struct TextureWindow TextureWindow;
typedef struct BufferVertex BufferVertex;
typedef struct SemiTransparentState SemiTransparentState;
typedef enum SemiTransparentMode SemiTransparentMode;
typedef enum FilterMode FilterMode;
typedef struct { unsigned unused; } HdTextureHandle;
typedef enum ImageDomain ImageDomain;
typedef enum Layout Layout;
typedef uint32_t ImageMiscFlags;
typedef uint32_t RenderPassOpFlags;
typedef enum RenderPassInfo_DepthStencil RenderPassInfo_DepthStencil;
typedef struct ImageCreateInfo ImageCreateInfo;
typedef struct Image Image;
typedef struct ImageView ImageView;
typedef struct RenderPassInfo RenderPassInfo;
typedef struct RenderPassInfo_Subpass RenderPassInfo_Subpass;
#include "types_under_test.h"

typedef struct CombinedResourceLayout
{
   uint32_t combined_spec_constant_mask;
   uint32_t attribute_mask;
} CombinedResourceLayout;
typedef struct PipelineLayout { CombinedResourceLayout resources; } PipelineLayout;
struct Program
{
   struct { Hash key; } intrusive_node;
   PipelineLayout layout;
};
struct Image { ImageCreateInfo create_info; Layout layout_type; };
struct ImageView { struct { Image *image; VkFormat format; } info; };
typedef struct RenderPass
{
   struct { Hash key; } intrusive_node;
   VkFormat color_format;
} RenderPass;
typedef struct RenderPassMap { RenderPass items[128]; unsigned count; } RenderPassMap;
typedef struct Device
{
   RenderPassMap render_passes;
   bool precompile_plan_attempted;
   bool precompile_plan_complete;
   unsigned precompile_runtime_escapes;
} Device;
typedef struct VertexAttribState
{
   unsigned binding, format;
   VkDeviceSize offset;
} VertexAttribState;
typedef struct CommandBuffer
{
   VkCommandBuffer cmd;
   Device *device;
   Program *current_program;
   PipelineLayout *current_layout;
   const RenderPass *compatible_render_pass;
   unsigned current_subpass, active_vbos, dirty;
   PipelineState static_state;
   struct PotentialState potential_static_state;
   VertexAttribState attribs[VULKAN_NUM_VERTEX_ATTRIBS];
   struct
   {
      unsigned input_rates[VULKAN_NUM_VERTEX_BUFFERS];
      VkDeviceSize strides[VULKAN_NUM_VERTEX_BUFFERS];
      uintptr_t buffers[VULKAN_NUM_VERTEX_BUFFERS];
      VkDeviceSize offsets[VULKAN_NUM_VERTEX_BUFFERS];
   } vbo;
} CommandBuffer;
typedef struct Renderer
{
   Device *device;
   unsigned scaling, msaa;
   VkFormat scaled_fb_format;
   FilterMode primitive_filter_mode;
   bool scaled_uv_offset, precompile_configuration_valid;
   struct VulkanPrecompileConfiguration precompile_configuration;
   struct
   {
#define TEST_PROGRAM_FIELD(id, field, role) Program *field;
      RENDERER_PIPELINE_MANIFEST(TEST_PROGRAM_FIELD)
#undef TEST_PROGRAM_FIELD
   } pipelines;
} Renderer;

static bool vulkan_pipeline_precompiling = true;
static bool vulkan_shader_precompilation = true;
static bool super_sampling, adaptive_smoothing, mdec_yuv, show_vram, psx_hdr_active;
static int psx_video_cable, dither_mode, psx_hdr_sdr_eotf;
static int psx_hdr_overbright_hot, psx_pgxp_color, psx_pgxp_fog, psx_hdr_multipass;
static unsigned precompile_attempts;
static unsigned precompile_failures_remaining;
static struct
{
   struct VulkanPipelineRecipe *items;
   unsigned count, capacity;
   bool failed;
} vulkan_precompile_recipes;
static VkRenderPass render_pass_get_render_pass(const RenderPass *pass)
{ return (VkRenderPass)(uintptr_t)1; }

static unsigned test_ctz(uint32_t bits)
{
   unsigned n = 0;
   if (!bits) return 32;
   while (!(bits & 1u)) { bits >>= 1; n++; }
   return n;
}
#define trailing_zeroes test_ctz
static const CombinedResourceLayout *pipeline_layout_get_resource_layout(const PipelineLayout *layout)
{ return &layout->resources; }
#include "hash_under_test.h"

static bool format_has_depth_or_stencil_aspect(VkFormat format)
{ return format == VK_FORMAT_D32_SFLOAT; }
static VkFormat imageview_get_format(const ImageView *view) { return view->info.format; }
static const Image *imageview_get_image(const ImageView *view) { return view->info.image; }
static const ImageCreateInfo *image_get_create_info(const Image *image) { return &image->create_info; }
static Layout image_get_layout_type(const Image *image) { return image->layout_type; }
static VkFormat device_get_default_depth_format(Device *device) { return VK_FORMAT_D32_SFLOAT; }
static RenderPass *render_pass_map_find(RenderPassMap *map, Hash hash)
{
   unsigned i;
   for (i = 0; i < map->count; i++)
      if (map->items[i].intrusive_node.key == hash) return &map->items[i];
   return NULL;
}
static RenderPass *render_pass_map_emplace_yield(RenderPassMap *map, Hash hash,
      Device *device, const RenderPassInfo *info)
{
   RenderPass *pass;
   assert(map->count < sizeof(map->items) / sizeof(map->items[0]));
   pass = &map->items[map->count++];
   pass->intrusive_node.key = hash;
   pass->color_format = info->color_attachments[0]->info.format;
   return pass;
}
#include "renderpass_under_test.h"

static void commandbuffer_set_dirty(CommandBuffer *cmd, unsigned dirty) { cmd->dirty |= dirty; }
static void commandbuffer_set_program(CommandBuffer *cmd, Program *program)
{
   assert(program);
   cmd->current_program = program;
   cmd->current_layout = &program->layout;
}
static void commandbuffer_begin_graphics(CommandBuffer *cmd)
{ cmd->current_program = NULL; cmd->current_layout = NULL; }
static void commandbuffer_begin_compute(CommandBuffer *cmd)
{ cmd->current_program = NULL; cmd->current_layout = NULL; }

struct RecordedRecipe { Program *program; Hash hash; Hash render_pass; VkFormat format; };
static struct RecordedRecipe recorded[4096];
static unsigned recorded_count, recorded_calls;
static void record_recipe(CommandBuffer *cmd, bool graphics)
{
   Hash hash;
   unsigned i;
   assert(cmd->cmd == VK_NULL_HANDLE);
   assert(vulkan_pipeline_precompiling);
   hash = commandbuffer_pipeline_recipe_hash(cmd, graphics);
   recorded_calls++;
   for (i = 0; i < recorded_count; i++)
      if (recorded[i].program == cmd->current_program && recorded[i].hash == hash)
         return;
   assert(recorded_count < sizeof(recorded) / sizeof(recorded[0]));
   recorded[recorded_count].program = cmd->current_program;
   recorded[recorded_count].render_pass = graphics ? cmd->compatible_render_pass->intrusive_node.key : 0;
   recorded[recorded_count].format = graphics ? cmd->compatible_render_pass->color_format : VK_FORMAT_UNDEFINED;
   recorded[recorded_count++].hash = hash;
}
static void commandbuffer_draw(CommandBuffer *cmd, uint32_t v, uint32_t n, uint32_t first, uint32_t instance)
{ record_recipe(cmd, true); }
static void commandbuffer_dispatch(CommandBuffer *cmd, uint32_t x, uint32_t y, uint32_t z)
{ record_recipe(cmd, false); }
static bool renderer_precompile_current_configuration_pipelines(Renderer *self)
{
   precompile_attempts++;
   if (precompile_failures_remaining)
   {
      precompile_failures_remaining--;
      return false;
   }
   return true;
}
static VkFormat renderer_hdr_scanout_format(Renderer *self)
{ return VK_FORMAT_A2B10G10R10_UNORM_PACK32; }
#include "state_under_test.h"
#include "capture_under_test.h"

typedef struct IntrusivePODWrapperPipeline { VkPipeline value; } IntrusivePODWrapperPipeline;
typedef struct TestJob { Program *program; Hash hash; } TestJob;
static TestJob graphics_jobs[4], compute_jobs[4];
static struct { TestJob *items; unsigned count; }
   vulkan_graphics_precompile_jobs = { graphics_jobs, 0 },
   vulkan_compute_precompile_jobs = { compute_jobs, 0 };
static struct
{
   Program *program;
   Hash hash;
   IntrusivePODWrapperPipeline entry;
} published_jobs[4];
static unsigned published_count;
static IntrusivePODWrapperPipeline *program_find_pipeline(Program *program, Hash hash)
{
   unsigned i;
   for (i = 0; i < published_count; i++)
      if (published_jobs[i].program == program && published_jobs[i].hash == hash)
         return &published_jobs[i].entry;
   return NULL;
}
#include "validation_under_test.h"

static Device test_device;
static Program programs[64];
static void setup_renderer(Renderer *renderer)
{
   unsigned i = 0;
   memset(renderer, 0, sizeof(*renderer));
   memset(programs, 0, sizeof(programs));
   renderer->device = &test_device;
   renderer->scaling = 4;
   renderer->msaa = 1;
   renderer->scaled_fb_format = VK_FORMAT_R8G8B8A8_UNORM;
   renderer->scaled_uv_offset = true;
#define TEST_PROGRAM_INIT(id, field, role) \
   renderer->pipelines.field = &programs[i]; \
   programs[i].intrusive_node.key = i + 1; \
   programs[i].layout.resources.combined_spec_constant_mask = 0x3ff; \
   programs[i++].layout.resources.attribute_mask = 0x7f;
   RENDERER_PIPELINE_MANIFEST(TEST_PROGRAM_INIT)
#undef TEST_PROGRAM_INIT
   assert(i <= sizeof(programs) / sizeof(programs[0]));
   /* Test reflection exposes only constants consumed by each shader family:
    * fixed-function primitives have no MaskTest, feedback has no TransMode,
    * and flat vertices have no texture attributes. The shader compiler itself
    * is not part of this GPU-free harness. */
   renderer->pipelines.flat->layout.resources.attribute_mask = 0x43;
   renderer->pipelines.flat_masked->layout.resources.attribute_mask = 0x43;
   renderer->pipelines.flat_floor->layout.resources.attribute_mask = 1;
   renderer->pipelines.flat->layout.resources.combined_spec_constant_mask &= ~(1u << SpecConstIndex_MaskTest);
   renderer->pipelines.textured_scaled->layout.resources.combined_spec_constant_mask &= ~(1u << SpecConstIndex_MaskTest);
   renderer->pipelines.textured_unscaled->layout.resources.combined_spec_constant_mask &= ~(1u << SpecConstIndex_MaskTest);
   renderer->pipelines.flat_masked->layout.resources.combined_spec_constant_mask &= ~(1u << SpecConstIndex_TransMode);
   renderer->pipelines.textured_masked_scaled->layout.resources.combined_spec_constant_mask &= ~(1u << SpecConstIndex_TransMode);
   renderer->pipelines.textured_masked_unscaled->layout.resources.combined_spec_constant_mask &= ~(1u << SpecConstIndex_TransMode);
}
static void setup_command(CommandBuffer *cmd, Renderer *renderer)
{
   memset(cmd, 0, sizeof(*cmd));
   cmd->device = renderer->device;
   cmd->compatible_render_pass = renderer_precompile_render_pass(renderer,
         renderer->scaled_fb_format, true, false);
   commandbuffer_set_program(cmd, renderer->pipelines.flat);
}

static void test_canonical_identity(void)
{
   Renderer renderer;
   CommandBuffer base, changed;
   RenderPass other_pass;
   Hash original;
   setup_renderer(&renderer);
   setup_command(&base, &renderer);
   base.current_layout->resources.attribute_mask = 1;
   base.current_layout->resources.combined_spec_constant_mask = 1;
   base.static_state.state.spec_constant_mask = 1;
   base.potential_static_state.spec_constants[0] = 7;
   base.attribs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
   base.vbo.strides[0] = 16;
   original = commandbuffer_pipeline_recipe_hash(&base, true);
   changed = base;
   changed.static_state.state.spec_constant_mask = 0x3ff;
   changed.potential_static_state.spec_constants[8] = 123;
   changed.static_state.state.depth_write = 1;
   changed.static_state.state.depth_compare = VK_COMPARE_OP_GREATER;
   changed.static_state.state.src_color_blend = VK_BLEND_FACTOR_CONSTANT_COLOR;
   changed.static_state.state.dst_color_blend = VK_BLEND_FACTOR_ONE;
   changed.static_state.state.color_blend_op = VK_BLEND_OP_MAX;
   changed.potential_static_state.blend_constants[0] = 0.75f;
   changed.attribs[3].format = VK_FORMAT_R8_UNORM;
   changed.vbo.buffers[0] = 1234;
   changed.vbo.offsets[0] = 5678;
   assert(commandbuffer_pipeline_recipe_hash(&changed, true) == original);

#define EXPECT_GRAPHICS_CHANGE(statement) \
   changed = base; statement; assert(commandbuffer_pipeline_recipe_hash(&changed, true) != original)
   EXPECT_GRAPHICS_CHANGE(changed.potential_static_state.spec_constants[0]++);
   EXPECT_GRAPHICS_CHANGE(changed.static_state.state.spec_constant_mask = 0);
   EXPECT_GRAPHICS_CHANGE(changed.attribs[0].format = VK_FORMAT_R32G32_SFLOAT);
   EXPECT_GRAPHICS_CHANGE(changed.attribs[0].offset = 4);
   EXPECT_GRAPHICS_CHANGE(changed.vbo.strides[0] = 32);
   EXPECT_GRAPHICS_CHANGE(changed.vbo.input_rates[0] = VK_VERTEX_INPUT_RATE_INSTANCE);
   EXPECT_GRAPHICS_CHANGE(changed.current_subpass = 1);
   other_pass = *base.compatible_render_pass;
   other_pass.intrusive_node.key++;
   EXPECT_GRAPHICS_CHANGE(changed.compatible_render_pass = &other_pass);
   EXPECT_GRAPHICS_CHANGE(changed.static_state.state.depth_test = 1);
#undef EXPECT_GRAPHICS_CHANGE

   base.static_state.state.blend_enable = 1;
   base.static_state.state.src_color_blend = VK_BLEND_FACTOR_CONSTANT_COLOR;
   original = commandbuffer_pipeline_recipe_hash(&base, true);
   changed = base;
   changed.potential_static_state.blend_constants[0] = 0.5f;
   assert(commandbuffer_pipeline_recipe_hash(&changed, true) != original);
   original = commandbuffer_pipeline_recipe_hash(&base, false);
   changed = base;
   changed.compatible_render_pass = NULL;
   memset(&changed.attribs, 0xff, sizeof(changed.attribs));
   changed.static_state.state.blend_enable = 0;
   changed.static_state.state.depth_test = 1;
   assert(commandbuffer_pipeline_recipe_hash(&changed, false) == original);
   changed.potential_static_state.spec_constants[0]++;
   assert(commandbuffer_pipeline_recipe_hash(&changed, false) != original);
   puts("PASS canonical identity: active state matters, inactive state does not");
}

static void test_capture_and_normalization(void)
{
   Renderer renderer;
   CommandBuffer cmd;
   unsigned count, variants;
   int gamma;
   Hash floor;
   setup_renderer(&renderer);
   setup_command(&cmd, &renderer);
   recorded_count = recorded_calls = 0;
   variants = renderer_precompile_primitive_pipelines(&renderer, &cmd, false);
   variants += renderer_precompile_primitive_pipelines(&renderer, &cmd, true);
   assert(variants == 107);
   assert(recorded_calls == 107);
   count = recorded_count;
   renderer_precompile_primitive_pipelines(&renderer, &cmd, false);
   renderer_precompile_primitive_pipelines(&renderer, &cmd, true);
   assert(recorded_count == count);
   renderer.primitive_filter_mode = FilterMode_Bilinear;
   variants = renderer_precompile_primitive_pipelines(&renderer, &cmd, false);
   variants += renderer_precompile_primitive_pipelines(&renderer, &cmd, true);
   assert(variants == 179);
   assert(recorded_count > count);

   setup_command(&cmd, &renderer);
   renderer.scaled_fb_format = VK_FORMAT_R16G16B16A16_SFLOAT;
   renderer.pipelines.flat_floor->layout.resources.combined_spec_constant_mask =
      (1u << SpecConstIndex_OffsetUV) | (1u << SpecConstIndex_Scaling);
   renderer_set_semi_transparent_pipeline_state(&cmd);
   cmd.potential_static_state.spec_constants[SpecConstIndex_OffsetUV] = 1;
   renderer_set_sub_floor_pipeline_state(&renderer, &cmd);
   assert(cmd.potential_static_state.spec_constants[SpecConstIndex_OffsetUV] == 0);
   floor = commandbuffer_pipeline_recipe_hash(&cmd, true);
   cmd.potential_static_state.spec_constants[SpecConstIndex_OffsetUV] = 9;
   renderer_set_sub_floor_pipeline_state(&renderer, &cmd);
   assert(commandbuffer_pipeline_recipe_hash(&cmd, true) == floor);
   for (gamma = 0; gamma < 3; gamma++)
   {
      psx_hdr_sdr_eotf = gamma;
      cmd.potential_static_state.spec_constants[SpecConstIndex_MaskTest] = 1;
      renderer_set_resolve_pipeline_state(&renderer, &cmd, 0);
      assert(cmd.potential_static_state.spec_constants[SpecConstIndex_ResolveEotf] == 0);
      renderer_set_resolve_pipeline_state(&renderer, &cmd, 1);
      assert(cmd.potential_static_state.spec_constants[SpecConstIndex_ResolveEotf] == (unsigned)gamma);
   }
   psx_hdr_sdr_eotf = 0;
   puts("PASS CPU capture: complete tuples, repeat dedup, floor UV and resolve EOTF normalization");
}

static void test_scanout_selectors(void)
{
   Renderer renderer;
   unsigned hdr, cable, ssaa, adaptive, dither, yuv;
   setup_renderer(&renderer);
   for (hdr = 0; hdr < 2; hdr++)
   for (cable = RHI_CABLE_NONE; cable <= RHI_CABLE_RGB; cable++)
   for (ssaa = 0; ssaa < 2; ssaa++)
   for (adaptive = 0; adaptive < 2; adaptive++)
   for (dither = 0; dither < 2; dither++)
   {
      Program *expected;
      bool hdr_quad = hdr && cable == RHI_CABLE_NONE;
      psx_hdr_active = hdr != 0;
      psx_video_cable = (int)cable;
      expected = hdr ? renderer.pipelines.hdr_scaled_quad_blitter : renderer.pipelines.scaled_quad_blitter;
      /* VRAM bypasses ordinary scanout's SSAA/adaptive/analog branches. */
      assert(renderer_vram_scanout_program(&renderer, true) == expected);
      expected = hdr ? renderer.pipelines.hdr_unscaled_quad_blitter : renderer.pipelines.unscaled_quad_blitter;
      assert(renderer_vram_scanout_program(&renderer, false) == expected);
      if (ssaa)
         expected = hdr_quad ? renderer.pipelines.hdr_unscaled_quad_blitter
            : dither && !hdr ? renderer.pipelines.unscaled_dither_quad_blitter : renderer.pipelines.unscaled_quad_blitter;
      else if (adaptive)
         expected = hdr_quad ? renderer.pipelines.hdr_mipmap_resolve
            : dither && !hdr ? renderer.pipelines.mipmap_dither_resolve : renderer.pipelines.mipmap_resolve;
      else
         expected = hdr_quad ? renderer.pipelines.hdr_scaled_quad_blitter
            : dither && !hdr ? renderer.pipelines.scaled_dither_quad_blitter : renderer.pipelines.scaled_quad_blitter;
      assert(renderer_scanout_program(&renderer, false, ssaa, adaptive, dither, false) == expected);
      for (yuv = 0; yuv < 2; yuv++)
      {
         expected = hdr_quad
            ? (yuv ? renderer.pipelines.hdr_bpp24_yuv_quad_blitter : renderer.pipelines.hdr_bpp24_quad_blitter)
            : (yuv ? renderer.pipelines.bpp24_yuv_quad_blitter : renderer.pipelines.bpp24_quad_blitter);
         assert(renderer_scanout_program(&renderer, true, ssaa, adaptive, dither, yuv) == expected);
      }
   }
   renderer.scaling = 1;
   psx_hdr_active = false;
   psx_video_cable = RHI_CABLE_NONE;
   assert(renderer_scanout_program(&renderer, false, false, true, false, false) == renderer.pipelines.scaled_quad_blitter);
   puts("PASS scanout selectors: HDR/VRAM, cable, SSAA, adaptive, dither and 24-bit/YUV matrix");
}

static bool captured_program(Program *program)
{
   unsigned i;
   for (i = 0; i < recorded_count; i++)
      if (recorded[i].program == program) return true;
   return false;
}
static bool captured_program_format(Program *program, VkFormat format)
{
   unsigned i;
   for (i = 0; i < recorded_count; i++)
      if (recorded[i].program == program &&
            recorded[i].format == format) return true;
   return false;
}

static void test_render_pass_metadata(void)
{
   const VkFormat formats[] = { VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32 };
   Renderer renderer;
   unsigned f, samples, primitives, feedback;
   setup_renderer(&renderer);
   for (f = 0; f < sizeof(formats) / sizeof(formats[0]); f++)
   for (samples = 1; samples <= 8; samples *= 2)
   for (primitives = 0; primitives < 2; primitives++)
   for (feedback = 0; feedback <= primitives; feedback++)
   {
      Image color, depth;
      ImageView color_view, depth_view;
      RenderPassInfo pass;
      RenderPassInfo_Subpass subpass;
      const RenderPass *metadata, *runtime;
      Hash original;
      /* Runtime-sized, resource-backed attachment descriptions use different
       * extent/load/store/layout details, but the same compatibility class.
       * Only the allocation/Vulkan handle boundary is mocked. Both paths use
       * the actual production attachment factories and cache-key function. */
      memset(&color, 0, sizeof(color));
      memset(&depth, 0, sizeof(depth));
      memset(&color_view, 0, sizeof(color_view));
      memset(&depth_view, 0, sizeof(depth_view));
      color.create_info = image_create_info_render_target(4096, 2048, formats[f]);
      color.create_info.samples = (VkSampleCountFlagBits)(primitives ? samples : 1);
      color.layout_type = Layout_General;
      color_view.info.image = &color;
      color_view.info.format = formats[f];
      render_pass_info_defaults(&pass);
      pass.num_color_attachments = 1;
      pass.color_attachments[0] = &color_view;
      pass.load_attachments = pass.store_attachments = 1;
      if (primitives)
      {
         depth.create_info = image_create_info_render_target(4096, 2048, VK_FORMAT_D32_SFLOAT);
         depth.create_info.samples = (VkSampleCountFlagBits)samples;
         depth.create_info.domain = ImageDomain_Transient;
         depth.layout_type = Layout_Optimal;
         depth_view.info.image = &depth;
         depth_view.info.format = depth.create_info.format;
         pass.depth_stencil = &depth_view;
         pass.op_flags = RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT;
         render_pass_info_subpass_defaults(&subpass);
         subpass.num_color_attachments = 1;
         subpass.color_attachments[0] = 0;
         if (feedback)
         {
            subpass.num_input_attachments = 1;
            subpass.input_attachments[0] = 0;
         }
         pass.num_subpasses = 1;
         pass.subpasses = &subpass;
      }
      /* A renderer recreation normally starts fresh; reset the test map so
       * every pair must produce the same key on separate insertion paths. */
      test_device.render_passes.count = 0;
      renderer.msaa = samples;
      metadata = renderer_precompile_render_pass(&renderer, formats[f], primitives, feedback);
      runtime = device_request_render_pass(renderer.device, &pass, true);
      original = metadata->intrusive_node.key;
      assert(runtime == metadata);
      assert(runtime->intrusive_node.key == original);
      assert(device_request_render_pass(renderer.device, &pass, false)->intrusive_node.key != original);
      color.create_info.samples = color.create_info.samples == VK_SAMPLE_COUNT_1_BIT ?
         VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
      assert(device_request_render_pass(renderer.device, &pass, true)->intrusive_node.key != original);
      color.create_info.samples = (VkSampleCountFlagBits)(primitives ? samples : 1);
      if (primitives)
      {
         /* Each attachment contributes its own samples, not only color[0]. */
         depth.create_info.samples = samples == 1 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
         assert(device_request_render_pass(renderer.device, &pass, true)->intrusive_node.key != original);
         depth.create_info.samples = (VkSampleCountFlagBits)samples;
         subpass.num_input_attachments = feedback ? 0 : 1;
         subpass.input_attachments[0] = 0;
         assert(device_request_render_pass(renderer.device, &pass, true)->intrusive_node.key != original);
      }
   }
   test_device.render_passes.count = 0;
   puts("PASS render-pass metadata: runtime/capture keys match; color/depth samples and feedback remain distinct");
}

static void test_active_manifest_matrix(void)
{
   Renderer renderer;
   CommandBuffer cmd;
   unsigned scale, hdr, cable, bits, multi, cases = 0;
   setup_renderer(&renderer);
   for (scale = 1; scale <= 16; scale *= 2)
   for (multi = 1; multi <= 4; multi *= 4)
   for (hdr = 0; hdr < 2; hdr++)
   for (cable = RHI_CABLE_NONE; cable <= RHI_CABLE_RGB; cable++)
   for (bits = 0; bits < 32; bits++)
   {
      bool adaptive;
      renderer.scaling = scale;
      renderer.msaa = multi;
      renderer.scaled_fb_format = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
      psx_hdr_active = hdr != 0;
      psx_video_cable = (int)cable;
      super_sampling = (bits & 1) != 0;
      adaptive_smoothing = (bits & 2) != 0;
      dither_mode = (bits & 4) != 0;
      mdec_yuv = (bits & 8) != 0;
      show_vram = (bits & 16) != 0;
      adaptive = scale != 1 && adaptive_smoothing && !super_sampling;
      setup_command(&cmd, &renderer);
      recorded_count = recorded_calls = 0;
      renderer_precompile_manifest_nonprimitive_pipelines(&renderer, &cmd);
#define TEST_ROLE_Compute 0
#define TEST_ROLE_Quad 0
#define TEST_ROLE_Primitive 1
#define TEST_ACTIVE_CAPTURE(id, field, role) \
      if (!TEST_ROLE_##role && renderer_precompile_manifest_program_is_active(&renderer, RendererPipelineManifestId_##id)) \
         assert(captured_program(renderer.pipelines.field));
      RENDERER_PIPELINE_MANIFEST(TEST_ACTIVE_CAPTURE)
#undef TEST_ACTIVE_CAPTURE
#undef TEST_ROLE_Compute
#undef TEST_ROLE_Quad
#undef TEST_ROLE_Primitive
      assert(renderer_precompile_manifest_program_is_active(&renderer,
            RendererPipelineManifestId_MipmapEnergyFirst) == (adaptive && scale >= 2));
      assert(renderer_precompile_manifest_program_is_active(&renderer,
            RendererPipelineManifestId_MipmapEnergy) == (adaptive && scale >= 4));
      assert(renderer_precompile_manifest_program_is_active(&renderer,
            RendererPipelineManifestId_MipmapEnergyBlur) == adaptive);
      if (adaptive)
      {
         assert(captured_program_format(renderer.pipelines.mipmap_energy_first, renderer.scaled_fb_format));
         if (scale >= 4)
            assert(captured_program_format(renderer.pipelines.mipmap_energy, renderer.scaled_fb_format));
         assert(captured_program_format(renderer.pipelines.mipmap_energy_blur, VK_FORMAT_R8_UNORM));
      }
      if (show_vram)
         assert(captured_program_format(renderer_vram_scanout_program(&renderer, true),
               hdr ? renderer_hdr_scanout_format(&renderer) : VK_FORMAT_A1R5G5B5_UNORM_PACK16));
      cases++;
   }
   printf("PASS active manifest: %u configuration combinations, mipmap boundaries and exact VRAM target format\n", cases);
}

static void test_configuration_gate(void)
{
   Renderer renderer;
   unsigned calls;
   setup_renderer(&renderer);
   super_sampling = adaptive_smoothing = mdec_yuv = show_vram = psx_hdr_active = false;
   psx_video_cable = dither_mode = psx_hdr_sdr_eotf = 0;
   psx_hdr_overbright_hot = psx_pgxp_color = psx_pgxp_fog = psx_hdr_multipass = 0;
   vulkan_shader_precompilation = true;
   precompile_attempts = 0;
   precompile_failures_remaining = 0;
   renderer_precompile_if_configuration_changed(&renderer);
   assert(precompile_attempts == 1);
   renderer_precompile_if_configuration_changed(&renderer);
   assert(precompile_attempts == 1);
#define EXPECT_CONFIG_DELTA(statement) \
   calls = precompile_attempts; statement; \
   renderer_precompile_if_configuration_changed(&renderer); \
   assert(precompile_attempts == calls + 1); \
   renderer_precompile_if_configuration_changed(&renderer); \
   assert(precompile_attempts == calls + 1)
   EXPECT_CONFIG_DELTA(renderer.primitive_filter_mode = FilterMode_Bilinear);
   EXPECT_CONFIG_DELTA(renderer.scaled_uv_offset = false);
   EXPECT_CONFIG_DELTA(adaptive_smoothing = true);
   EXPECT_CONFIG_DELTA(mdec_yuv = true);
   EXPECT_CONFIG_DELTA(dither_mode = 1);
   EXPECT_CONFIG_DELTA(psx_video_cable = RHI_CABLE_RGB);
   EXPECT_CONFIG_DELTA(psx_hdr_active = true);
   EXPECT_CONFIG_DELTA(show_vram = true);
   EXPECT_CONFIG_DELTA(super_sampling = true);
   EXPECT_CONFIG_DELTA(psx_hdr_sdr_eotf = 2);
   EXPECT_CONFIG_DELTA(psx_hdr_overbright_hot = 1);
   EXPECT_CONFIG_DELTA(psx_pgxp_color = 1);
   EXPECT_CONFIG_DELTA(psx_pgxp_fog = 1);
   EXPECT_CONFIG_DELTA(psx_hdr_multipass = 1);
   EXPECT_CONFIG_DELTA(renderer.msaa = 4);
   EXPECT_CONFIG_DELTA(renderer.scaling = 2);
   EXPECT_CONFIG_DELTA(renderer.scaled_fb_format = VK_FORMAT_R16G16B16A16_SFLOAT);
#undef EXPECT_CONFIG_DELTA
   calls = precompile_attempts;
   vulkan_shader_precompilation = false;
   renderer_precompile_if_configuration_changed(&renderer);
   assert(precompile_attempts == calls && !renderer.precompile_configuration_valid);
   vulkan_shader_precompilation = true;
   renderer_precompile_if_configuration_changed(&renderer);
   assert(precompile_attempts == calls + 1);

   setup_renderer(&renderer);
   precompile_attempts = 0;
   precompile_failures_remaining = 1;
   renderer_precompile_if_configuration_changed(&renderer);
   assert(precompile_attempts == 2);
   assert(renderer.precompile_configuration_valid);
   assert(renderer.device->precompile_plan_attempted);
   assert(renderer.device->precompile_plan_complete);

   setup_renderer(&renderer);
   precompile_attempts = 0;
   precompile_failures_remaining = 2;
   renderer_precompile_if_configuration_changed(&renderer);
   assert(precompile_attempts == 2);
   assert(renderer.precompile_configuration_valid);
   assert(renderer.device->precompile_plan_attempted);
   assert(!renderer.device->precompile_plan_complete);
   renderer_precompile_if_configuration_changed(&renderer);
   assert(precompile_attempts == 2);
   precompile_failures_remaining = 0;
   puts("PASS configuration gate: live deltas, disable/re-enable and one bounded retry");
}

static void test_runtime_escape_accounting(void)
{
   Program program;
   memset(&test_device, 0, sizeof(test_device));
   memset(&program, 0, sizeof(program));
   program.intrusive_node.key = 0x1234;
   device_precompile_note_runtime_escape(&test_device, &program, 0x5678, true);
   assert(!test_device.precompile_plan_attempted);
   assert(test_device.precompile_runtime_escapes == 0);
   test_device.precompile_plan_attempted = true;
   test_device.precompile_plan_complete = true;
   device_precompile_note_runtime_escape(&test_device, &program, 0x5678, true);
   assert(!test_device.precompile_plan_complete);
   assert(test_device.precompile_runtime_escapes == 1);
   puts("PASS runtime escape accounting: observed misses revoke plan completeness");
}

static void test_exact_recipe_validation(void)
{
   Renderer renderer;
   Program *graphics, *compute;
   setup_renderer(&renderer);
   graphics = renderer.pipelines.flat;
   compute = renderer.pipelines.resolve_to_unscaled;
   vulkan_precompile_expect_recipe(graphics, 11, true);
   vulkan_precompile_expect_recipe(graphics, 11, true);
   vulkan_precompile_expect_recipe(compute, 22, false);
   assert(vulkan_precompile_recipes.count == 2);
   assert(!vulkan_precompile_validate_recipes(false));
   graphics_jobs[0].program = graphics;
   graphics_jobs[0].hash = 11;
   compute_jobs[0].program = compute;
   compute_jobs[0].hash = 22;
   vulkan_graphics_precompile_jobs.count = vulkan_compute_precompile_jobs.count = 1;
   assert(vulkan_precompile_validate_recipes(false));
   assert(!vulkan_precompile_validate_recipes(true));
   graphics_jobs[0].hash = 12;
   assert(!vulkan_precompile_validate_recipes(false));
   graphics_jobs[0].hash = 11;
   graphics_jobs[0].program = compute;
   assert(!vulkan_precompile_validate_recipes(false));
   graphics_jobs[0].program = graphics;
   published_count = 2;
   published_jobs[0].program = graphics;
   published_jobs[0].hash = 11;
   published_jobs[0].entry.value = (VkPipeline)(uintptr_t)1;
   published_jobs[1].program = compute;
   published_jobs[1].hash = 22;
   published_jobs[1].entry.value = VK_NULL_HANDLE;
   assert(!vulkan_precompile_validate_recipes(true));
   published_jobs[1].entry.value = (VkPipeline)(uintptr_t)2;
   assert(vulkan_precompile_validate_recipes(true));
   vulkan_precompile_recipes.failed = true;
   assert(!vulkan_precompile_validate_recipes(true));
   free(vulkan_precompile_recipes.items);
   memset(&vulkan_precompile_recipes, 0, sizeof(vulkan_precompile_recipes));
   puts("PASS exact recipe validation: dedup, missing/wrong identity, queue versus publication and failure");
}

int main(void)
{
   test_render_pass_metadata();
   test_canonical_identity();
   test_capture_and_normalization();
   test_scanout_selectors();
   test_active_manifest_matrix();
   test_configuration_gate();
   test_runtime_escape_accounting();
   test_exact_recipe_validation();
   puts("All Vulkan recipe regressions passed (no GPU or game required).");
   return 0;
}

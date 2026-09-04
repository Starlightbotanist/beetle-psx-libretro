/* Fake-driver regression for the production precompile batch worker.
 * The worker implementation and job types are extracted by
 * test_precompile_jobs.py; no GPU, frontend, core or game is used. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#ifndef VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT
#define VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT \
   ((VkPipelineCreateFlagBits)0x00000100)
#define VK_PIPELINE_COMPILE_REQUIRED_EXT ((VkResult)1000297000)
#endif

#define VULKAN_NUM_ATTACHMENTS 8
#define VULKAN_NUM_SPEC_CONSTANTS 10
#define VULKAN_NUM_VERTEX_ATTRIBS 16
#define VULKAN_NUM_VERTEX_BUFFERS 16
#define min_(a, b) ((a) < (b) ? (a) : (b))

typedef uint64_t Hash;
typedef struct Program { unsigned unused; } Program;
typedef struct Device { unsigned unused; } Device;
typedef struct slock { unsigned unused; } slock_t;

static void slock_lock(slock_t *lock) { (void)lock; }
static void slock_unlock(slock_t *lock) { (void)lock; }
static VkDevice device_get_device(Device *device)
{
   return (VkDevice)(uintptr_t)device;
}

#include "precompile_types_under_test.h"

static unsigned graphics_calls;
static unsigned compute_calls;
static unsigned largest_batch;
static bool probe_mode;

static VkResult fake_create_pipelines(uint32_t count,
      const VkPipelineCreateFlags *flags, const int32_t *ids,
      VkPipeline *pipelines)
{
   VkResult result = VK_SUCCESS;
   uint32_t i;
   assert(count > 0 && count <= VulkanPrecompileBatchSize);
   if (count > largest_batch)
      largest_batch = count;
   for (i = 0; i < count; i++)
   {
      bool probe = (flags[i] &
            VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT) != 0;
      assert(probe == probe_mode);
      if (probe && (ids[i] & 1))
      {
         pipelines[i] = VK_NULL_HANDLE;
         result = VK_PIPELINE_COMPILE_REQUIRED_EXT;
      }
      else if (!probe && ids[i] == 99)
      {
         pipelines[i] = VK_NULL_HANDLE;
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      else
         pipelines[i] = (VkPipeline)(uintptr_t)(1000 + ids[i]);
   }
   return result;
}

static VkResult vkCreateGraphicsPipelines(VkDevice device,
      VkPipelineCache cache, uint32_t count,
      const VkGraphicsPipelineCreateInfo *infos,
      const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
   VkPipelineCreateFlags flags[VulkanPrecompileBatchSize];
   int32_t ids[VulkanPrecompileBatchSize];
   uint32_t i;
   (void)device;
   (void)cache;
   assert(allocator == NULL);
   graphics_calls++;
   for (i = 0; i < count; i++)
   {
      flags[i] = infos[i].flags;
      ids[i] = infos[i].basePipelineIndex;
   }
   return fake_create_pipelines(count, flags, ids, pipelines);
}

static VkResult vkCreateComputePipelines(VkDevice device,
      VkPipelineCache cache, uint32_t count,
      const VkComputePipelineCreateInfo *infos,
      const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
   VkPipelineCreateFlags flags[VulkanPrecompileBatchSize];
   int32_t ids[VulkanPrecompileBatchSize];
   uint32_t i;
   (void)device;
   (void)cache;
   assert(allocator == NULL);
   compute_calls++;
   for (i = 0; i < count; i++)
   {
      flags[i] = infos[i].flags;
      ids[i] = infos[i].basePipelineIndex;
   }
   return fake_create_pipelines(count, flags, ids, pipelines);
}

#include "precompile_worker_under_test.h"

static void reset_driver(bool probe)
{
   graphics_calls = 0;
   compute_calls = 0;
   largest_batch = 0;
   probe_mode = probe;
}

static void initialize_jobs(struct VulkanPrecompileContext *context,
      struct VulkanGraphicsPrecompileJob *graphics, unsigned graphics_count,
      struct VulkanComputePrecompileJob *compute, unsigned compute_count)
{
   unsigned i;
   memset(context, 0, sizeof(*context));
   memset(graphics, 0, graphics_count * sizeof(*graphics));
   memset(compute, 0, compute_count * sizeof(*compute));
   context->graphics_jobs.items = graphics;
   context->graphics_jobs.count = graphics_count;
   context->compute_jobs.items = compute;
   context->compute_jobs.count = compute_count;
   for (i = 0; i < graphics_count; i++)
   {
      graphics[i].pipe.basePipelineIndex = (int32_t)i;
      graphics[i].result = VK_NOT_READY;
   }
   for (i = 0; i < compute_count; i++)
   {
      compute[i].pipe.basePipelineIndex = (int32_t)(graphics_count + i);
      compute[i].result = VK_NOT_READY;
   }
}

static void run_worker(struct VulkanPrecompileContext *context,
      bool probe)
{
   Device device;
   struct VulkanPrecompileQueue queue;
   struct VulkanPrecompileWorker worker;
   memset(&device, 0, sizeof(device));
   memset(&queue, 0, sizeof(queue));
   memset(&worker, 0, sizeof(worker));
   queue.context = context;
   queue.graphics_jobs = context->graphics_jobs.count;
   queue.total_jobs = queue.graphics_jobs + context->compute_jobs.count;
   queue.cache_probe = probe;
   worker.device = &device;
   worker.pipeline_cache = (VkPipelineCache)(uintptr_t)1;
   worker.queue = &queue;
   vulkan_precompile_worker(&worker);
}

static void test_probe_batches_and_maps_results(void)
{
   struct VulkanPrecompileContext context;
   struct VulkanGraphicsPrecompileJob graphics[6];
   struct VulkanComputePrecompileJob compute[3];
   unsigned i;
   initialize_jobs(&context, graphics, 6, compute, 3);
   reset_driver(true);
   run_worker(&context, true);

   assert(graphics_calls == 2);
   assert(compute_calls == 1);
   assert(largest_batch == VulkanPrecompileBatchSize);
   for (i = 0; i < 6; i++)
   {
      assert(graphics[i].pipe.flags == 0);
      if (i & 1)
      {
         assert(graphics[i].pipeline == VK_NULL_HANDLE);
         assert(graphics[i].result == VK_PIPELINE_COMPILE_REQUIRED_EXT);
      }
      else
      {
         assert(graphics[i].pipeline != VK_NULL_HANDLE);
         assert(graphics[i].result == VK_SUCCESS);
      }
   }
   for (i = 0; i < 3; i++)
   {
      unsigned id = 6 + i;
      if (id & 1)
      {
         assert(compute[i].pipeline == VK_NULL_HANDLE);
         assert(compute[i].result == VK_PIPELINE_COMPILE_REQUIRED_EXT);
      }
      else
      {
         assert(compute[i].pipeline != VK_NULL_HANDLE);
         assert(compute[i].result == VK_SUCCESS);
      }
   }
}

static void test_compile_skips_hits_and_preserves_partial_success(void)
{
   struct VulkanPrecompileContext context;
   struct VulkanGraphicsPrecompileJob graphics[5];
   struct VulkanComputePrecompileJob compute[2];
   initialize_jobs(&context, graphics, 5, compute, 2);

   graphics[0].result = VK_SUCCESS;
   graphics[0].pipeline = (VkPipeline)(uintptr_t)55;
   graphics[2].pipe.basePipelineIndex = 99;
   reset_driver(false);
   run_worker(&context, false);

   assert(graphics_calls == 2);
   assert(compute_calls == 1);
   assert(graphics[0].pipeline == (VkPipeline)(uintptr_t)55);
   assert(graphics[0].result == VK_SUCCESS);
   assert(graphics[1].pipeline != VK_NULL_HANDLE);
   assert(graphics[1].result == VK_SUCCESS);
   assert(graphics[2].pipeline == VK_NULL_HANDLE);
   assert(graphics[2].result == VK_ERROR_OUT_OF_HOST_MEMORY);
   assert(graphics[3].pipeline != VK_NULL_HANDLE);
   assert(graphics[3].result == VK_SUCCESS);
   assert(graphics[4].pipeline != VK_NULL_HANDLE);
   assert(graphics[4].result == VK_SUCCESS);
   assert(vulkan_precompile_pending_jobs(&context) == 0);
}

int main(void)
{
   test_probe_batches_and_maps_results();
   test_compile_skips_hits_and_preserves_partial_success();
   puts("Vulkan precompile batching and cache-probe tests passed");
   return 0;
}

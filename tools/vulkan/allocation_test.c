/* Fault-injection test doubles and assertions for extracted production code.
 * The container and publication implementations are generated from
 * rhi_lib_vulkan.c by test_allocation.py, not copied into this fixture. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

typedef uint64_t Hash;
typedef uintptr_t VkPipeline;
typedef struct IntrusivePODWrapperPipeline IntrusivePODWrapperPipeline;
typedef struct IntrusivePODWrapperPtr IntrusivePODWrapperPtr;

#define VK_NULL_HANDLE 0
#define RHI_STATIC_ASSERT(condition, message)
#define LOGE(...) ((void)0)

static unsigned allocation_calls, fail_at, live_allocations;

static void fail_after(unsigned call)
{
   allocation_calls = 0;
   fail_at = call;
}

static bool should_fail(void)
{
   return ++allocation_calls == fail_at;
}

static void *test_malloc(size_t size)
{
   void *p;
   if (should_fail())
      return NULL;
   p = malloc(size);
   if (p)
      live_allocations++;
   return p;
}

static void *test_calloc(size_t count, size_t size)
{
   void *p;
   if (should_fail())
      return NULL;
   p = calloc(count, size);
   if (p)
      live_allocations++;
   return p;
}

static void *test_realloc(void *old, size_t size)
{
   void *p;
   bool was_null = old == NULL;
   if (should_fail())
      return NULL;
   p = realloc(old, size);
   if (p && was_null)
      live_allocations++;
   return p;
}

static void test_free(void *p)
{
   if (p)
   {
      assert(live_allocations);
      live_allocations--;
   }
   free(p);
}

#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#include "allocation_under_test.h"

typedef struct Device
{
   unsigned dirty;
   unsigned destroyed;
} Device;

struct Program
{
   Device *device;
   struct vk_pipeline_map pipelines;
};

static Device *device_get_device(Device *device)
{
   return device;
}

static void device_pipeline_cache_mark_dirty(Device *device)
{
   device->dirty++;
}

static void vkDestroyPipeline(Device *device, VkPipeline pipeline,
      const void *allocator)
{
   assert(pipeline != VK_NULL_HANDLE);
   assert(allocator == NULL);
   device->destroyed++;
}

static IntrusivePODWrapperPipeline *program_find_pipeline(
      struct Program *program, Hash hash)
{
   return vk_pipeline_map_find(&program->pipelines, hash);
}

#include "publication_under_test.h"

static void test_pool(void)
{
   unsigned failure, i;

   /* Initial vacancy metadata, slab metadata, and slab allocation failures. */
   for (failure = 1; failure <= 3; failure++)
   {
      struct ObjectPoolRaw pool;
      void *slot;
      object_pool_raw_init(&pool, 32);
      fail_after(failure);
      assert(object_pool_raw_allocate(&pool) == NULL);
      assert(pool.mem_count == 0 && pool.vac_count == 0);
      fail_after(0);
      slot = object_pool_raw_allocate(&pool);
      assert(slot != NULL);
      object_pool_raw_free(&pool, slot);
      object_pool_raw_deinit(&pool);
      assert(live_allocations == 0);
   }

   /* Growth must preserve the first slab, and returning all slots is infallible. */
   for (failure = 1; failure <= 2; failure++)
   {
      struct ObjectPoolRaw pool;
      void *slots[192];
      object_pool_raw_init(&pool, 32);
      fail_after(0);
      for (i = 0; i < 64; i++)
         assert((slots[i] = object_pool_raw_allocate(&pool)) != NULL);
      fail_after(failure);
      assert(object_pool_raw_allocate(&pool) == NULL);
      assert(pool.mem_count == 1 && pool.vac_count == 0);
      fail_after(0);
      for (i = 64; i < 192; i++)
         assert((slots[i] = object_pool_raw_allocate(&pool)) != NULL);
      fail_after(1);
      for (i = 0; i < 192; i++)
         object_pool_raw_free(&pool, slots[i]);
      assert(allocation_calls == 0 && pool.vac_count == 192);
      object_pool_raw_deinit(&pool);
      assert(live_allocations == 0);
   }
}

static void test_publication(void)
{
   unsigned failure, i;

   /* Publication adds a fourth allocation point: initial hash buckets. */
   for (failure = 1; failure <= 4; failure++)
   {
      Device device = { 0, 0 };
      struct Program program;
      program.device = &device;
      vk_pipeline_map_init(&program.pipelines);
      fail_after(failure);
      assert(program_add_pipeline(&program, 1, 101) == VK_NULL_HANDLE);
      assert(device.destroyed == 1 && device.dirty == 0);
      assert(program_find_pipeline(&program, 1) == NULL);
      fail_after(0);
      assert(program_add_pipeline(&program, 1, 102) == 102);
      assert(device.dirty == 1);
      fail_after(1);
      assert(program_add_pipeline(&program, 1, 103) == 102);
      assert(allocation_calls == 0 && device.destroyed == 2 && device.dirty == 1);
      vk_pipeline_map_deinit(&program.pipelines);
      assert(live_allocations == 0);
   }

   /* A failed populated-table grow must keep all existing pipelines reachable. */
   {
      Device device = { 0, 0 };
      struct Program program;
      program.device = &device;
      vk_pipeline_map_init(&program.pipelines);
      fail_after(0);
      for (i = 0; i < 3; i++)
         assert(program_add_pipeline(&program, i * 16, i + 1) == i + 1);
      assert(program.pipelines.hashmap.count == 16);
      fail_after(1);
      assert(program_add_pipeline(&program, 48, 4) == VK_NULL_HANDLE);
      assert(program.pipelines.hashmap.count == 16);
      assert(device.dirty == 3 && device.destroyed == 1);
      for (i = 0; i < 3; i++)
         assert(program_find_pipeline(&program, i * 16)->value == i + 1);
      assert(program_find_pipeline(&program, 48) == NULL);
      fail_after(0);
      assert(program_add_pipeline(&program, 48, 5) == 5);
      assert(device.dirty == 4);
      assert(vk_pipeline_map_emplace_yield(&program.pipelines, 48, 99)->value == 5);
      vk_pipeline_map_deinit(&program.pipelines);
      assert(live_allocations == 0);
   }
}

static void test_replace(void)
{
   struct vk_ptr_map map;
   unsigned i;
   vk_ptr_map_init(&map);
   fail_after(0);
   for (i = 0; i < 3; i++)
      assert(vk_ptr_map_emplace_replace(&map, i * 16,
               (void *)(uintptr_t)(i + 1)) != NULL);
   fail_after(1);
   assert(vk_ptr_map_emplace_replace(&map, 48, (void *)(uintptr_t)4) == NULL);
   for (i = 0; i < 3; i++)
      assert(vk_ptr_map_find(&map, i * 16)->value == (void *)(uintptr_t)(i + 1));
   fail_after(0);
   assert(vk_ptr_map_emplace_replace(&map, 16,
            (void *)(uintptr_t)55)->value == (void *)(uintptr_t)55);
   assert(vk_ptr_map_find(&map, 16)->value == (void *)(uintptr_t)55);
   assert(vk_ptr_map_emplace_replace(&map, 48, (void *)(uintptr_t)4) != NULL);
   vk_ptr_map_deinit(&map);
   assert(live_allocations == 0);
}

int main(void)
{
   test_pool();
   test_publication();
   test_replace();
   puts("Vulkan allocator/publication fault-injection tests passed");
   return 0;
}

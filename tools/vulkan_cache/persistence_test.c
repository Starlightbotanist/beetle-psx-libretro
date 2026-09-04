/* Test doubles for the persistence block extracted from rhi_lib_vulkan.c.
 * The driver represents pipeline knowledge as a bitset, making lost updates
 * observable without relying on an opaque real GPU driver's cache format. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/wait.h>
#endif
#include <unistd.h>

#define PATH_MAX_LENGTH 2048
#define VK_UUID_SIZE 16
#define VK_NULL_HANDLE NULL
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
#define VK_ERROR_OUT_OF_DEVICE_MEMORY -2
#define VK_ERROR_DEVICE_LOST -4
#define VK_PIPELINE_CACHE_HEADER_VERSION_ONE 1
#define VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO 17
#define RETRO_VFS_FILE_ACCESS_READ 1
#define RETRO_VFS_FILE_ACCESS_WRITE 2
#define RETRO_VFS_FILE_ACCESS_HINT_NONE 0
#define RETRO_VFS_FILE_ACCESS_HINT_SEQUENTIAL_BULK 0
#define RHI_STATIC_ASSERT(value, name) typedef char name[(value) ? 1 : -1]
static char save_log[512], save_attempt_log[512];
static void test_log(const char *format, ...)
{
   char *target = NULL;
   va_list args;
   if (strstr(format, "pipeline cache] save dirty="))
      target = save_log;
   else if (strstr(format, "save-attempt"))
      target = save_attempt_log;
   if (!target)
      return;
   va_start(args, format);
   vsnprintf(target, sizeof(save_log), format, args);
   va_end(args);
}
#define LOGI(...) test_log(__VA_ARGS__)
#define LOGE(...) test_log(__VA_ARGS__)

typedef int VkResult;
typedef int64_t retro_time_t;
typedef struct FakeCache
{
   uint32_t header_size, header_version, vendor_id, device_id;
   uint8_t uuid[VK_UUID_SIZE];
   uint64_t entries;
} *VkPipelineCache;

typedef struct VkPipelineCacheCreateInfo
{
   int sType;
   void *pNext;
   unsigned flags;
   size_t initialDataSize;
   const void *pInitialData;
} VkPipelineCacheCreateInfo;

typedef struct Device
{
   void *device;
   VkPipelineCache pipeline_cache;
   char pipeline_cache_file_name[96];
   unsigned pipeline_cache_dirty_count;
   bool pipeline_cache_has_data;
   bool pipeline_cache_storage_warned;
   struct
   {
      uint32_t vendorID, deviceID;
      uint8_t pipelineCacheUUID[VK_UUID_SIZE];
   } gpu_props;
} Device;

typedef struct RFILE { FILE *file; bool writing; } RFILE;
static char retro_base_directory[4096];
static char retro_save_directory[4096];
static retro_time_t mock_time = 1000000;
static retro_time_t snapshot_delay;
static unsigned snapshot_queries, write_opens;
static unsigned vfs_replace_attempts;
static bool fail_write, fail_close, fail_replace, fail_read;
static VkResult import_error, merge_error;

static retro_time_t cpu_features_get_time_usec(void) { return mock_time; }

#ifdef _WIN32
static wchar_t *utf8_to_utf16_string_alloc(const char *text);
#endif

static uint32_t encoding_crc32(uint32_t crc, const void *data, size_t size)
{
   const uint8_t *bytes = (const uint8_t *)data;
   size_t i;
   unsigned bit;
   crc = ~crc;
   for (i = 0; i < size; i++)
   {
      crc ^= bytes[i];
      for (bit = 0; bit < 8; bit++)
         crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
   }
   return ~crc;
}

static void fill_pathname_join_special(char *out, const char *root,
      const char *name, size_t size)
{
   size_t root_size = strlen(root);
   size_t name_size = strlen(name);
   assert(root_size + name_size + 2 <= size);
   memcpy(out, root, root_size);
   out[root_size] = '/';
   memcpy(out + root_size + 1, name, name_size + 1);
}

static bool path_mkdir(const char *path)
{
   struct stat st;
#ifdef _WIN32
   int result = _mkdir(path);
#else
   int result = mkdir(path, 0700);
#endif
   return result == 0 ||
      (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static bool path_is_valid(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0;
}

static RFILE *filestream_open(const char *path, unsigned mode, unsigned hint)
{
   FILE *file;
   RFILE *result;
   if (mode == RETRO_VFS_FILE_ACCESS_READ && fail_read)
      return NULL;
#ifdef _WIN32
   {
      wchar_t *wide = utf8_to_utf16_string_alloc(path);
      if (!wide)
         return NULL;
      file = _wfopen(wide,
            mode == RETRO_VFS_FILE_ACCESS_WRITE ? L"wb" : L"rb");
      free(wide);
   }
#else
   file = fopen(path, mode == RETRO_VFS_FILE_ACCESS_WRITE ? "wb" : "rb");
#endif
   if (!file)
      return NULL;
   result = (RFILE *)malloc(sizeof(*result));
   assert(result);
   result->file = file;
   result->writing = mode == RETRO_VFS_FILE_ACCESS_WRITE;
   write_opens += result->writing;
   return result;
}

static int64_t filestream_get_size(RFILE *file)
{
   struct stat st;
   return fstat(fileno(file->file), &st) == 0 ? st.st_size : -1;
}

static int64_t filestream_read(RFILE *file, void *data, int64_t size)
{
   return (int64_t)fread(data, 1, (size_t)size, file->file);
}

static int64_t filestream_write(RFILE *file, const void *data, int64_t size)
{
   if (fail_write)
      size /= 2;
   return (int64_t)fwrite(data, 1, (size_t)size, file->file);
}

static int filestream_close(RFILE *file)
{
   int result = fclose(file->file);
   if (file->writing && fail_close)
      result = -1;
   if (!result)
      free(file);
   return result;
}

static int filestream_delete(const char *path)
{
#ifdef _WIN32
   wchar_t *wide = utf8_to_utf16_string_alloc(path);
   int result = wide ? _wremove(wide) : -1;
   free(wide);
   return result;
#else
   return remove(path);
#endif
}

#ifdef _WIN32
static wchar_t *utf8_to_utf16_string_alloc(const char *text)
{
   int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
   wchar_t *wide = (wchar_t *)malloc((size_t)length * sizeof(*wide));
   if (!length || !wide)
   {
      free(wide);
      return NULL;
   }
   assert(MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length));
   return wide;
}
#endif

static int filestream_rename(const char *old_path, const char *new_path)
{
   vfs_replace_attempts++;
   if (fail_replace)
      return -1;
#ifdef _WIN32
   {
      wchar_t *old_wide = utf8_to_utf16_string_alloc(old_path);
      wchar_t *new_wide = utf8_to_utf16_string_alloc(new_path);
      bool replaced = old_wide && new_wide &&
         MoveFileExW(old_wide, new_wide, MOVEFILE_REPLACE_EXISTING) != 0;
      free(old_wide);
      free(new_wide);
      return replaced ? 0 : -1;
   }
#else
   return rename(old_path, new_path);
#endif
}

static VkResult vkCreatePipelineCache(void *device,
      const VkPipelineCacheCreateInfo *info, void *allocator,
      VkPipelineCache *cache)
{
   if (info->initialDataSize && import_error)
      return import_error;
   *cache = (VkPipelineCache)calloc(1, sizeof(**cache));
   assert(*cache);
   if (info->initialDataSize)
   {
      assert(info->initialDataSize == sizeof(**cache));
      memcpy(*cache, info->pInitialData, sizeof(**cache));
   }
   return VK_SUCCESS;
}

static VkResult vkMergePipelineCaches(void *device, VkPipelineCache dest,
      unsigned count, const VkPipelineCache *sources)
{
   unsigned i;
   if (merge_error)
      return merge_error;
   for (i = 0; i < count; i++)
      dest->entries |= sources[i]->entries;
   return VK_SUCCESS;
}

static void vkDestroyPipelineCache(void *device, VkPipelineCache cache,
      void *allocator)
{
   free(cache);
}

static VkResult vkGetPipelineCacheData(void *device, VkPipelineCache cache,
      size_t *size, void *data)
{
   snapshot_queries++;
   mock_time += snapshot_delay;
   if (data)
   {
      assert(*size >= sizeof(*cache));
      memcpy(data, cache, sizeof(*cache));
   }
   *size = sizeof(*cache);
   return VK_SUCCESS;
}

/* Exercise the common atomic writer and the production persistence code. */
#include "atomic_write_under_test.h"
#include "persistence_under_test.h"

static Device make_device(void)
{
   Device device;
   memset(&device, 0, sizeof(device));
   device.device = (void *)(uintptr_t)1;
   device.gpu_props.vendorID = 0x5143;
   device.gpu_props.deviceID = 0x1234;
   memset(device.gpu_props.pipelineCacheUUID, 0xa5, VK_UUID_SIZE);
   device.pipeline_cache = (VkPipelineCache)calloc(1, sizeof(*device.pipeline_cache));
   assert(device.pipeline_cache);
   device.pipeline_cache->header_size = 32;
   device.pipeline_cache->header_version = 1;
   device.pipeline_cache->vendor_id = device.gpu_props.vendorID;
   device.pipeline_cache->device_id = device.gpu_props.deviceID;
   memcpy(device.pipeline_cache->uuid, device.gpu_props.pipelineCacheUUID, VK_UUID_SIZE);
   device_pipeline_cache_set_file_name(&device);
   return device;
}

static void new_roots(const char *tmp, unsigned number)
{
   snprintf(retro_base_directory, sizeof(retro_base_directory), "%s/system%u", tmp, number);
   snprintf(retro_save_directory, sizeof(retro_save_directory), "%s/save%u", tmp, number);
   assert(path_mkdir(retro_base_directory));
   assert(path_mkdir(retro_save_directory));
}

static void cache_path(Device *device, const char *root, char *path)
{
   assert(device_pipeline_cache_build_path(path, PATH_MAX_LENGTH, root,
            device->pipeline_cache_file_name, "test", true));
}

static uint64_t disk_entries(Device *device, const char *root)
{
   size_t size;
   bool failed;
   void *data = device_pipeline_cache_load_path(device, &size, root,
         device->pipeline_cache_file_name, "test", &failed);
   uint64_t entries;
   assert(data && !failed && size == sizeof(struct FakeCache));
   entries = ((struct FakeCache *)data)->entries;
   free(data);
   return entries;
}

static void test_selection_and_updates(const char *tmp)
{
   Device writer = make_device(), first = make_device(), second = make_device();
   char name[96];
   new_roots(tmp, 1);
   strcpy(name, writer.pipeline_cache_file_name);
   assert(strstr(name, "_v1") == NULL && strstr(name, ".bin") != NULL);
   writer.gpu_props.pipelineCacheUUID[0]++;
   device_pipeline_cache_set_file_name(&writer);
   assert(strcmp(name, writer.pipeline_cache_file_name) != 0);
   writer.gpu_props.pipelineCacheUUID[0]--;
   device_pipeline_cache_set_file_name(&writer);
   writer.pipeline_cache->entries = 1;
   assert(device_pipeline_cache_write(&writer, retro_base_directory, "system"));
#ifndef _WIN32
   assert(vfs_replace_attempts == 1);
#endif
   writer.pipeline_cache->entries = 2;
   assert(device_pipeline_cache_write(&writer, retro_save_directory, "save"));
   device_pipeline_cache_load(&first);
   device_pipeline_cache_load(&second);
   assert(first.pipeline_cache_has_data && second.pipeline_cache_has_data);
   assert(first.pipeline_cache->entries == 3 && second.pipeline_cache->entries == 3);
   first.pipeline_cache->entries |= 4;
   assert(device_pipeline_cache_write(&first, retro_base_directory, "system"));
   second.pipeline_cache->entries |= 8;
   assert(device_pipeline_cache_write(&second, retro_base_directory, "system"));
   assert(disk_entries(&second, retro_base_directory) == 15);
   free(writer.pipeline_cache);
   free(first.pipeline_cache);
   free(second.pipeline_cache);
}

static void test_faults_and_unchanged_data(const char *tmp)
{
   Device device = make_device();
   unsigned old_queries, old_writes;
   new_roots(tmp, 2);
   retro_save_directory[0] = '\0';
   device.pipeline_cache->entries = 1;
   device_pipeline_cache_mark_dirty(&device);
   assert(device.pipeline_cache_has_data);
   device_pipeline_cache_save(&device);
   assert(device.pipeline_cache_dirty_count == 0);
   old_queries = snapshot_queries;
   device_pipeline_cache_save(&device);
   assert(snapshot_queries == old_queries);
   old_writes = write_opens;
   device_pipeline_cache_mark_dirty(&device);
   device_pipeline_cache_save(&device);
   assert(write_opens == old_writes); /* Identical driver payload: no rewrite. */
   device.pipeline_cache->entries = 3;
   device_pipeline_cache_mark_dirty(&device);
   fail_close = true;
   device_pipeline_cache_save(&device);
   fail_close = false;
   assert(device.pipeline_cache_dirty_count == 1);
   assert(disk_entries(&device, retro_base_directory) == 1);
   fail_write = true;
   device_pipeline_cache_save(&device);
   fail_write = false;
   assert(disk_entries(&device, retro_base_directory) == 1);
   fail_replace = true;
   device_pipeline_cache_save(&device);
   fail_replace = false;
   assert(disk_entries(&device, retro_base_directory) == 1);
   fail_read = true;
   device_pipeline_cache_save(&device);
   fail_read = false;
   assert(disk_entries(&device, retro_base_directory) == 1);
   merge_error = VK_ERROR_OUT_OF_HOST_MEMORY;
   device_pipeline_cache_save(&device);
   merge_error = 0;
   assert(disk_entries(&device, retro_base_directory) == 1);
   device_pipeline_cache_save(&device);
   assert(device.pipeline_cache_dirty_count == 0);
   assert(disk_entries(&device, retro_base_directory) == 3);
   free(device.pipeline_cache);
}

static void test_lock_and_fallback(const char *tmp)
{
   Device writer = make_device(), reader = make_device();
   struct RhiPipelineCacheLock lock;
   char path[PATH_MAX_LENGTH];
#ifndef _WIN32
   pid_t holder;
   int holder_release;
#endif
   new_roots(tmp, 3);
   writer.pipeline_cache->entries = 1;
   assert(device_pipeline_cache_write(&writer, retro_base_directory, "system"));
   cache_path(&writer, retro_base_directory, path);
   assert(device_pipeline_cache_lock(path, &lock));
   {
#ifdef _WIN32
      char executable[PATH_MAX_LENGTH], command[PATH_MAX_LENGTH * 2 + 32];
      STARTUPINFOA startup;
      PROCESS_INFORMATION child;
      DWORD status;
      memset(&startup, 0, sizeof(startup));
      startup.cb = sizeof(startup);
      assert(GetModuleFileNameA(NULL, executable, sizeof(executable)));
      snprintf(command, sizeof(command), "\"%s\" --lock-test \"%s\"", executable, path);
      assert(CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
               NULL, NULL, &startup, &child));
      assert(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0);
      assert(GetExitCodeProcess(child.hProcess, &status) && status == 0);
      CloseHandle(child.hThread);
      CloseHandle(child.hProcess);
#else
      pid_t child = fork();
      int status;
      assert(child >= 0);
      if (child == 0)
      {
         struct RhiPipelineCacheLock other;
         _exit(device_pipeline_cache_lock(path, &other) ? 1 : 0);
      }
      assert(waitpid(child, &status, 0) == child);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
   }
#ifndef _WIN32
   /* POSIX record locks belong to a process, so a second lock attempt by this
    * process would succeed. Have another process hold the lock while the
    * writer exercises the fallback path. */
   device_pipeline_cache_unlock(&lock);
   {
      int ready_pipe[2], release_pipe[2];
      char signal;
      assert(pipe(ready_pipe) == 0);
      assert(pipe(release_pipe) == 0);
      holder = fork();
      assert(holder >= 0);
      if (holder == 0)
      {
         struct RhiPipelineCacheLock other;
         close(ready_pipe[0]);
         close(release_pipe[1]);
         if (!device_pipeline_cache_lock(path, &other) ||
             write(ready_pipe[1], "R", 1) != 1 ||
             read(release_pipe[0], &signal, 1) != 1)
            _exit(2);
         device_pipeline_cache_unlock(&other);
         _exit(0);
      }
      close(ready_pipe[1]);
      close(release_pipe[0]);
      assert(read(ready_pipe[0], &signal, 1) == 1);
      close(ready_pipe[0]);
      holder_release = release_pipe[1];
   }
#endif
   writer.pipeline_cache->entries = 3;
   device_pipeline_cache_mark_dirty(&writer);
   device_pipeline_cache_save(&writer); /* Busy SYSTEM falls back to SAVE. */
   assert(writer.pipeline_cache_dirty_count == 0);
   assert(disk_entries(&writer, retro_base_directory) == 1);
   assert(disk_entries(&writer, retro_save_directory) == 3);
#ifdef _WIN32
   device_pipeline_cache_unlock(&lock);
#else
   {
      int status;
      assert(write(holder_release, "X", 1) == 1);
      close(holder_release);
      assert(waitpid(holder, &status, 0) == holder);
      assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
#endif
   device_pipeline_cache_load(&reader);
   assert(reader.pipeline_cache->entries == 3); /* Stale SYSTEM cannot shadow SAVE. */
   free(writer.pipeline_cache);
   free(reader.pipeline_cache);
}

static void test_validation_and_quarantine(const char *tmp)
{
   Device device = make_device(), reader = make_device();
   char path[PATH_MAX_LENGTH], rejected_path[PATH_MAX_LENGTH];
   FILE *file;
   size_t size;
   bool failed;
   void *data;
   unsigned index;
   static const long corrupt_offsets[] = { 0, 8, 12, 16, 24, 32, 40 };
   new_roots(tmp, 4);
   device.pipeline_cache->entries = 7;
   cache_path(&device, retro_base_directory, path);
   for (index = 0; index < sizeof(corrupt_offsets) / sizeof(corrupt_offsets[0]); index++)
   {
      assert(device_pipeline_cache_write(&device, retro_base_directory, "system"));
      file = fopen(path, "r+b");
      assert(file);
      assert(fseek(file, corrupt_offsets[index], SEEK_SET) == 0);
      assert(fputc(0xff, file) != EOF);
      assert(fclose(file) == 0);
      data = device_pipeline_cache_load_path(&device, &size, retro_base_directory,
            device.pipeline_cache_file_name, "test", &failed);
      assert(!data && !failed && path_is_valid(path));
   }
   assert(device_pipeline_cache_write(&device, retro_base_directory, "system"));
   import_error = VK_ERROR_OUT_OF_HOST_MEMORY;
   device_pipeline_cache_load(&reader);
   assert(path_is_valid(path)); /* Allocation failures are not bad blobs. */
   import_error = VK_ERROR_DEVICE_LOST;
   device_pipeline_cache_load(&reader);
   assert(path_is_valid(path));
   import_error = -3;
   device_pipeline_cache_load(&reader);
   assert(!path_is_valid(path)); /* Rejection under lock moves it aside. */
   assert(snprintf(rejected_path, sizeof(rejected_path), "%s.rejected", path) > 0);
   assert(path_is_valid(rejected_path));

   /* A later rejection atomically replaces the one stable quarantine instead
    * of creating an unbounded series of uniquely named siblings. */
   import_error = 0;
   device.pipeline_cache->entries = 9;
   assert(device_pipeline_cache_write(&device, retro_base_directory, "system"));
   import_error = -3;
   device_pipeline_cache_load(&reader);
   assert(!path_is_valid(path) && path_is_valid(rejected_path));
   import_error = 0;
   {
      char rejected_name[128];
      snprintf(rejected_name, sizeof(rejected_name), "%s.rejected",
            device.pipeline_cache_file_name);
      data = device_pipeline_cache_load_path(&device, &size,
            retro_base_directory, rejected_name, "test", &failed);
      assert(data && !failed && size == sizeof(struct FakeCache));
      assert(((struct FakeCache *)data)->entries == 9);
      free(data);
   }
   free(device.pipeline_cache);
   free(reader.pipeline_cache);
}

static void test_device_header(void)
{
   Device device = make_device();
   struct FakeCache cache = *device.pipeline_cache;
   assert(device_pipeline_cache_data_compatible(&device, &cache, sizeof(cache)));
   assert(!device_pipeline_cache_data_compatible(&device, &cache, 31));
   cache.header_size = 31;
   assert(!device_pipeline_cache_data_compatible(&device, &cache, sizeof(cache)));
   cache = *device.pipeline_cache;
   cache.header_size = sizeof(cache) + 1;
   assert(!device_pipeline_cache_data_compatible(&device, &cache, sizeof(cache)));
   cache = *device.pipeline_cache;
   cache.header_version++;
   assert(!device_pipeline_cache_data_compatible(&device, &cache, sizeof(cache)));
   cache = *device.pipeline_cache;
   cache.vendor_id++;
   assert(!device_pipeline_cache_data_compatible(&device, &cache, sizeof(cache)));
   cache = *device.pipeline_cache;
   cache.device_id++;
   assert(!device_pipeline_cache_data_compatible(&device, &cache, sizeof(cache)));
   cache = *device.pipeline_cache;
   cache.uuid[0]++;
   assert(!device_pipeline_cache_data_compatible(&device, &cache, sizeof(cache)));
   free(device.pipeline_cache);
}

static void test_large_cache(const char *tmp)
{
   Device device = make_device();
   char path[PATH_MAX_LENGTH];
   size_t payload_size = 17u * 1024u * 1024u;
   size_t size;
   size_t file_size = payload_size + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE;
   uint8_t *bytes = (uint8_t *)calloc(1, file_size);
   void *data;
   bool failed;
   FILE *file;
   assert(bytes);
   new_roots(tmp, 5);
   cache_path(&device, retro_base_directory, path);
   memcpy(bytes, VULKAN_PIPELINE_CACHE_FILE_MAGIC, 8);
   pipeline_cache_write_u32_le(bytes + 8, VULKAN_PIPELINE_CACHE_FILE_VERSION);
   pipeline_cache_write_u32_le(bytes + 12, (uint32_t)payload_size);
   memcpy(bytes + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE, device.pipeline_cache, 32);
   pipeline_cache_write_u32_le(bytes + 16, encoding_crc32(0,
            bytes + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE, payload_size));
   file = fopen(path, "wb");
   assert(file && fwrite(bytes, 1, file_size, file) == file_size);
   assert(fclose(file) == 0);
   free(bytes);
   data = device_pipeline_cache_load_path(&device, &size, retro_base_directory,
         device.pipeline_cache_file_name, "test", &failed);
   assert(data && !failed && size == payload_size);
   free(data);
   file = fopen(path, "r+b");
   assert(file);
#ifdef _WIN32
   assert(_chsize_s(_fileno(file),
            (__int64)VULKAN_PIPELINE_CACHE_MAX_DATA_SIZE + 25) == 0);
#else
   assert(ftruncate(fileno(file),
            (off_t)VULKAN_PIPELINE_CACHE_MAX_DATA_SIZE + 25) == 0);
#endif
   assert(fclose(file) == 0);
   data = device_pipeline_cache_load_path(&device, &size, retro_base_directory,
         device.pipeline_cache_file_name, "test", &failed);
   assert(!data && failed && path_is_valid(path));
   free(device.pipeline_cache);
}

#ifdef BEETLE_VULKAN_PIPELINE_DIAGNOSTICS
static void test_save_timing(const char *tmp)
{
   Device device = make_device();
   new_roots(tmp, 6);
   device.pipeline_cache->entries = 1;
   snapshot_delay = 9000; /* Two driver queries cross the 16.67 ms budget. */
   device_pipeline_cache_mark_dirty(&device);
   device_pipeline_cache_save(&device);
   assert(strstr(save_log, "elapsed_usec=18000"));
   assert(strstr(save_attempt_log, "result=written"));
   assert(strstr(save_attempt_log, "bytes=40"));
   device_pipeline_cache_mark_dirty(&device);
   device_pipeline_cache_save(&device);
   assert(strstr(save_attempt_log, "result=unchanged"));
   snapshot_delay = 0;
   free(device.pipeline_cache);
}
#endif

#ifdef _WIN32
static void test_windows_native_paths(const char *tmp)
{
   char directory[PATH_MAX_LENGTH], path[PATH_MAX_LENGTH], temp[PATH_MAX_LENGTH];
   wchar_t *directory_wide, *path_wide, *temp_wide;
   struct RhiPipelineCacheLock lock;
   HANDLE file;
   DWORD count;
   char bytes[3];
   size_t length;
   /* Exercise the production UTF-8 -> UTF-16 helpers with non-ASCII paths. */
   snprintf(directory, sizeof(directory), "%s/unicode-\xc3\xa9-\xe6\xbc\xa2", tmp);
   directory_wide = utf8_to_utf16_string_alloc(directory);
   assert(directory_wide && CreateDirectoryW(directory_wide, NULL));
   length = strlen(directory);
   assert(length + sizeof("/cache.bin.tmp") <= sizeof(temp));
   memcpy(path, directory, length);
   memcpy(path + length, "/cache.bin", sizeof("/cache.bin"));
   memcpy(temp, path, strlen(path));
   memcpy(temp + strlen(path), ".tmp", sizeof(".tmp"));
   path_wide = utf8_to_utf16_string_alloc(path);
   temp_wide = utf8_to_utf16_string_alloc(temp);
   assert(path_wide && temp_wide && device_pipeline_cache_lock(path, &lock));
   file = CreateFileW(path_wide, GENERIC_WRITE, FILE_SHARE_READ, NULL,
         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   assert(file != INVALID_HANDLE_VALUE);
   assert(WriteFile(file, "old", 3, &count, NULL) && count == 3);
   assert(CloseHandle(file));
   /* A real Windows sharing violation must preserve the old destination;
    * the common helper also removes the failed stable temporary. */
   file = CreateFileW(path_wide, GENERIC_READ, FILE_SHARE_READ, NULL,
         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   assert(file != INVALID_HANDLE_VALUE);
   assert(!filestream_write_file_atomic(path, "new", 3));
   assert(ReadFile(file, bytes, sizeof(bytes), &count, NULL) && count == 3);
   assert(memcmp(bytes, "old", 3) == 0);
   assert(GetFileAttributesW(temp_wide) == INVALID_FILE_ATTRIBUTES);
   assert(CloseHandle(file));
   assert(filestream_write_file_atomic(path, "new", 3));
   assert(GetFileAttributesW(temp_wide) == INVALID_FILE_ATTRIBUTES);
   file = CreateFileW(path_wide, GENERIC_READ, FILE_SHARE_READ, NULL,
         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   assert(file != INVALID_HANDLE_VALUE);
   assert(ReadFile(file, bytes, sizeof(bytes), &count, NULL) && count == 3);
   assert(memcmp(bytes, "new", 3) == 0);
   assert(CloseHandle(file));
   device_pipeline_cache_unlock(&lock);
   free(directory_wide);
   free(path_wide);
   free(temp_wide);
}
#endif

int main(int argc, char **argv)
{
   if (argc == 3 && strcmp(argv[1], "--lock-test") == 0)
   {
      struct RhiPipelineCacheLock lock;
      if (!device_pipeline_cache_lock(argv[2], &lock))
         return 0;
      device_pipeline_cache_unlock(&lock);
      return 1;
   }
   assert(argc == 2);
   test_selection_and_updates(argv[1]);
   test_faults_and_unchanged_data(argv[1]);
   test_lock_and_fallback(argv[1]);
   test_validation_and_quarantine(argv[1]);
   test_device_header();
   test_large_cache(argv[1]);
#ifdef BEETLE_VULKAN_PIPELINE_DIAGNOSTICS
   test_save_timing(argv[1]);
#endif
#ifdef _WIN32
   test_windows_native_paths(argv[1]);
#endif
   puts("Vulkan cache persistence: selection, merging, locking, VFS publication, fallback, write/close/rename/read failures, unchanged-data suppression, bounded quarantine, and size bounds passed.");
   return 0;
}

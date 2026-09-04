/* Internal persistent VkPipelineCache implementation. Included once from
 * rhi_lib_vulkan.c after Device is defined; keeping it in the same translation
 * unit avoids exposing private renderer state through another interface. */
#define VULKAN_PIPELINE_CACHE_DIR_NAME "Beetle PSX HW"
#define VULKAN_PIPELINE_CACHE_FILE_MAGIC "BPSXVKC1"
/* This version is stored in every cache file. Increment it only when a hard
 * incompatibility makes the wrapper format unreadable; Vulkan's own payload
 * header separately handles GPU and driver compatibility. */
#define VULKAN_PIPELINE_CACHE_FILE_VERSION 1u
#define VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE 24u
#define VULKAN_PIPELINE_CACHE_MAX_DATA_SIZE (256u * 1024u * 1024u)

#if defined(_WIN32)
#include <encodings/utf.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

   struct RhiPipelineCacheLock
   {
#if defined(_WIN32)
      HANDLE handle;
#else
      int fd;
#endif
   };

   static void device_pipeline_cache_set_file_name(Device *self)
   {
      char uuid[VK_UUID_SIZE * 2 + 1];
      static const char hex[] = "0123456789abcdef";
      unsigned i;
      for (i = 0; i < VK_UUID_SIZE; i++)
      {
         uuid[2 * i] = hex[self->gpu_props.pipelineCacheUUID[i] >> 4];
         uuid[2 * i + 1] = hex[self->gpu_props.pipelineCacheUUID[i] & 15];
      }
      uuid[VK_UUID_SIZE * 2] = '\0';
      snprintf(self->pipeline_cache_file_name,
            sizeof(self->pipeline_cache_file_name),
            "vulkan_pipeline_cache_%08x_%08x_%s.bin",
            (unsigned)self->gpu_props.vendorID,
            (unsigned)self->gpu_props.deviceID, uuid);
   }
   /* A persistent lock inode/handle is essential: unlinking a lock file would
    * let a waiting process and a new process lock different objects. Locks are
    * nonblocking and released by the OS after a crash; a busy writer leaves
    * the cache dirty for the next safe save rather than blocking. */
   static bool device_pipeline_cache_lock(const char *path,
         struct RhiPipelineCacheLock *lock)
   {
      char lock_path[PATH_MAX_LENGTH];
      int length = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
      if (length < 0 || (size_t)length >= sizeof(lock_path))
         return false;
#if defined(_WIN32)
      {
         wchar_t *wide = utf8_to_utf16_string_alloc(lock_path);
         OVERLAPPED overlap;
         if (!wide)
            return false;
         lock->handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
               FILE_ATTRIBUTE_NORMAL, NULL);
         free(wide);
         if (lock->handle == INVALID_HANDLE_VALUE)
            return false;
         memset(&overlap, 0, sizeof(overlap));
         if (!LockFileEx(lock->handle, LOCKFILE_EXCLUSIVE_LOCK |
                  LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap))
         {
            CloseHandle(lock->handle);
            return false;
         }
         return true;
      }
#elif defined(__unix__) || defined(__APPLE__)
      {
         struct flock file_lock;
         lock->fd = open(lock_path, O_CREAT | O_RDWR, 0600);
         if (lock->fd < 0)
         {
            LOGE("[Vulkan pipeline cache] could not open lock errno=%d\n",
                  errno);
            return false;
         }
         memset(&file_lock, 0, sizeof(file_lock));
         /* Prefer standardized record locking to BSD flock. Android's
          * emulated-storage FUSE path may reject the latter. */
         file_lock.l_type = F_WRLCK;
         file_lock.l_whence = SEEK_SET;
         file_lock.l_len = 1;
         if (fcntl(lock->fd, F_SETLK, &file_lock) != 0)
         {
            LOGI("[Vulkan pipeline cache] lock unavailable errno=%d\n",
                  errno);
            close(lock->fd);
            return false;
         }
         return true;
      }
#else
      /* Libretro VFS has no exclusive locking or atomic-replace contract.
       * Reading such paths remains supported; never risk an existing cache
       * by pretending an uncoordinated VFS write is an atomic transaction. */
      (void)lock;
      return false;
#endif
   }

   static void device_pipeline_cache_unlock(struct RhiPipelineCacheLock *lock)
   {
#if defined(_WIN32)
      CloseHandle(lock->handle);
#elif defined(__unix__) || defined(__APPLE__)
      close(lock->fd);
#else
      (void)lock;
#endif
   }

   struct RhiPipelineCacheHeaderV1
   {
      uint32_t header_size;
      uint32_t header_version;
      uint32_t vendor_id;
      uint32_t device_id;
      uint8_t uuid[VK_UUID_SIZE];
   };
   RHI_STATIC_ASSERT(sizeof(struct RhiPipelineCacheHeaderV1) == 32,
         pipeline_cache_header_must_be_32_bytes);

   static uint32_t pipeline_cache_read_u32_le(const uint8_t *data)
   {
      return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
   }

   static void pipeline_cache_write_u32_le(uint8_t *data, uint32_t value)
   {
      data[0] = (uint8_t)(value >> 0);
      data[1] = (uint8_t)(value >> 8);
      data[2] = (uint8_t)(value >> 16);
      data[3] = (uint8_t)(value >> 24);
   }

   static bool device_pipeline_cache_build_path(char *path,
         size_t path_size, const char *root, const char *file_name,
         const char *location, bool create_dir)
   {
      char cache_dir[PATH_MAX_LENGTH];
      size_t needed;

      if (!root || root[0] == '\0')
         return false;

      needed = strlen(root) + 1 + strlen(VULKAN_PIPELINE_CACHE_DIR_NAME) + 1;
      if (needed > sizeof(cache_dir))
      {
         LOGE("[Vulkan pipeline cache] %s directory path is too long\n",
               location);
         return false;
      }
      fill_pathname_join_special(cache_dir, root,
            VULKAN_PIPELINE_CACHE_DIR_NAME, sizeof(cache_dir));

      if (create_dir && !path_mkdir(cache_dir))
      {
         LOGE("[Vulkan pipeline cache] could not create %s cache directory: %s\n",
               location, cache_dir);
         return false;
      }

      needed = strlen(cache_dir) + 1 + strlen(file_name) + 1;
      if (needed > path_size)
      {
         LOGE("[Vulkan pipeline cache] %s file path is too long\n",
               location);
         return false;
      }
      fill_pathname_join_special(path, cache_dir,
            file_name, path_size);
      return true;
   }

   static bool device_pipeline_cache_directories_differ(void)
   {
      return retro_save_directory[0] != '\0' &&
         strcmp(retro_base_directory, retro_save_directory) != 0;
   }

   static void device_pipeline_cache_ignore(const char *path,
         const char *reason)
   {
      /* Do not delete from the read path: another process may have replaced
       * this file since it was opened. A later locked save can replace it. */
      LOGI("[Vulkan pipeline cache] ignored %s cache: %s\n", reason, path);
   }

   static bool device_pipeline_cache_data_compatible(const Device *self,
         const void *data, size_t size)
   {
      struct RhiPipelineCacheHeaderV1 header;

      if (size < sizeof(header))
         return false;
      memcpy(&header, data, sizeof(header));
      return header.header_size >= sizeof(header) &&
         header.header_size <= size &&
         header.header_version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
         header.vendor_id == self->gpu_props.vendorID &&
         header.device_id == self->gpu_props.deviceID &&
         memcmp(header.uuid, self->gpu_props.pipelineCacheUUID,
               VK_UUID_SIZE) == 0;
   }

   static void *device_pipeline_cache_load_path(Device *self,
          size_t *data_size, const char *root, const char *file_name,
          const char *location, bool *read_failed)
   {
      char path[PATH_MAX_LENGTH];
      RFILE *file;
      int64_t file_size;
      int64_t bytes_read;
      uint8_t *file_data;
      uint32_t file_version;
      uint32_t payload_size;
      uint32_t payload_crc;

      *data_size = 0;
      *read_failed = false;

      if (!device_pipeline_cache_build_path(path, sizeof(path), root,
            file_name, location, false))
         return NULL;

      file = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if (!file)
      {
         *read_failed = path_is_valid(path);
         return NULL;
      }

      file_size = filestream_get_size(file);
      if (file_size < (int64_t)(VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE + 32u) ||
          file_size > (int64_t)(VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE +
             VULKAN_PIPELINE_CACHE_MAX_DATA_SIZE))
      {
         /* An unreadable or oversized existing cache must not be replaced by
          * a smaller snapshot that could not merge its existing entries. */
         *read_failed = file_size < 0 || file_size >
            (int64_t)(VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE +
               VULKAN_PIPELINE_CACHE_MAX_DATA_SIZE);
         LOGI("[Vulkan pipeline cache] invalid cache size=%lld path=%s\n",
               (long long)file_size, path);
         filestream_close(file);
         device_pipeline_cache_ignore(path, "invalid");
         return NULL;
      }

      file_data = (uint8_t *)malloc((size_t)file_size);
      if (!file_data)
      {
         *read_failed = true;
         LOGE("[Vulkan pipeline cache] could not allocate %lld bytes for load\n",
               (long long)file_size);
         filestream_close(file);
         return NULL;
      }

      bytes_read = filestream_read(file, file_data, file_size);
      filestream_close(file);
      if (bytes_read != file_size)
      {
         *read_failed = true;
         LOGI("[Vulkan pipeline cache] short read=%lld expected=%lld path=%s\n",
               (long long)bytes_read, (long long)file_size, path);
         free(file_data);
         device_pipeline_cache_ignore(path, "unreadable");
         return NULL;
      }

      file_version = pipeline_cache_read_u32_le(file_data + 8);
      payload_size = pipeline_cache_read_u32_le(file_data + 12);
      payload_crc = pipeline_cache_read_u32_le(file_data + 16);
      if (memcmp(file_data, VULKAN_PIPELINE_CACHE_FILE_MAGIC, 8) != 0)
      {
         LOGI("[Vulkan pipeline cache] unknown cache signature path=%s\n",
               path);
         free(file_data);
         device_pipeline_cache_ignore(path, "unknown");
         return NULL;
      }
      if (file_version != VULKAN_PIPELINE_CACHE_FILE_VERSION)
      {
         LOGI("[Vulkan pipeline cache] incompatible file version=%u expected=%u path=%s\n",
               file_version, VULKAN_PIPELINE_CACHE_FILE_VERSION, path);
         free(file_data);
         device_pipeline_cache_ignore(path, "incompatible-version");
         return NULL;
      }
      if (payload_size != (uint32_t)(file_size -
             VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE) ||
          encoding_crc32(0, file_data + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE,
             payload_size) != payload_crc)
      {
         LOGI("[Vulkan pipeline cache] corrupt cache payload path=%s\n",
               path);
         free(file_data);
         device_pipeline_cache_ignore(path, "corrupt");
         return NULL;
      }

      if (!device_pipeline_cache_data_compatible(self,
            file_data + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE, payload_size))
      {
         LOGI("[Vulkan pipeline cache] incompatible GPU/driver cache path=%s\n",
               path);
         free(file_data);
         device_pipeline_cache_ignore(path, "incompatible-driver");
         return NULL;
      }

      memmove(file_data, file_data + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE,
            payload_size);
      *data_size = payload_size;
      return file_data;
   }

   /* Called only with this file's native lock held. Keep exactly the newest
    * driver-rejected blob at a stable sibling path. Atomic replacement bounds
    * quarantine storage to one file per GPU/location while retaining useful
    * evidence for diagnosis. If replacement fails, the primary stays intact. */
   static void device_pipeline_cache_quarantine(const char *path)
   {
      char rejected_path[PATH_MAX_LENGTH];
      int length = snprintf(rejected_path, sizeof(rejected_path),
            "%s.rejected", path);
      if (length >= 0 && (size_t)length < sizeof(rejected_path) &&
            filestream_rename(path, rejected_path) == 0)
         LOGI("[Vulkan pipeline cache] quarantined driver-rejected data\n");
   }

   static bool device_pipeline_cache_merge_data(Device *self, const void *data,
         size_t data_size, bool *rejected)
   {
      VkPipelineCache source = VK_NULL_HANDLE;
      VkPipelineCacheCreateInfo info = {
         VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
      };
      VkResult result;
      *rejected = false;
      info.initialDataSize = data_size;
      info.pInitialData = data;
      result = vkCreatePipelineCache(self->device, &info, NULL, &source);
      if (result != VK_SUCCESS)
      {
         /* Resource/device failures do not demonstrate a bad driver blob. */
         *rejected = result != VK_ERROR_OUT_OF_HOST_MEMORY &&
            result != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
            result != VK_ERROR_DEVICE_LOST;
         LOGI("[Vulkan pipeline cache] could not import persistent data result=%d\n",
               (int)result);
         return false;
      }
      result = vkMergePipelineCaches(self->device, self->pipeline_cache,
            1, &source);
      vkDestroyPipelineCache(self->device, source, NULL);
      if (result != VK_SUCCESS)
      {
         LOGE("[Vulkan pipeline cache] merge failed result=%d\n", (int)result);
         return false;
      }
      return true;
   }

   static bool device_pipeline_cache_seed_path(Device *self, const char *root,
         const char *location)
   {
      char path[PATH_MAX_LENGTH];
      struct RhiPipelineCacheLock lock;
      size_t size = 0;
      void *data;
      bool locked;
      bool merged = false;
      bool rejected = false;
      bool read_failed;
      if (!device_pipeline_cache_build_path(path, sizeof(path), root,
               self->pipeline_cache_file_name, location, false))
         return false;
      locked = device_pipeline_cache_lock(path, &lock);
      data = device_pipeline_cache_load_path(self, &size, root,
            self->pipeline_cache_file_name, location, &read_failed);
      if (data)
      {
         merged = device_pipeline_cache_merge_data(self, data, size, &rejected);
         if (merged)
            LOGI("[Vulkan pipeline cache] seeded bytes=%llu location=%s\n",
                  (unsigned long long)size, location);
         else if (rejected && locked)
            device_pipeline_cache_quarantine(path);
         free(data);
      }
      if (locked)
         device_pipeline_cache_unlock(&lock);
      return merged;
   }

   static void device_pipeline_cache_load(Device *self)
   {
      bool seeded = device_pipeline_cache_seed_path(self,
            retro_base_directory, "system");
      /* A readable but unwritable SYSTEM file must not shadow a newer SAVE
       * fallback. Merge both valid candidates, even when SYSTEM succeeded. */
      if (device_pipeline_cache_directories_differ())
         seeded = device_pipeline_cache_seed_path(self,
               retro_save_directory, "save") || seeded;
      self->pipeline_cache_has_data = seeded;
      if (!seeded)
         LOGI("[Vulkan pipeline cache] cold start; no compatible cache found\n");
   }

   static uint8_t *device_pipeline_cache_snapshot(Device *self,
         size_t *payload_size)
   {
      size_t data_size = 0;
      size_t actual_size;
      uint8_t *file_data;
      VkResult result;

      result = vkGetPipelineCacheData(self->device, self->pipeline_cache,
            &data_size, NULL);
      if (result != VK_SUCCESS || data_size == 0 ||
          data_size > VULKAN_PIPELINE_CACHE_MAX_DATA_SIZE)
      {
         LOGE("[Vulkan pipeline cache] save size query failed result=%d bytes=%llu\n",
               (int)result, (unsigned long long)data_size);
         return NULL;
      }

      file_data = (uint8_t *)malloc(
            VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE + data_size);
      if (!file_data)
      {
         LOGE("[Vulkan pipeline cache] could not allocate %llu bytes for save\n",
               (unsigned long long)data_size);
         return NULL;
      }

      actual_size = data_size;
      result = vkGetPipelineCacheData(self->device, self->pipeline_cache,
            &actual_size, file_data + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE);
      if (result != VK_SUCCESS || actual_size == 0 ||
          actual_size > data_size ||
          !device_pipeline_cache_data_compatible(self,
             file_data + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE, actual_size))
      {
         LOGE("[Vulkan pipeline cache] save data query failed result=%d bytes=%llu\n",
               (int)result, (unsigned long long)actual_size);
         free(file_data);
         return NULL;
      }

      memcpy(file_data, VULKAN_PIPELINE_CACHE_FILE_MAGIC, 8);
      pipeline_cache_write_u32_le(file_data + 8,
            VULKAN_PIPELINE_CACHE_FILE_VERSION);
      pipeline_cache_write_u32_le(file_data + 12, (uint32_t)actual_size);
      pipeline_cache_write_u32_le(file_data + 16,
            encoding_crc32(0,
               file_data + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE,
               actual_size));
      pipeline_cache_write_u32_le(file_data + 20, 0);
      *payload_size = actual_size;
      return file_data;
   }

   static bool device_pipeline_cache_write(Device *self, const char *root,
         const char *location)
   {
      char path[PATH_MAX_LENGTH];
      struct RhiPipelineCacheLock lock;
      size_t old_size = 0;
      size_t payload_size = 0;
      size_t file_size;
      void *old_data = NULL;
      uint8_t *file_data = NULL;
      bool rejected = false;
      bool saved = false;
      bool read_failed;
#ifdef BEETLE_VULKAN_PIPELINE_DIAGNOSTICS
      retro_time_t start_usec = cpu_features_get_time_usec();
      bool unchanged = false;
#endif

      if (!device_pipeline_cache_build_path(path, sizeof(path), root,
               self->pipeline_cache_file_name, location, true) ||
          !device_pipeline_cache_lock(path, &lock))
      {
#ifdef BEETLE_VULKAN_PIPELINE_DIAGNOSTICS
         LOGI("[Vulkan pipeline cache] save-attempt location=%s result=deferred bytes=0 elapsed_usec=%lld\n",
               location, (long long)(cpu_features_get_time_usec() - start_usec));
#endif
         return false;
      }

      /* Merge the latest disk state while holding the destination lock, not
       * just the startup snapshot. Two running cores can then add pipelines
       * without the later writer erasing the earlier writer's discoveries. */
      old_data = device_pipeline_cache_load_path(self, &old_size, root,
            self->pipeline_cache_file_name, location, &read_failed);
      if (read_failed)
         goto done;
      if (old_data && !device_pipeline_cache_merge_data(self, old_data,
               old_size, &rejected))
      {
         if (!rejected)
            goto done;
         device_pipeline_cache_quarantine(path);
         free(old_data);
         old_data = NULL;
      }

      file_data = device_pipeline_cache_snapshot(self, &payload_size);
      if (!file_data)
         goto done;
      if (old_data && old_size == payload_size &&
          memcmp(old_data, file_data + VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE,
             payload_size) == 0)
      {
         saved = true;
#ifdef BEETLE_VULKAN_PIPELINE_DIAGNOSTICS
         unchanged = true;
#endif
         goto done;
      }
      file_size = VULKAN_PIPELINE_CACHE_FILE_HEADER_SIZE + payload_size;
      /* The destination lock makes one stable sibling temporary sufficient.
       * The common helper truncates a crash leftover, verifies write/close,
       * atomically replaces the destination and removes a failed temporary. */
      saved = filestream_write_file_atomic(path, file_data,
            (int64_t)file_size);
      if (saved)
         LOGI("[Vulkan pipeline cache] saved bytes=%llu location=%s\n",
               (unsigned long long)payload_size, location);
      else
         LOGE("[Vulkan pipeline cache] atomic write failed location=%s bytes=%llu\n",
               location, (unsigned long long)file_size);
done:
      free(file_data);
      free(old_data);
      device_pipeline_cache_unlock(&lock);
#ifdef BEETLE_VULKAN_PIPELINE_DIAGNOSTICS
      LOGI("[Vulkan pipeline cache] save-attempt location=%s result=%s bytes=%llu elapsed_usec=%lld\n",
            location, unchanged ? "unchanged" : (saved ? "written" : "failed"),
            (unsigned long long)payload_size,
            (long long)(cpu_features_get_time_usec() - start_usec));
#endif
      return saved;
   }

   /* Publication and persistence saves run on the render thread after worker joins.
    * Do not mark from compiler workers: successful publication, not a duplicate
    * or failed driver creation, is the unit of newly useful cached work. */
   static void device_pipeline_cache_mark_dirty(Device *self)
   {
      self->pipeline_cache_has_data = true;
      if (self->pipeline_cache_dirty_count != ~0u)
         self->pipeline_cache_dirty_count++;
   }

   static void device_pipeline_cache_save(Device *self)
   {
      bool saved;
#ifdef BEETLE_VULKAN_PIPELINE_DIAGNOSTICS
      retro_time_t start_usec;
      unsigned dirty_count;
#endif
      if (self->device == VK_NULL_HANDLE ||
          self->pipeline_cache == VK_NULL_HANDLE ||
          self->pipeline_cache_dirty_count == 0)
         return;
#ifdef BEETLE_VULKAN_PIPELINE_DIAGNOSTICS
      start_usec = cpu_features_get_time_usec();
      dirty_count = self->pipeline_cache_dirty_count;
#endif
      saved = device_pipeline_cache_write(self, retro_base_directory, "system");
      if (!saved && device_pipeline_cache_directories_differ())
         saved = device_pipeline_cache_write(self, retro_save_directory, "save");
#ifdef BEETLE_VULKAN_PIPELINE_DIAGNOSTICS
      LOGI("[Vulkan pipeline cache] save dirty=%u result=%s elapsed_usec=%lld\n",
            dirty_count, saved ? "saved" : "deferred",
            (long long)(cpu_features_get_time_usec() - start_usec));
#endif
      if (saved)
      {
         self->pipeline_cache_dirty_count = 0;
         self->pipeline_cache_storage_warned = false;
      }
      else if (!self->pipeline_cache_storage_warned)
      {
         LOGI("[Vulkan pipeline cache] save deferred: no available locked, atomically replaceable system/save path; retaining dirty state\n");
         self->pipeline_cache_storage_warned = true;
      }
   }

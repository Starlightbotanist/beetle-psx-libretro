# Vulkan pipeline regression tests

The tests extract production code and use deterministic Vulkan, allocator and
filesystem doubles. They need no game data or GPU.

From the repository root on a POSIX host:

```
python3 tools/vulkan/test_recipes.py
python3 tools/vulkan/test_allocation.py
python3 tools/vulkan/test_precompile_jobs.py
python3 tools/vulkan_cache/test_persistence.py
```

The allocation suite uses ASan/UBSan. Under WSL with MinGW and Windows
interop, also test production Windows locking and replacement:

```
python3 tools/vulkan_cache/test_persistence.py --windows
```

## Scope

- Recipes: normalized identities, renderer selections, configuration changes,
  isolated attempts and exact expected/queued/published validation.
- Allocation: transactional growth, publication failure, duplicate ownership
  and cleanup.
- Jobs: bounded graphics/compute batches, cache probing, hit reuse and partial
  success after an aggregate driver failure.
- Persistence: directory merging, concurrent writers, locks, invalid data,
  I/O failures, atomic replacement, bounded quarantine and unchanged data.

These logical and failure-path tests do not replace driver testing. Test
renderer changes with Vulkan validation, representative gameplay and live
option changes. Android remains authoritative for timing; iOS and tvOS require
device testing.

Precompilation follows Beetle's renderer branches and shares runtime state
setters, program selection and identity generation. `plan_complete` covers the
enumerated identities, not every possible future renderer path. A later miss
is logged as a runtime escape and revokes completeness.

An incomplete plan retries once at the same blocking configuration boundary.
The retry queues only unpublished identities; a second failure is remembered
to prevent repeated gameplay hitches.

When supported, `VK_EXT_pipeline_creation_cache_control` probes a populated
cache before compilation. Hits are retained, errors fall back safely and only
misses compile. Both phases use small batches.

Persistent files are keyed by Vulkan vendor, device and cache UUID and include
a wrapper version. SYSTEM and SAVE caches are imported. Writers lock and merge
the current destination before atomic replacement. VFS-only paths remain
readable, but safe writes require native locking and replacement support.

Gameplay does not serialize the cache. Successful precompilation saves at its
configuration boundary; clean teardown saves runtime discoveries. Each cache
location keeps at most one `.rejected` payload. Test-era unkeyed files are not
migrated.

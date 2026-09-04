# Vulkan pipeline regression tests

These tests extract the implementation under test from `rhi/rhi_lib_vulkan.c`.
The C fixtures supply deterministic Vulkan/allocator/file-system doubles, not
copies of production algorithms. They need no game data or graphics device.

From the repository root on a POSIX host:

```
python3 tools/vulkan/test_recipes.py
python3 tools/vulkan/test_allocation.py
python3 tools/vulkan_cache/test_persistence.py
python3 tools/vulkan_cache/test_persistence.py --diagnostics
```

The allocation tests run release and diagnostic builds with ASan/UBSan. The
other tests require Python 3 and a C compiler. Under WSL with MinGW and Windows
interop, also test the actual Windows locking and replacement implementation:

```
python3 tools/vulkan_cache/test_persistence.py --windows
python3 tools/vulkan_cache/test_persistence.py --windows --diagnostics
```

## Scope

- Recipes: effective shader constants, fixed/vertex/pass identity, shared
  primitive/scanout selection, HDR Display VRAM, configuration transitions,
  and exact expected/queued/published recipe validation.
- Allocation: transactional pool/map growth, failed publication, duplicate
  handle ownership, and cleanup.
- Persistence: both-location merge, stale concurrent writers, process locks,
  corrupt/incompatible files, allocation and I/O failures, checked close,
  atomic replacement, bounded rejection quarantine, and unchanged-data
  suppression.

These are logical/failure-path tests, not a substitute for real-driver execution.
After renderer changes, run a core with Vulkan validation and representative
gameplay, including live option changes. Android remains authoritative for
compilation and persistence-save costs. iOS/tvOS
need their own device testing.

## Development diagnostics

Build with `EXTRA_FLAGS=-DBEETLE_VULKAN_PIPELINE_DIAGNOSTICS` (or the equivalent
compiler define). This enables first-use identity/escape logs, actual worker
counts and timing, and persistence-save timing. Normal builds omit those
diagnostic fields and per-job/per-use clocks/logs.

The precompile traversal is derived from Beetle's renderer state branches.
Runtime and capture share state setters, program selectors, and normalized
identity generation. Validation checks every enumerated exact identity before
and after publication, including identities already present in memory. Its
summary therefore reports `plan_complete`, not an unqualified claim that every
future runtime branch was anticipated. Any later first-use identity absent
after a successful plan is reported as a runtime escape and revokes that
plan's completeness. New paths still need regression cases and representative
runtime coverage.

An incomplete plan receives one bounded retry at the same already-blocking
configuration boundary. The second pass queues only identities that were not
published by the first. A second failure is remembered so it cannot become a
blocking retry on every gameplay frame; observed runtime misses remain logged.

Persistent files are named by Vulkan vendor/device/cache UUID and contain an
embedded wrapper version. Ordinary shader changes do not require a wrapper
version bump. Both SYSTEM and SAVE are imported, and a writer locks and merges
the current destination before replacement. Native Windows/POSIX paths support
safe writes; VFS-only paths remain readable but cannot be written safely without
locking/atomic-replace support, so the core tries the other directory.

No cache serialization or filesystem write is initiated from the per-frame
gameplay path. Successful precompilation saves immediately at the existing
configuration-change boundary, and clean Vulkan teardown saves runtime-learned
pipelines. Each cache location retains at most one driver-rejected payload at
the stable `.rejected` sibling path. Existing unkeyed test-era files are left
untouched and are not migrated.

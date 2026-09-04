# Vulkan pipeline cache tests

The persistence test extracts production cache and atomic-write code and uses
deterministic Vulkan and filesystem doubles. It needs no game data or GPU.

From the repository root:

```
python3 tools/vulkan_cache/test_persistence.py
```

Under WSL with MinGW and Windows interop, also test production Windows locking
and replacement:

```
python3 tools/vulkan_cache/test_persistence.py --windows
```

Coverage includes SYSTEM and SAVE merging, concurrent writers, process locks,
invalid data, I/O failures, atomic replacement, bounded quarantine and
unchanged-data suppression. Real devices remain necessary for driver behavior
and performance testing.

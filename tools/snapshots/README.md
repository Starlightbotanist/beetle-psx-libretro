# Runtime snapshot tests

The headless host loads a real core through libretro APIs. Its original PS-X
executable and reset stub need no game or console BIOS. It tests software
output; hardware callbacks are rejected rather than counted as passing images.

```sh
make -j2 HAVE_HW=0 HAVE_LIGHTREC=0
make -C tools/snapshots check CORE=/absolute/path/to/core.so
make -C tools/snapshots check CORE=/absolute/path/to/core.so SCALE=4
```

Native Windows uses MinGW GCC and native Make with its MSYS recipe shell:

```powershell
& $make -C tools/snapshots check SHELL=C:/msys64/usr/bin/sh.exe `
  CC=C:/msys64/mingw64/bin/gcc.exe CORE=C:/path/to/core.dll
```

Linux links `-ldl`; Windows uses `LoadLibrary`. This revision is tested on native
Windows. Use an isolated writable directory. Generated files are reused only
when their bytes match.

## Checks

The guest reads changing offscreen VRAM before replacing it, publishes complete
double-buffered records, and blends a triangle into retained pixels. Forward
and restored continuations must agree and make progress. Redraws or constant
patterns cannot conceal an omitted restore.

Beetle mode compares ordinary state bytes and normalized RGB, repeated loads,
independent owners, reset, invalidation and rejected-load recovery. Native runs
also test 1x -> 2x -> 1x; append `--fixed-scale` to exclude those transitions.
`gl_errors_test` extracts the shipping helper to test inherited errors, bounded
draining, sticky loss, reset notification and failed-state memory barriers.

Validate the assertions with deliberately broken restore code:

```sh
python3 tools/snapshots/check_mutants.py --output /new/path/mutants
```

Use `--make`, repeated `--make-arg=KEY=VALUE`, and `--jobs` for the toolchain.
Generated objects/sources stay outside normal build paths. The control passes
at 1x/4x; omitted software VRAM fails at both. Omitted enhanced pages fail at
4x and pass at fixed 1x without reaching the omitted operation. The checker
copies host metadata as well as bytes and verifies the copied mode, executable
access and content before invoking it; this preserves POSIX executable bits.

## Shared core host

```text
snapshot_smoke CORE NEW_DIRECTORY PROFILE CONTENT [SYSTEM_DIRECTORY]
snapshot_smoke --generate-psx DIRECTORY
```

Profiles contain unquoted `key=value` lines and `#` comments. `@rsp=1` also
checks the N64 fixture's scalar/vector/flag consumers. `@presented=1` reads the
last software frame after a restore, checking presentation ownership. Supplied core options
must be queried. Shared mode compares full RAM, actual GPU/RSP reads and RGB;
it does not assume ordinary serialization is an observational or universal
oracle. It tests 32 retained steps, speculative depths 1/2/3/12, repeated loads,
empty-state rejection and independent release.

The shared implementation lives in RetroArch's
`libretro-common/samples/snapshots`. This core fetches pinned sources only for
tests; fixtures/profiles remain here. The shared `run_profiles.py` isolates
runs and records hashes. These tests do not certify
hardware rendering, frontend scheduling, every private field, device loss/reset
or performance.

Temporal upscaling also retains core geometry, jitter and pending resets.
Frontend presentation images and timing stay live. `tools/upscaler/run.sh`
checks copied history against uninterrupted geometry, including unfinished
drawing areas and repeated restores. It does not exercise vendor runtimes.

## Test harness dependency

Only these explicit test targets fetch sources from RetroArch. Normal core
builds and gameplay do not download or use the test harness. `harness.lock.json`
pins a full commit and SHA-256 for each source file, including the matching
libretro header. The first `make harness`, `check` or `check-gpu` downloads
the sources into a shared user cache; later runs verify and reuse them.
The cache is under `%LOCALAPPDATA%/libretro/snapshot-tests` on Windows, or
`$XDG_CACHE_HOME/libretro/snapshot-tests` (`~/.cache` by default) elsewhere. Only targets supported by this core are available.

Use `OFFLINE=1` to require cached sources, or set
`TOOLS=/path/to/RetroArch/libretro-common/samples/snapshots` for local
development or an offline source checkout. `HARNESS_CACHE` selects another
cache directory. A local override intentionally bypasses the pin and fetching.
Updating the harness requires changing the revision and checksums together;
branch heads and tags are never used as dependency versions.

Beetle PSX HW - broad PGXP memory-state diagnostic v3
Base: 12bdb844261f3d8f43d470f569eb3ace0dcad049

PURPOSE

This keeps the broad Memory Only counters and separates CPU loads into three
PGXP shadow-memory states: unsupported address, supported but never written,
and supported and previously written. Invalid results are counted by state and
opcode. Native vertex fallbacks are also split into invalid-XY and command-word
mismatch causes. It is diagnostic only: it does not change PGXP validity,
address conversion, or rendering decisions.

INSTALLATION

The ZIP is a repository-root overlay with no wrapper directory. If the tree
still contains the previous broad diagnostic, simply overwrite it:

  cd ~/beetle-psx-libretro
  unzip -o ~/Downloads/beetle_pgxp_broad_memory_state_diag_v3.zip
  bash ~/beetle-local-tools/build_and_deploy.sh

If the tree is uncertain, preserve anything important first, then reset it.
These commands discard all local tracked and untracked changes:

  cd ~/beetle-psx-libretro
  git fetch origin
  git reset --hard origin/master
  git clean -fd
  unzip -o ~/Downloads/beetle_pgxp_broad_memory_state_diag_v3.zip
  bash ~/beetle-local-tools/build_and_deploy.sh

TEST A - BROAD GAME SURVEY

Use the same controlled baseline for each game:

  CPU Backend: Disabled (Beetle Interpreter)
  PGXP Operation Mode: Memory Only
  PGXP Vertex Cache: Off

Run a representative section of each game. Please include GT2 and any game
whose visual severity seems larger than its previous summary counters imply.
One combined log is fine; each core launch records its options.

TEST B - BOOT-LOGO CPU PROVENANCE

Enable the PlayStation boot logo and use Memory + CPU. Cold-launch the same
game once with each backend:

  1. Disabled (Beetle Interpreter)
  2. Max Performance
  3. Lightrec Interpreter

Let each launch continue through the complete logo and roughly ten seconds
beyond it. Report whether corruption is persistent, temporary, or absent.

LOG TAGS

  [pgxp_frame]         existing 60-frame broad summary
  [pgxp_cpu_boot]      first 360 CPU-mode frames, now including register masks
  [pgxp_load]          first 96 unsupported CPU loads in a core session
  [pgxp_load_summary]  60-frame load/opcode/address-region totals
  [pgxp_load_state]    unsupported/never-written/written load totals
  [pgxp_load_state_op] invalid results by memory state and load opcode
  [pgxp_cpu_change]    bounded JIT/Lightrec register-validity transitions
  [pgxp_load_page]     unsupported address pages observed in that window

Memory states 0/1/2 mean unsupported/never-written/previously-written. The
seven opcode counters are LB/LH/LWL/LW/LBU/LHU/LWR. Address-region counters
are BIOS/parallel-port/cache-control/other. Each memory state receives its own
bounded sample budget, while summaries continue for the full session.

The pgxp_frame reject=a/b/c fields count native fallbacks with invalid XY,
value mismatch, and both conditions. Because both causes can apply to one
vertex, a and b are not intended to sum to the native total.

EXPECTED OVERHEAD

This build performs extra address lookups, hashing, counters, and logging.
Do not compare its performance with a release build. Memory Only avoids the
per-frame CPU boot stream, but still retains broad event instrumentation.

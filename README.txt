Beetle PSX HW - PGXP vertex-fallback provenance diagnostic v4
Base: 12bdb844261f3d8f43d470f569eb3ace0dcad049

PURPOSE

This records bounded command-buffer provenance when rendering falls back to a
native vertex, including the command-buffer slot, GP0 word, shadow word, and
validity metadata. Existing broad Memory Only counters remain available. This
build is diagnostic only: it does not change PGXP validity, address conversion,
or rendering decisions.

TEST

Build the currently checked-out branch with the normal local script. Use this
controlled baseline:

  CPU Backend: Disabled (Beetle Interpreter)
  PGXP Operation Mode: Memory Only
  PGXP Vertex Cache: Off

Load directly into one controlled gameplay section where PGXP geometry defects
are visible, preferably in Spyro 3 or Gran Turismo 2. The first 96 native
fallbacks in the core session are logged, so avoid spending that sample budget
in menus. One representative run is enough for the first pass.

LOG TAGS

  [pgxp_vertex_native] bounded native-fallback command-buffer provenance
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

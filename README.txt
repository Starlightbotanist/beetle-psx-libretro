Beetle PSX HW - PGXP unique-fallback byte-store diagnostic v9
Base: 12bdb844261f3d8f43d470f569eb3ace0dcad049

PURPOSE

This records the most recent byte store behind each bounded native vertex
fallback, including its address, value, frame, and the PGXP shadow state before
the store invalidated it. A collision-free RAM-word table retains every CPU
byte-store record. Duplicate source-address/command-word pairs are suppressed
so repeating 2D geometry cannot exhaust the bounded sample budget. Each record
distinguishes an address hit from an exact invalidation-tag match. FIFO and
command-buffer provenance remains available.
Existing broad Memory Only counters remain available. This build is
diagnostic only: it does not change PGXP validity, address conversion,
or rendering decisions.

TEST

Build the currently checked-out branch with the normal local script. Use this
controlled baseline:

  CPU Backend: Disabled (Beetle Interpreter)
  PGXP Operation Mode: Memory Only
  PGXP Vertex Cache: Off

Run the same visible-defect section in Spyro 1, Spyro 3, and Gran Turismo 2.
The first 96 unique source-address/command-word native fallbacks in each core
session are logged. Load directly into the visible-defect gameplay section.

LOG TAGS

  [pgxp_vertex_native] bounded native-fallback command-buffer provenance
  [pgxp_vertex_source] FIFO source metadata for each bounded fallback
  [pgxp_vertex_store8] most recent byte store plus address/tag match state
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

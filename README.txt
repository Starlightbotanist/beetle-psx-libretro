Beetle PSX HW - PGXP MFC2/SLL5/SRA5 preservation test v13
Base: 12bdb844261f3d8f43d470f569eb3ace0dcad049

PURPOSE

This test preserves precise X/Y through a value-verified MFC2 to SLL 5 to
in-place SRA 5 sequence, matching the narrow case proven by v12. The behavior
change is compiled only under PGXP_DIAG and refuses invalid or mismatched source
shadows. Sixty-frame summaries
count every MFC2, SLL5 and in-place SRA5 candidate/match, successful
preservation, stage-2/stage-3 store,
and FIFO arrival to locate where incomplete chains stop. Bounded event logs now
record lineage-bearing FIFO words and every renderer vertex that consumes one,
regardless of whether it is tracked, cached, or native. Duplicate fallback pairs
remain suppressed, and the byte-store trace remains available.
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
  [pgxp_vertex_lineage] MFC2/SLL5/SRA5 ancestry of the source word
  [pgxp_lineage_summary]  60-frame counts at each lineage boundary
  [pgxp_lineage_fifo]     first 96 lineage-bearing GPU FIFO words
  [pgxp_lineage_vertex]   first 96 renderer vertices consuming lineage
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

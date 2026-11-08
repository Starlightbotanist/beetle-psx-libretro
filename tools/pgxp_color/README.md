# PGXP arithmetic and colour checks

Five offline suites link the real PGXP sources. They need no GPU, BIOS or
PlayStation content:

    make -C tools/pgxp_color check

The Makefile enables `PGXP_DIAG=1` because the range suite reads diagnostic
counters. These counters are absent from the default release build. Compiler
and linker options can be supplied through `CC` and `CFLAGS`.

## Oracle (`oracle.c`)

The oracle exercises the shipping `PGXP_GetColor()` against an independent
replica of the GTE's `Lm_C(MACn >> 4)` saturation. An accepted precise colour
must requantize to the three architectural RGB bytes. The GP0 command byte
does not participate in that comparison.

The suite exhaustively sweeps each channel's MAC values from -256 through
4351, including clamp boundaries and the sixteen fractional positions per
byte. It also checks saturating tails, integer limits, three million
deterministic random triples, half a million altered colour words and command
byte independence. The random checks do not exhaust the full input domain.

Requantization alone cannot establish ownership: a later write can produce
the same native word. Colour-domain flags and invalidation must also remain
correct; the transport, precision and isolation suites exercise those rules.

## Transport (`transport.c`)

Transport follows GTE colour production through register hooks, CPU tracking,
memory and the FIFO-to-command-buffer copy. Its eight cases cover:

* Direct `swc2` delivery and `mfc2` followed by command-byte OR and `sw`.
* Gouraud packet offsets and colour FIFO ordering.
* Refusal after an untracked CPU colour replacement.
* NCLIP magnitude limits.
* Architectural-zero ADD identity and the right-zero ADDU identity in
  Memory Only mode.

Colour payloads carry `PGXP_COLOR` rather than being interpreted as packed
vertex coordinates. Supported identity operations preserve that role; an
operation that changes the RGB payload invalidates it. Adding a command byte
can therefore preserve a colour, while arbitrary CPU colour arithmetic does
not acquire a precise GTE colour merely by producing the same bytes.

## Range (`range.c`)

Range checks diagnostic counts for accepted over-white colours. It verifies
the 255 boundary, brightness buckets, peak value and exclusion of refused
shadows. These synthetic counts describe the fixtures, not a game's hit rate
or the frequency of over-white lighting in gameplay.

## Precision (`precision.c`)

Precision exercises the actual arithmetic handlers and GPU consumers. Cases
cover comparisons, HI/LO moves, division, fractional carries, shifts, numeric
limits and the zero register. Other cases cover GTE register aliases, mode
and texture changes, colour/vertex separation, Memory Only colour handling,
RAM mirrors and holes, DMA invalidation, partial-word operations and byte
store retirement.

The tests deliberately pair native words with distinct precise payloads so
that equal native values cannot conceal a stale owner. They include nearby
negative cases and check what `PGXP_GetVertex()` or `PGXP_GetColor()` receives
where applicable.

## Isolation (`isolation.c`)

Isolation is built with `HAVE_LIGHTREC=1`. It exercises PGXP's temporary
isolated-RAM shadow through entry, mutation, restoration, invalidation and
reset. Register and command-buffer copies already consumed from that window
must survive restoration; writes outside the restored window retain their
own ownership. This does not test Lightrec's separate native-RAM cache model.

## Scope and additional validation

These suites do not run a complete CPU backend, execute emitted recompiler
blocks, render a frame or establish original-hardware accuracy. They do not
measure runtime, startup, memory use or real-content hit rates. Full-core
tests must additionally cover pending loads and branches, DMA, save/load and
reset transitions, renderer settings, native VRAM and displayed output.

Run the suites with the supported production compilers and optimization
settings. Sanitizer builds complement those runs; they do not establish
renderer or gameplay correctness. Report the exact revision, flags and
platform with results rather than carrying old configuration claims forward.

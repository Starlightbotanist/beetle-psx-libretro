# GPU UV geometry and pending-command checks

These tests link the shipping `mednafen/psx/gpu_polygon_sub.c` helper, with
only renderer-selection globals stubbed. They require no game, BIOS, frontend,
GPU, Python package or network access. They are maintained regression tests;
the original local capture hosts and diagnostic traces are not release code.

From the repository root:

```sh
make -C tools/gpu_uv check
```

The standard-library Python runner also builds in a temporary directory. On
Windows it defaults to native MinGW GCC and native Git for Windows; explicit
compiler and Git paths can be supplied. On other platforms it uses `cc` and
`git` from the environment. It never fetches revisions or starts an emulator.

```powershell
python tools/gpu_uv/check.py --cc 'C:\msys64\mingw64\bin\gcc.exe'
```

The standalone assertions cover uniform-depth and oblique geometry, flips,
eight strip orders, texture windows, native/enhanced renderer settings, culled
and degenerate halves, all four varying-depth positions, and valid collapsed-UV
and one-pixel fallback artwork. Uniform W=1 inputs explicitly exercise the
PGXP-off geometry/UV policy. This cannot establish a semantic distinction
between a sprite and a billboard with identical projected input.

## Reproducing the previously reported counts

The 12,736-case result is a historical **differential** comparison, not a claim
that every case independently establishes hardware accuracy. The C program's
`--matrix` mode preserves that exact input order and result format. The depth
fixture's `--depth-matrix` mode emits its 160 cases. The Python runner compiles
both the baseline helper and the current helper and checks their actual output.
The requested baseline revisions must already exist locally; a shallow clone
may need its history extended by its owner first.

```powershell
python tools/gpu_uv/check.py --matrix-base eaca6eadd55fb95025f1ecfc225630f10b70cab2 --depth-base 2cf77727cc49e7eedde3484a41f7c4bb9c170e17
```

The matrix compares the shoulder-only build with the quad classifier: 8,640
negative cases remain unchanged, 3,840 oblique/depth cases change, and 256
normal-quad cases reconstruct correctly despite stale derived fields. The
160-case comparison shows the reviewed depth gap: 16 failures become zero,
with the 32 uniform-depth controls unchanged. These are finite input domains,
not exhaustive geometry or savestate tests. The standalone suite includes
additional assertions and reports its own count separately.

The older baseline is `eaca6eadd55fb95025f1ecfc225630f10b70cab2` `OpenGL polygon texture-center sampling` (child of `ba33faea2c589d7381d7cd141db9228fd360333c`)(removes the MGS shoulder wedges in the controlled enhanced-OpenGL replay).
The depth baseline is `2cf77727cc49e7eedde3484a41f7c4bb9c170e17` `Reject oblique hardware quads from sprite UV correction` (child of `eaca6eadd55fb95025f1ecfc225630f10b70cab2`)(removes the traced large-surface correction jumps in the MGS command-area replay).

## Original renderer and savestate fixture

Generate a PS-X executable containing only procedural artwork and original
test instructions:

```powershell
python tools/gpu_uv/generate_fixture.py path/to/flipped-uv.exe
python tools/gpu_uv/generate_fixture.py path/to/pending-uv.exe --pending-only
python tools/gpu_uv/generate_fixture.py path/to/pending-one-pixel.exe --pending-only --one-pixel
python tools/gpu_uv/generate_fixture.py path/to/pending-control.exe --pending-only --complete-uv
```

The gallery has four flipped-UV triangle orientations, with and without a
texture window, plus a genuine rectangle control. Triangles are submitted
with their original UVs. Native RGB5 comparisons remove the different display
byte expansion used by software and hardware renderers. Enhanced-resolution
differences between renderers are not automatically errors.

The pending variants stop between the third and fourth vertices of a textured
quad. RAM word `0x800f0000` is 1 during the delay and 2 after completion. Save
while it is 1, let the program complete (including the cache-clearing draw),
then reload that state and let it complete again. Compare with uninterrupted
execution at the same emulated point. A full-UV second triangle is the nearby
negative control; the collapsed-UV and one-pixel variants need the saved first
triangle's correction fields. The delay is deterministic but frontend frame
counts may depend on startup conditions; verify the marker/pending command.

The one-pixel rule can already have changed the saved first vertex's V. Its
original first-triangle UV correction is therefore part of the pending command
state and must be serialized; blindly rebuilding it can change valid artwork.
New states store both offsets and the possible-2D flag. Older states omit them
and load with neutral defaults; those missing original corrections cannot be
recovered universally, particularly for the one-pixel variant. Ordinary
completed-quad states do not need that pending cache. New fields are appended
to the GPU section so older positional fast-state entries retain their order.

Geometry assertions do not exercise the actual state serializer. The renderer
and save/restore fixture requires a built core and a frontend; record platform,
API, Internal GPU Resolution, PGXP, filtering and reference capture separately.

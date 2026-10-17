# GPU UV geometry and pending-command checks

The geometry tests link the shipping `mednafen/psx/gpu_polygon_sub.c` helper.
The serializer test links the shipping `gpu.c`, `state.c` and `compat_strl.c`,
with device/renderer notifications stubbed. They require no game, BIOS,
frontend, GPU, Python package or network access. They are maintained regression
tests; the original local capture hosts and diagnostic traces are not release code.

From the repository root:

```sh
make -C tools/gpu_uv check
```

The standard-library Python runner also builds in a temporary directory. On
Windows it defaults to native MinGW GCC and native Git for Windows; explicit
compiler and Git paths can be supplied. On other platforms it uses `cc` and
`git` from the environment. It never fetches revisions or starts an emulator.
The serializer target uses GCC-compatible LTO and GNU linker section collection
to discard unrelated GPU execution paths. On MinGW it also disables automatic
exports. Geometry compiles as GNU89; the existing `gpu.c` requires GNU99.
The serializer build uses the core's existing warning exceptions for those
production sources. Its unused outer file-header writer gets a dummy version
definition; the test exercises GPU sections directly.

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

## Automated GPU serializer test

Both commands above also run `serialization.c` against the actual shipping
`GPU_StateAction` and `MDFNSS_StateAction`, including the real GPU state table.
The test removes the last three entries from a freshly written GPU section to
make an older chunk; all preceding bytes remain unchanged. It verifies the
entry names/sizes and the last older field, so it tests appending order as well
as neutral defaults without duplicating a state table.

The finite domain is both `FastSaveStates` modes, idle/pending quad commands,
and all eight valid combinations of the two offsets and possible-2D flag.
For each input it checks a current round trip, a chunk without the three new
entries after a successful restore, and a truncated final entry. Missing fields
must default to zero; a failed read must not commit the three new locals. Idle
states must serialize neutral corrections. VRAM, a saved W value and the last
older field are controls. An actual `GPU_Power` call also checks that the shared
soft-reset path clears the command and corrections. This gives 97 cases and
820 assertions. GPU transfers, emulated command execution and whole-core file
headers are outside this CPU-only codec test; the renderer fixture remains a
separate integration test.

## Mixed-fixture renderer checks

Generate the exact original mixed fixture:

```powershell
python tools/gpu_uv/generate_mixed_fixture.py path/to/mixed-uv.exe
```

The executable SHA-256 is
`ad5ad3d60ec4ef163c012968504b20873c87ee072cabb8b3f912537e3aac56fb`.
It issues 512 textured primitives: 128 sprites and 384 triangles, with UV
flips, texture windows, multiple blend modes, raw/modulated textures and
Gouraud shading. Earlier local notes called it a "512-triangle fixture";
"512-primitive mixed fixture" is the accurate name.

Observed on native Windows, NVIDIA RTX 3050 Ti laptop GPU, driver 546.30,
OpenGL Core 3.3 versus Vulkan: PGXP disabled, nearest filtering,
Texture UV Offset enabled, 16bpp(native), SDR, native dithering,
Multi-Sampled Anti Aliasing 1x, Software Framebuffer disabled and
Supersampling (Downsample to Native Resolution) disabled.

The historical **Reviewed combined** build is `99319114891fc998fef3e69e889f19f12b6ce534` `Validate all quad depths in sprite fallback` (child of `2cf77727cc49e7eedde3484a41f7c4bb9c170e17`)(varying-depth fixture failures fall from 16 to zero; the mixed-fixture enhanced difference remains).
The **State follow-up** build is `64b52d59441a2e38f9e00169527e5ef493a20d0d` `Preserve pending quad UV corrections` (child of `99319114891fc998fef3e69e889f19f12b6ce534`)(delayed collapsed/one-pixel restore differences fall from 28,672/768 to zero; normal drawing is unchanged).
The table was repeated on State follow-up, core SHA-256
`312cd0042f529c3dc2b42b5aa103544d0e3d0347cac3a8de7d5b8bc3e0a9ea46`,
and agrees with the historical Reviewed combined captures.

The enhanced difference had two independent causes. OpenGL clamped sprite
UVs to 255 before applying the texture window, preventing byte wrapping at
the page edge (92,736 differing pixels). Vulkan retained fractional shading
bytes for ordinary textures at enhanced resolutions, while OpenGL and the
software modulation operation truncate those operands (8,314 pixels).
The fixes clamp GL UVs before byte wrapping and use Vulkan's existing byte
conversion to truncate ordinary RGB5 shading before modulation. Raw, filtered, HD and
HDR source policies retain their existing modulation paths.

Displayed capture at frame 120; **Before** is State follow-up above and
**Both fixes** is the local tested runtime, core SHA-256
`a981da60459a8e4dfb37470d451677514c392f09cd549912a97286d57a5c0472`.
Its source is the State follow-up plus the comment/test follow-ups and the
two renderer fixes in this branch. No unrelated diagnostic shader is used.

The ordered implementation is
`6382d66f71d8b4dbfdac57db2f7fea5713e6b580` `Wrap OpenGL sprite UVs after primitive bounds` (child of `5d6d04a1d72084dbc947dd2a5bbcd107ccc0d813`)(92,736 sprite-tile differences against repeated native software become zero), then
`f03caae555553cd1e00ecf666f84bee930edea02` `Truncate Vulkan RGB5 shading bytes` (child of `6382d66f71d8b4dbfdac57db2f7fea5713e6b580`)(the remaining 8,314 mixed-fixture GL/Vulkan differences become zero).

| Fixture / settings | Comparison | Before | Both fixes |
|---|---|---|---|
| Original, 1x | GL versus Vulkan | 0 / 71,680 | 0 / 71,680 |
| Original, 1x | GL versus software | 0 / 71,680 | 0 / 71,680 |
| Original, 2x | GL versus Vulkan | Unmeasured | 0 / 286,720 |
| Original, 4x | GL versus Vulkan | 101,050 / 1,146,880 | 0 / 1,146,880 |
| Original, 8x | GL versus Vulkan | Unmeasured | 0 / 4,587,520 |
| 4-bit indexed, 4x | GL versus Vulkan | 86,589 / 1,146,880 | 0 / 1,146,880 |
| 8-bit indexed, 4x | GL versus Vulkan | 84,121 / 1,146,880 | 0 / 1,146,880 |
| GPU-written texture, 4x | GL versus Vulkan | 92,736 / 1,146,880 | 0 / 1,146,880 |

Generate the additional maintained inputs with `--texture-depth 4`,
`--texture-depth 8`, `--feedback`, or `--coverage`. The default output remains
byte-identical to the original executable. Coverage uses opaque white
artwork with the same positions. GL and Vulkan cover the same 456,260 pixels
at 4x before and after; the original fixture's differences lie on common
coverage. The nearest 4x sprite output also matches nearest-repeated native
software in all 286,720 pixels of the 70 visible sprite tiles after the GL
fix. These finite fixture results do not establish original-hardware or
whole-game accuracy.

The display-byte normalization is `round(byte * 31 / 255)` for hardware and
`byte // 8` for software. These are displayed pixel differences, not native
VRAM readback or universal incorrect-pixel counts. The 4x remainder predates
all MGS changes: the color base has 277,757 differences, the shoulder fix
reduces that to 101,050, and subsequent MGS commits retain the same result.

Remaining cross-renderer differences are explicitly outside that nearest
result. At 4x, bilinear has 125,621 differing displayed RGB-byte pixels and
xBR has 34,814, out of 1,146,880. Vulkan's filtered captures are unchanged by
the modulation fix; GL's 507,904 pixels of page-interior filter controls are
unchanged by the wrapping fix. In 32bpp, one pre-existing pixel differs by one
red byte (GL 35 versus Vulkan 34, at x=1113, y=723); both values are unchanged
by these fixes. HDR/HD and other GPUs are not covered by these captures.

## PGXP-on integration check

The MGS opening command-area camera move was also captured using State
follow-up and the GPU/driver above, GL and Vulkan at 4x nearest, **PGXP Operation Mode** set
to **Memory Only**, **PGXP Perspective Correct Texturing** enabled and
**PGXP Vertex Cache** disabled. The same 180-frame replay was sampled every
15 frames. Actual saved decoder vertices at frame 15 had W values
476.07544, 416.46606 and 428.69336; this exercised nonuniform depth supplied
by PGXP. A save/restore at frame 80 matched uninterrupted execution in all
12 displayed captures for each API: five before restoration and seven after.
This is scene-specific integration evidence; it does not establish all PGXP
configurations, deliberate PGXP-on mid-quad interruption, or pending-command
restoration across renderers. Capture hosts remain local development tools.

# ioquake3 GL1/GL2 blocker roadmap and supporting vendor RE

Status: consolidated blocker list plus new hardware-free reverse engineering,
2026-09-24.  Nothing in this note was run on the VX900 target; no kernel or
Mesa source was changed.  It builds on the handoff
[`chrome9-handoff-2026-09-24.md`](chrome9-handoff-2026-09-24.md), the ioquake3
notes indexed in [`README.md`](README.md), and the parallel
[`chrome9-hw-tcl-re.md`](chrome9-hw-tcl-re.md) and
[`chrome9-throughput-re.md`](chrome9-throughput-re.md).

Evidence labels used below:

| label | meaning |
|---|---|
| **dynamic** | produced by calling vendor code on this host (native 32-bit `dlopen` of the Linux 5.75 DRI driver, or Wine for the Windows UMD); byte-exact and reproducible |
| **static** | disassembly or data-section extraction with addresses |
| **cross** | Linux 5.75 and Windows 2015 binaries agree independently |
| **manual** | VX900 *Chrome9 HD DX9 R100 Part II* (`hd3d.txt` extraction), printed page |
| **source** | ioquake3 `93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1` or current Mesa/kernel source |
| **estimate** | produced by a probe built from an older Mesa revision; an upper bound, not a current-HEAD fact |
| **inference** | reasoned interpretation; never a byte recipe on its own |

Binary identities: Linux `via_chrome9_dri.so`
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`;
Windows D3D UMD `VTGUModeDX32A.dll`
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`;
Windows OpenGL ICD `VTGOGL32A.dll`
`a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`.

## 1. Prioritized blocker list

The goal is a correct menu and first map (q3dm1) under both renderers.
Ranks are ordered by "what stops the first correct q3dm1 frame", not by
implementation size.

### GL2 (`cl_renderer opengl2`, default cvars)

A path decision comes first.  Mesa exposes `GL_ARB_framebuffer_object` only
when the driver sets `pipe_caps::mixed_framebuffer_sizes`
(`st_extensions.c:1056`); `chrome9_screen.c` does not set it (**source**).
ioquake3 enables its FBO path only with GL 3.0 or `GL_ARB_framebuffer_object`
(`tr_extensions.c:59-80`, **source**).  Therefore the current driver sends
GL2 down the renderer's legitimate no-FBO path: no depth prepass, no HDR
render target, no tonemap/auto-exposure, no sun or projected shadows
(`tr_backend.c:896,1465`, `tr_scene.c:486-492`).  Confirm this in the next
ioquake3 log (`...GL_ARB_framebuffer_object not found`).  Keeping FBOs off
for the first-map milestone removes ranks G8-G10 from the critical path
without faking any renderer output.

| rank | path | blocker | evidence state | confidence |
|---:|---|---|---|---|
| G1 | lightall, `LIGHTDEF_USE_LIGHTMAP` (world) | complete 5-sampler / 6-varying / ~80-96 ALU program has never produced a checked pixel on VX900 | host: mode-zero + culled-FACE object schedules to peak 9 temps and replays through the kernel compiler (`ioquake3-gl2-cull-specialized-face.md`); 5 sparse descriptors silicon-qualified (`3825c75`) | high that it is the next world gate |
| G2 | lightall, cull-none (two-sided) materials | FACE is dynamic; the constant-FACE specialization cannot be used | software two-sided marker designed (`chrome9-draw-twoside-face-sign.md`); **new**: native VS back-face-color producer and cross-driver PS face-sign oracle, section 2 | high (design), unqualified on silicon |
| G3 | lightall, `LIGHTDEF_USE_LIGHT_VECTOR` (entities/models) | not yet compiled by current Mesa HEAD; needs 7 varyings, 3 samplers, `b2f32(flt(0, var_LightDir.w))`, one non-interpolated `load_input` | section 6, **estimate** probe | medium |
| G4 | lightall, `LIGHTDEF_USE_LIGHT_VERTEX` (vertex-lit world) | 85 ALU + 3 TEX, pre-scheduler peak 13 live temps (> 12) | section 6, **estimate** | medium |
| G5 | lightall/generic alpha modes 1 and 2 | `alpha == 0` and `alpha >= 0.5` KILL lowerings; mode 3 is silicon-qualified | `ioquake3-gl2-alpha-test-boundary.md`, `ioquake3-gl2-product-alpha-kill.md` | high |
| G6 | GL2 bounded run length | runs so far used `CHROME9_DRAW_LIMIT=1`; the first uncapped frame and its next rejection are unknown | `ioquake3-gl2-product-alpha-kill.md` | high |
| G7 | fogpass, dlight, texturecolor, generic (menu) | programs designed or compiled; no GL2 menu pixel oracle recorded | `ioquake3-gl2-fogpass-native-design.md`, `ioquake3-gl2-generic-texturecolor.md` | medium |
| G8 | FBO path (only if `ARB_framebuffer_object` is exposed) | RGBA8 RTT + `DEPTH_COMPONENT24` renderbuffer + `glBlitFramebuffer`, depth prepass, tonemap/calclevels4x (`log2`, `exp2`, offset samples) | section 5 | high |
| G9 | sun shadows | require a `q3gl2_sun` shader keyword (`tr_shader.c:1802-1811`); retail q3dm1 does not trigger them; also need FBO | **source** | high |
| G10 | pshadow, SSAO, depth blur, cube maps, parallax | off by default: `cg_shadows 1` (pshadow needs 4), `r_ssao 0`, `r_cubeMapping 0`, `r_parallaxMapping 0`; SSAO/depth blur also need derivatives, which the hardware lacks | **source**, section 3 | high |
| G11 | HDR float targets | vendor has no RGBA16F target/texture (section 4); leave `ARB_texture_float` off, ioquake3 then uses RGBA8 | **static** | high |

### GL1 (`cl_renderer opengl1`)

| rank | path | blocker | evidence state |
|---:|---|---|---|
| L1 | sustained submission | kind-7 (one-texture blend batch) cookie timeout; classified as a genuine CR/parser-progress timeout, not yet localized | `ioquake-gl1-kind7-cookie-timeout.md` |
| L2 | uncapped first frame | q3dm1 reaches the 32,768-triangle safety cap with no shader/draw rejection, but no uncapped, camera-positioned pixel reference exists | `ioquake3-gl1-shipped-material-add-census.md`, `ioquake3-gl1-live-texture-census.md` |
| L3 | throughput | per-triangle submission cost dominates; the cap exists because full frames are slow | `chrome9-throughput-re.md`, `persistent-batched-submit-performance-plan.md` |
| L4 | other maps | texture-env `GL_ADD` (q3dm17 `metalfloor_wall_15ow`); not needed for q3dm1 | `ioquake3-gl1-shipped-material-add-census.md` |

GL1 needs no new vendor RE for q3dm1: the captured workload uses only
BGRX8/BGRA8 textures with mip chains (`ioquake3-gl1-live-texture-census.md`)
and already-qualified alpha/depth/blend tuples
(`ioquake3-alpha-depth-blend-cmd22-qualification.md`).  Its blockers are
submission robustness and performance.

## 2. FACE: native back-face color producer (new)

### 2.1 VS side: the compiler routes back colors to native outputs 36/37 (dynamic)

The Linux VS 2.0 compiler accepts D3D ATTROUT indices 2 and 3 and emits
distinct native output ids.  With the full parser, dispatcher and
`PVSCP_POSTPROCESS_H5` (`CHROME9_POSTPROCESS=1`), a single colour MOV from
`v1` produces:

| output token | native destination/control | object `+4` | object `+0xc` |
|---|---:|---:|---:|
| `oD0` `d00f0000` | `0008422f` (unified 34, SAT) | `0x44` | `0` |
| `oD1` `d00f0001` | `0008423f` (unified 35, SAT) | `0x84` | `0` |
| `oD2` `d00f0002` | `0008024f` (unified 36) | `0x44` | `0x40` |
| `oD3` `d00f0003` | `0008025f` (unified 37) | `0x84` | `0x80` |

All four together give `+4=0xc4`, `+0xc=0xc0`.  Writing `oD2` marks the
*front primary* linkage bit 6 in `+4` and a separate bit 6 in `+0xc`; `oD3`
does the same for the secondary colour.  The pairing is exact evidence that
index 2/3 are the back-face counterparts of `oD0/oD1`.  Front colours carry
the SAT bit (14); back colours do not.  A constant source is legal:
`mov oD2, c0` emits `0040014c 00000000 00000000 0008024f`.  The eight rows are
in [`chrome9-face-bfc-oracle-corpus.tsv`](chrome9-face-bfc-oracle-corpus.tsv)
and replay with `tools/re/chrome9_face_bfc_oracle_check.py`.

This agrees with the two-sided fixed-function lighting result in
`chrome9-hw-tcl-re.md` (ATTROUT 2, `...0004424f`, `+0xc=0x40`).

### 2.2 GL front end maps `result.color.back.*` to ATTROUT 2/3 (static, cross)

The ARB vertex-program output translation tables are identical in both GL
drivers:

```text
Linux  icdvp_output_reg_type_to_stm_def @ 0x415160 (.data, size 0x3c)
Linux  icdvp_output_reg_to_stm_def      @ 0x415120 (.data, size 0x3c)
Windows VTGOGL32A.dll same bytes         @ 0x103996b4 / 0x10399678

index : 0  1  2  3  4  5  6  7..14
type  : 4  5  5  5  5  4  4  6
reg   : 0  0  1  2  3  1  2  0..7
```

(D3D register types: 4 RASTOUT, 5 ATTROUT, 6 TEXCRDOUT.)  The fifteen
entries follow the `ARB_vertex_program` result binding order
position, front primary, front secondary, back primary, back secondary, fog,
point size, texcoord 0..7.  The table bytes are **static/cross**; attaching
the ARB names to indices 3/4 is **inference** from that order, strongly
supported by the compiler pairing in 2.1.

### 2.3 Activation and raster selection (static, cross, manual)

* `A61[19] = (object+0xc != 0)`.  Linux `stmUpdateVS_inv:0x17c610..0x17c620`
  (byte `0x2242` bit 3) and Windows `VTGUModeDX32A:0x1015acfb..0x1015ad06`
  (`setne; shl $0x13`) compute the same bit.  **Static/cross.**  The VX900
  manual does not document A60-A62; the bit's name is unknown.
* `A23[7]` selects which physical winding takes `oBFD#` (manual p.161).
  Linux `__glDpSTMInitializeCull:0x1ed9cb..0x1ed9e3,0x1eda98,0x1edad8` maps
  `glFrontFace(GL_CW)` to stm value 1 and `GL_CCW` to 2, and
  `STM_UpdateCull_INV:0x196ade..0x196af8` / `0x196b40..0x196b5b` writes
  `A23[7] = (value == 1)` in both the cull-enabled and cull-disabled branches.
  Bit 8 (cull winding) and A01 bit 7 (cull enable) are separate.
  **Static.**  Whether the hardware's CW/CCW naming matches Gallium's
  post-viewport winding must use the same formula as native culling
  (`native-cull-bias-lowering.md`) and needs the silicon ladder below.
* No FVF/output-mask bit for back colours exists in the VX900 A25/A26/A28/A29
  definitions (manual pp.161-166); the older HCM FVF attributes `31h/32h`
  (`chrome9-face-sign-bfc-producer.md`) are not the VX900 mechanism.  The
  producer is the VS output register plus A61[19] and A23[7].  **Manual +
  static; unqualified on silicon.**

### 2.4 PS side: exact face-sign program (dynamic, cross)

With front primary = white and back primary = black, the lightall sign
`!gl_FrontFacing ? n : -n` is `n * (1 - 2*p)`.  The PS 2.0 form

```text
def c0, -2, 1, 0, 0 ; dcl v0 ; dcl t0
mad r0, v0.x, c0.x, c0.y
mul oC0, t0, r0.x
```

compiles identically in both vendor compilers:

```text
Windows D3D (Wine, raw == final) and Linux pre-configuration-2:
  00802490 00080000 00f04000 00000020
  0140b6d0 30000000 00f0c005 00000820
Linux production configuration 2:
  00802490 00080000 00f04000 00000020
  0140b6d0 30200000 00f2c005 00000820
  A90..A9a 90058001 91000000 92000002 93000000 94000000 95000000 ...
  active resource counts 1 1 1 0
```

Input token SHA-256 `734b6714...b57597`, Windows final ALU SHA-256
`3a6040f0...52842e7`.  This is the pixel oracle for either producer.

### 2.5 Two producers, one PS contract

1. **Software (now).**  Chrome9 always runs Gallium `draw` (software VS,
   clipping, culling).  Draw's two-side stage selects COLOR/BCOLOR per final
   triangle; the marker design in `chrome9-draw-twoside-face-sign.md` is exact
   and needs no new hardware state.  Default lightall passes `var_Color` as a
   generic varying, so PRIMARY0 is free.
2. **Native (with hardware TCL).**  When the VS runs on the GPU, draw's
   two-side stage is gone; then emit `mov oD0, white` and `mov oD2, black`
   (constant source is legal, 2.1), set A61[19], disable hardware culling for
   that draw and program A23[7] from the post-viewport front winding.

Both feed the same 2.4 program.  The FACE lowering costs one extra MAD
relative to the synthetic-sign resource experiment (MUL with folded NEG),
i.e. +1 native ALU per program.

## 3. Fragment program capacity and flow control

### 3.1 No larger program store exists in any vendor path

| source | ALU | TEX | temps | constants | flow | label |
|---|---:|---:|---:|---:|---|---|
| Linux `INVPS_AddAInst`/`AddTInst` append limits (`0x19a290`, `0x19a1e0`) | 96 | 32 | 12 + 4 special (`stmPPSSetRegManager_invi`) | 32 (`ioquake3-gl2-fragment-constants.md`) | none | static (earlier notes) |
| Windows D3DCAPS9 template `0x1030fcc8` | `PS20Caps.NumInstructionSlots = 96` total | (in total) | `PS20Caps.NumTemps = 12` | PS 2.0 | `DynamicFlowControlDepth = 0`, `StaticFlowControlDepth = 0`, `PS20Caps.Caps = 0` | static (new) |
| same template | `MaxPShaderInstructionsExecuted = 96`, `MaxPixelShader30InstructionSlots = 0`, `PixelShaderVersion = ffff0200` | | | | no PS 3.0 | static (new) |
| Linux GL ARB_fragment_program limits | native instructions 64, ALU 32, TEX 32, indirections 32 | | 12 | params 32, local/env 24 | none | static (new, 3.2) |
| VX900 manual | A90-A9A are "Reserved" (type-01, p.120); only the upload sub-types `20h/21h/22h` are named (p.71) | | | | | manual |

`PS20Caps.Caps = 0` also means no arbitrary swizzle, no gradient
instructions, no predication, and the PS 2.0 dependent-read limit applies at
the D3D API level (Wine `d3d9caps.h` bit names).  The template is applied for
chip selector 5, the branch that tests PCI `7122`
(`0x100c7227..0x100c7231`, table `0x100c7430/0x100c7444`); the per-adapter
modifier `0x100c6f70` changes VS, TnL and device-capability fields but never
the PS fields.

Hardware encoding room is wider than the stores: A95-A9A provide six
24-bit ALU-to-TAU words (144 bits) and A93/A94 48 TAU bits, and A92 packs the
ALU count in its low half and the TAU count at bit 16.  No vendor compiler
writes A99/A9A, uploads beyond record 95, or offsets the `20030000` ALU
upload selector.  **Conclusion (static, high):** 96 ALU + 32 TAU records and
12 allocatable temps are the complete vendor-exposed PS capacity; there is no
program paging, second bank or larger PS 3.0 store to recover.  Capacity
beyond it must come from Mesa-side scheduling/packing or splitting work
across passes, not from an unknown register.

### 3.2 GL ARB_program limits in the Linux ICD (static)

`__glim_GetProgramivARB` (`0x2db700`) reads per-target pairs at
`ctx + 0x278 + 4*t` (t = 0 vertex, 1 fragment), `0x280`, `0x288`, `0x298`,
`0x2a0`, `0x2a8`, and the fragment-only `0x2b0/0x2b4/0x2b8`; it returns the
constants 128 for `MAX_PROGRAM_INSTRUCTIONS` (`0x2dba3d`) and 16 for the
attribute limits (`0x2db86e`, `0x2db9e6`).  The device-constant block from
`__glS3InvGetDeviceConstants` (`0x1d7840`) sits at `ctx + 0x1c0`: `GetIntegerv`
reads `GL_MAX_TEXTURE_SIZE` from `ctx+0x1e8` (`__glDoGet:0x2984d6`), which is
device constant `+0x28 = 2048`.  With that overlay the raw block in
`gl-feature-matrix.md` resolves to:

| query | vertex | fragment | device constant |
|---|---:|---:|---|
| `MAX_PROGRAM_NATIVE_INSTRUCTIONS` | 0 | 64 | `+b8/+bc` |
| `MAX_PROGRAM_(NATIVE_)TEMPORARIES` | 0 | 12 | `+c0/+c4` |
| `MAX_PROGRAM_(NATIVE_)PARAMETERS` | 0 | 32 | `+c8/+cc` |
| `MAX_PROGRAM_(NATIVE_)ADDRESS_REGISTERS` | 1 | 1 | `+d8/+dc` |
| `MAX_PROGRAM_LOCAL_PARAMETERS` | 96 | 24 | `+e0/+e4` |
| `MAX_PROGRAM_ENV_PARAMETERS` | 96 | 24 | `+e8/+ec` |
| `MAX_PROGRAM_(NATIVE_)ALU/TEX/TEX_INDIRECTIONS` | - | 32/32/32 | `+f0/+f4/+f8` |

The Windows block has one inserted dword; its only changed value is vertex
`MAX_PROGRAM_ENV_PARAMETERS` 96 -> 256, matching D3D's 256 VS constants.  The
vertex zeros in Linux are consistent with a software-emulated ARB vertex
program path (**inference**).  Correction: `GL_MAX_TEXTURE_UNITS` is read from
`ctx+0x1e4` = device constant `+0x24` (SI default 8, `__glInitConstantDefault`
`0x2815ef`), not from `+0x10` as the older table labels it; the value is 8
either way.

### 3.3 Dynamic loops and branches

No vendor pixel path supports them (**static**, section 3.1; PS 2.0 dispatcher
has no CALL/IF/LOOP entries per `via-5.75-shader-re.md`).  ioquake3 impact:

* lightall parallax loops exist only with `r_parallaxMapping` (default 0);
* depth-blur loops have constant bounds (unrollable) but also use
  `dFdx/dFdy`, which have no hardware implementation
  (`chrome9-dsx-dsy-negative-boundary.md`); SSAO likewise;
* runtime `u_AlphaTest` branches are already handled by uniform
  specialization (`04a35c3`);
* `float(x > 0.0)` in `LIGHT_VECTOR` must be a CMP-based select, not flow.

## 4. Formats, filtering, sRGB and float (static, new)

`tools/re/chrome9_umd_caps_dump.py` decodes the UMD's static tables with bit
names parsed from Wine's public `d3d9caps.h`/`d3dukmdt.h`.  Only the DDI
`FORMATOP_*` names are embedded by hand (they are absent from the local
headers); their assignment is internally consistent: depth formats carry only
`ZSTENCIL*`, bump formats only `BUMPMAP`, display modes only on R5G6B5 and
X8R8G8B8, and GetCaps clears exactly `0x40000` (`NOFILTER`) on L16 under an
adapter flag (`0x100b112c..0x100b1140`).

FORMATOP table `0x1030f2e0`, 43 x 20 bytes (GetCaps count `0x100b1006`,
copy `0x100b10ad`):

| class | formats | operations |
|---|---|---|
| 32-bit colour | A8R8G8B8, X8R8G8B8, A8B8G8R8, X8B8G8R8, A2R10G10B10, A2B10G10R10 | texture, volume, cube, render target, sRGB read, sRGB write, autogen mip |
| 16-bit colour | R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4 | same (R5G6B5/X8R8G8B8 also display modes) |
| luminance/alpha | A8L8 (renderable); L8, A8, A4L4 (texture only, `NOALPHABLEND`) | L16/G16R16: render target, `NOFILTER`, `NOALPHABLEND` |
| compressed | DXT1..DXT5 | texture, volume, cube, sRGB read |
| float | R16F, G16R16F, R32F | texture + render target, `NOFILTER`, `NOALPHABLEND` |
| depth | D16, D16_LOCKABLE, D32, D32_LOCKABLE | depth-stencil only, **not** texture |
| depth texture | DDI codes 74 (S8D24) and 76 (X8D24) | depth-stencil **and** texture |

There is no A16B16G16R16F or A32B32G32R32F entry.  ioquake3 GL2's HDR target
therefore has no vendor precedent; with `ARB_texture_float` withheld the
renderer uses `GL_RGBA8` (`tr_fbo.c:267-269`), which is the correct choice.

D3DCAPS9 template `0x1030fcc8` texture fields:

* `TextureFilterCaps` = MIN/MAG point, linear, anisotropic; MIP point and
  linear; `MaxAnisotropy = 16`; volume textures without anisotropy.
* `TextureCaps` includes `MIPMAP`, `MIPCUBEMAP`, `CUBEMAP`, `VOLUMEMAP`,
  `PROJECTED`, `POW2` with `NONPOW2CONDITIONAL` (NPOT only without mips and
  with clamp), `TEXREPEATNOTSCALEDBYSIZE`; `MaxTextureRepeat = 128`
  (already used by `signed-wrap-mipmap-nearest-contract.md`).
* Address modes wrap, mirror, clamp, border, mirror-once.
* `NumSimultaneousRTs = 4` and `PrimitiveMiscCaps` has
  `MRTPOSTPIXELSHADERBLENDING`, `INDEPENDENTWRITEMASKS`, `SEPARATEALPHABLEND`
  and `POSTBLENDSRGBCONVERT`; `Caps3` has `LINEAR_TO_SRGB_PRESENTATION`.  The
  manual's "Number of Render Target" field (A01[16:14], p.91) matches the
  MRT count.  GL ICDs still expose one draw buffer.

sRGB hardware detail (A30 bit 8 texture de-gamma with the five-word table;
A53 bits 6/5:4 destination de-gamma/gamma) is already closed statically in
`fixed-function-texture-sync.md` and `render-target-format-oracles.md`; the
FORMATOP and caps bits above are the D3D-level confirmation.  ioquake3 does
not use sRGB formats by default (only DDS-loaded images), so sRGB is not on
the q3dm1 path.

## 5. Render-to-texture needs if FBOs are later exposed

Only relevant after rank G8 is chosen.  ioquake3 then needs (**source**,
`tr_fbo.c`):

* `renderFbo`: `GL_RGBA8` colour (live-proven A52 format `99`) plus
  `GL_DEPTH_COMPONENT24` renderbuffer (X8Z24 R2T work:
  `chrome9-d24-r2t-transition-oracle.md`);
* `glBlitFramebuffer` (assumed present with `ARB_framebuffer_object`);
* quarter/scratch FBOs sampled by calclevels4x/tonemap/down4x/bokeh, i.e.
  render-then-sample cache invalidation (`resource-layout-clear-cache.md`);
* depth prepass into the same FBO.

Vendor evidence supports RGBA8 colour, D24 depth and D24 depth textures.
Mesa additionally needs `mixed_framebuffer_sizes`; that cap claims
attachments of different sizes and must be validated separately.

## 6. Lightall permutation estimates (estimate)

The retained host probe `/tmp/chrome9-lightall-resource-probe-exact`
(SHA-256 `4fbb372e...0be34`, built 10:49 before Mesa `cecf7f6`, so without
the committed fmad/NEG/SAT compaction and without the pressure scheduler) was
run with `SPECIALIZE=1` (alpha mode 0, constant FACE).  Sources were
preprocessed from the T2 ioquake3 tree exactly like the earlier default
source; the lightmap source is byte-identical to the recorded
`7913798a...c2d6`.

| permutation | varyings | samplers | plain | `COMPACT=1` | note |
|---|---:|---:|---|---|---|
| `USE_LIGHTMAP` + deluxe/normal/specular | 6 | 5 | 94 ALU + 5 TEX, peak 8 | 78 ALU + 5 TEX, peak 11 | fits; margin for the FACE MAD |
| `USE_LIGHT_VECTOR` + normal/specular | 7 | 3 | rejected before counting | rejected | `b2f32(flt(0, VAR6.w))`, `load_input` of VAR6, vec4 with one undefined lane |
| `USE_LIGHT_VERTEX` + normal/specular | 6 | 3 | rejected | 85 ALU + 3 TEX, peak 13 | old allocator fails at record 39; current scheduler not in this probe |

The lightmap row agrees with the committed 95 ALU + 5 TEX figure (which
carried a synthetic sign MUL).  Re-run all three against current Mesa HEAD
before treating any count as current.

## 7. Proposed next implementation steps

1. **Decide the GL2 FBO policy** for the first-map milestone: keep
   `mixed_framebuffer_sizes` off and confirm the no-FBO path in the next
   ioquake3 log.  Revisit only after the world renders.
2. **Lightall silicon oracle (G1).**  Run the complete scheduled mode-zero,
   culled-FACE lightmap object once with distinct per-sampler colours and CPU
   reference pixels, under the existing guarded cmd22 flow.
3. **FACE for cull-none (G2)**, software producer first: implement the
   draw two-side marker as specified, then run the section 2.4 program as the
   pixel oracle on adjacent opposite-winding triangles (front must give
   `-n`, back `+n`), including a clipped triangle and both `front_ccw` values.
   Keep the native route (2.5 step 2) for the hardware-TCL work; its first
   silicon test is `mov oD0,white; mov oD2,black` + A61[19] + toggling only
   A23[7], which must exchange the two colours without culling either
   triangle.
4. **Re-measure G3/G4 on HEAD** with a rebuilt probe; add a bounded CMP
   lowering for `b2f32(flt(0,x))` with the same finite-domain proof style as
   the existing selects, and admit the flat `load_input` only after its
   interpolation mode is proven constant per primitive.
5. **Alpha modes 1/2 (G5)** with guarded equality and exact-half boundary
   oracles, following the mode-3 pattern.
6. **Uncapped GL2 run (G6)** with the unsupported-NIR dump, recording the
   first new rejection; then GL2 menu and fog/dlight pixel checks (G7).
7. **GL1 (L1-L3):** execute the kind-7 discriminator table, then an uncapped
   q3dm1 frame against a software reference, then the throughput plan.
8. Do not spend RE time on PS flow control, a larger PS store, float/HDR
   targets or sRGB for this milestone: sections 3 and 4 close them as absent
   or off-path.

## 8. Corrections to earlier notes

* `signed-wrap-mipmap-nearest-contract.md` lists `0x1030fbf0` as a D3DCAPS9
  record.  The adapter constructor copies it as `0x35` dwords (`0xd4` bytes,
  the D3DCAPS8 size that GetCaps case 12 checks at `0x100b13d2`) and its
  shader version is PS 1.4; only `0x1030fcc8` is the 304-byte D3DCAPS9 image.
  `0x103151e0` holds pointer-like values in the PS 2.0 caps positions and is
  not a D3DCAPS9 either.  The texture fields quoted there are unaffected.
* `gl-feature-matrix.md`: the raw program block is named in section 3.2, and
  `GL_MAX_TEXTURE_UNITS` comes from device constant `+0x24`, not `+0x10`.
* `chrome9-hw-tcl-re.md` calls the A23[7] back-colour linkage unproven; it
  remains silicon-unproven, but section 2.3 now gives the vendor's exact
  front-face-to-A23[7] rule and the A61[19] consumer on both platforms.

## 9. New files and reproduction

| file | purpose |
|---|---|
| `tools/re/chrome9_umd_caps_dump.py` | hash-gated decoder of the UMD D3DCAPS9 template and FORMATOP table |
| `tools/re/chrome9_face_bfc_oracle_check.py` | replays the back-face-colour VS and face-sign PS corpus |
| `docs/research/chrome9-face-bfc-oracle-corpus.tsv` | eight byte-exact compiler rows |

```sh
# 32-bit stub DSOs for the legacy driver's libexpat/libdrm dependencies
gcc -m32 -shared -fPIC -Wl,-soname,libexpat.so.1 tools/re/chrome9_stub.c -o $L/libexpat.so.1
gcc -m32 -shared -fPIC -Wl,-soname,libdrm.so.2   tools/re/chrome9_stub.c -o $L/libdrm.so.2
gcc -m32 -O1 tools/re/chrome9_vs_compile_probe.c -ldl -o $W/vsc
gcc -m32 -O1 tools/re/chrome9_pps_probe.c -ldl -o $W/pps
python3 tools/re/chrome9_face_bfc_oracle_check.py \
  --vs-probe $W/vsc --pps-probe $W/pps --library-path $L
python3 tools/re/chrome9_umd_caps_dump.py
```

Run from the workspace root (the probes open `./binary/...`).  The Wine
face-sign check used `tools/wine_shader_probe.c` (reproducible PE SHA-256
`a87f9bf5...d94c`) in a private prefix whose `z:` drive points to a scratch
directory and whose `y:` points to `/`, so the probe's fixed `Z:\tmp` outputs
stay out of the shared `/tmp`.

Artifact hashes (host scratch, `~/tmp/re-gl2`): VS probe `4c300dad...dc3d`,
PS probe `fd891936...b264`, caps dump output `2558df6c...9203`, corpus TSV
`582e12cd...d1d`, light-vector source `7d0dfdcf...bc17`, light-vertex source
`447a2e2e...12b5`.

## 10. Current-source delta (2026-09-24, offline only)

This section records source-tree changes after the ranked audit above; it is
not a VX900 qualification or a claim about the currently loaded module.

* **Depth/stencil:** the current kernel tree now has a cmd22 stencil block:
  `chrome9_engine.c:731-785` validates and packs CW/CCW A23-A26,
  `:2259-2268` enables A01 bit 4 with the full block, and `:7513-7559`
  requires a bound X8Z24 depth resource.  Current Mesa
  `chrome9_draw.c:906-930,1307-1341,1554-1557` admits it only with an
  S8_UINT_Z24_UNORM attachment and mirrors the selected API face into both
  native face blocks.  `chrome9_context.c:5967-5971` now retains both stencil
  references.  This is uncommitted, uncompiled Mesa / untested-silicon work
  according to `chrome9-handoff-2026-09-24.md`; the previous command-17
  proposal in `ioquake-next-raster-state-ladder.md` is not the current ABI.
  The vendor D24S8/A23-A26 contract remains at Windows D3D
  `VTGUModeDX32A.dll:0x101fc08b..0x101fc1e6`, independently checked against
  Linux `STM_Stencil_INV:0x1955e0`.
* **Alpha:** fixed A00/A33 alpha test is already sourced by Windows D3D
  `VTGUModeDX32A.dll:0x101fb090` and Linux
  `STM_AlphaTest_INV:0x194c40` / `STM_AlphaFunc_INV:0x194ad0`; current
  Mesa `chrome9_draw.c:862-889` emits the three ioquake mode values for the
  legacy packed path.  Generic lightall mode three has a guarded
  product-alpha KILL silicon oracle (`ioquake3-gl2-product-alpha-kill.md`);
  modes one/equality and two/inclusive-half remain distinct unproved paths.
* **FACE:** draw-time cull-specialized fragment variants are now wired in
  `chrome9_context.c:5033-5092`, so the older G2 text overstates that
  integration gap.  Cull-none still needs a genuine per-primitive producer:
  vendor VS ATTROUT 2/3, Windows A61[19]
  `VTGUModeDX32A.dll:0x1015acfb..0x1015ad06`, and manual A23[7]
  support the back-face-colour route, but it has no VX900 pixel oracle.
* **Kind-7 cookie:** the captured timeout remains a missing parser-reachability
  cookie, not a proven bad H2 (`ioquake-gl1-kind7-cookie-timeout.md`).  Current
  Mesa `chrome9_draw.c:893-904,2824-2831` and kernel
  `chrome9_engine.c:4047-4067` now reject the shared one-texture
  MODULATE+blend state, including the one-triangle form.  This contains a
  GPU-wedge class on the *legacy* immediate-list path; if Mesa cannot select
  the separate generic cmd22 path, it can drop GL1 UI draws.  The old note's
  legacy one-triangle fallback is no longer an established safe workaround.
  Vendor Windows KMD
  `VTGKModeDX32A.sys:0x000b755f..0x000b85f4` supplies FE33/3D-wait
  completion grammar, while the current kernel still retires via H0 sync,
  DST_POS cookie and owned PAUSE (`chrome9_engine.c:3750-3799`).  First close
  the exact vendor packet/state difference offline, then test one guarded
  discriminator; do not remove the ABI rejection on an inferred cause.

## 11. Qualified cmd22 route around the legacy MODULATE+blend timeout

The vendor Linux configuration-two compiler's one-texture MODULATE object is
`A90..A93=90018000 91000001 92010001 93000001`, ALU
`0140b6d0 30040053 00f20005 00000820`, TAU `301001b0`
(`shader-upload-oracles.md:115-131`).  The current legacy kernel builder embeds
those same objects (`chrome9_engine.c:1040-1053`) and independently composes
the blend A00/A33/A34-A37/A53 block (`:1089-1149`).  The Windows D3D and GL
blend builders at `VTGUModeDX32A.dll:0x101fd9e0` and
`VTGOGL32A.dll:0x10251220` establish the state fields, but no retained vendor
capture proves this exact 16-triangle immediate MODULATE+blend packet.  The
kind-7 cookie timeout therefore does not invalidate the native MUL/TAU object
or the API combination in every packet shape.

Current Mesa selects cmd22 when the fragment has a valid semantic object and
PRIMARY0 is supported (`chrome9_draw.c:129-135,273-295`); this check precedes
the legacy-only MODULATE+blend rejection at `:900-904`.  A projected GL1
sample can use that route only after the private NIR lowering recognizes a
direct 2-D coordinate and its own Q (`chrome9_context.c:3874-3911`) and every
post-TNL Q has raw bits `3f800000` (`chrome9_draw.c:1225-1290`).  The generic
batch submits `DRM_IOCTL_CHROME9_DRAW_FRAGMENT_PROGRAM`
(`chrome9_draw.c:1865-1878`); the kernel tags it as kind
`CHROME9_DRAW_FRAGMENT_PROGRAM` (`chrome9_engine.c:7482-7508`), outside the
legacy state/kind rejection at `:4047-4067`.  Neither API face culling nor
polygon offset should be added in the packet: Gallium Draw already performs
clipping, winding cull, offset and triangle decomposition
(`chrome9_draw.c:244-253`).

This is more than an encoding argument.  The guarded VX900 cmd22 oracle in
`ioquake3-alpha-depth-blend-cmd22-qualification.md:29-76` passed the exact
`TEX s0,T0.xy; MUL oC0,s0,PRIMARY` result with GL1 same-factor source-over:
BGRA `40807f00`, XRGB RGB `00807f00`, unchanged source and guards, clean
retirement.  The same two-triangle oracle passed GE128 plus D16 LEQUAL/write,
EQUAL/read-only dlight (`ONE,ONE` and `DST_COLOR,ONE`), fog source-over with
EQUAL and LEQUAL/read-only, and a masked-base/lightmap state transition.
Fixed alpha comes from A00 bit 20 and A33, not a TEXKILL substitution.  The
legacy immediate MODULATE list remains rejected.

The remaining count boundary is concrete: the qualified cmd22 oracle issued
two triangles per draw, while Mesa coalesces generic cmd22 draws up to 16
triangles (`chrome9_draw.c:32,1592-1618`).  A 16-triangle *legacy* packet
timed out; that neither proves nor disproves the different 16-triangle cmd22
packet.  Before treating cmd22 as a sustained UI path, retain the legacy
kernel guard and require this small, ordered matrix:

| rung | fixed inputs | required proof |
| --- | --- | --- |
| route | real GL1 projected MODULATE NIR, Q exactly one, BGRA and XRGB | semantic-valid PRIMARY/T0 object; cmd22 selected, no legacy ioctl or silent reject |
| count | identical texture/target/blend; 2 (known), then 4, 8, 16 triangles | exact cookie, owned PAUSE, error register zero, target samples, source/guards intact; stop at first fault |
| state | one or two triangles, then actual asset sizes/filter/mips | repeat BGRA/XRGB source-over, GE128+LEQUAL/write, EQUAL/read-only dynamic-light/fog with both alpha and depth readback |
| cull | opposite windings under `front_ccw` and FRONT/BACK/NONE | Draw emits only the surviving triangles; cmd22 native A01 cull stays off |

The raw cmd22 fixed-state combinations have already passed with constant 2x2
sources.  The genuinely new qualification is the 4/8/16 generic packet count
and integration with real sampled assets; do not rerun the fixed-state matrix
as if it were a missing hardware feature.

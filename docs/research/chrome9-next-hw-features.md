<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Chrome9 next hardware features: cube maps, DXTn, strips/fans, two-sided colour

Status: 2026-09-24.  Host-only research.  Nothing in this note ran on VX900
silicon as part of this work.  The note ranks four unused hardware features
by their chance of working and their value for ioquake3, glmark2 and GL
conformance.  For each feature it gives the register contract with
evidence, an implementation plan and silicon rungs that can be run one at a
time.  The rung tool lives outside the source trees:
`~/tmp/agent-features/chrome9_next_oracle.c` plus
`chrome9_next_oracle_tex.c` (GPL-2.0-only).  See section 7.

Evidence labels:

* **manual**: `OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.pdf`, text
  extraction `~/tmp/ogpm/OGPM_Chrome9 HD DX9 _R100_PartII_3D_Video.txt`,
  cited as `M:<line>`.  **manual/HCM** and **manual/HC3** are the VX855 and
  VX800 Part 2 texts in the same directory.
* **static**: disassembly of `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`
  (`~/tmp/via_dri.dis`, cited `dri:0x…`) and of the Windows
  `VTGUModeDX32A.dll` (`win:0x…`) and `VTGOGL32A.dll` (`ogl:0x…`).
* **dynamic**: vendor code run on the host without a GPU
  (`tools/re/chrome9_pps_probe.c`, `chrome9_tcl_ffvs_gl_probe.c`,
  `chrome9_twoside_state_probe.c`).
* **silicon**: results already recorded in this directory.
* **inference**: derived from the above, not observed.

## 1. Ranking

| rank | feature | value | chance | main open point | first rung |
|---:|---|---|---|---|---|
| 1 | cube maps | high for conformance: GL 1.3 core, today `chrome9_resource_layout()` rejects every non-2D target, so cube textures cannot even be created | high: image, layout and shader words are static or dynamic exact | face orientation on silicon | `cube c1` |
| 2 | DXT1/DXT3/DXT5 | medium-high: `EXT_texture_compression_s3tc` (not exposed today, not even through Mesa's RGBA8 fallback), 4:1/8:1 texture bandwidth on a UMA part, ioquake3 `r_ext_compressed_textures 1` | high for one level (format bytes manual+static); mip layout depends on one discriminator | whether POT levels come from the subtype-10 table or are derived | `dxt p0`, then `dxt d1` |
| 3 | native triangle strip / fan | low-medium: ioquake3 GL1 sky and beams use strips (`tr_sky.c:372`, `tr_surface.c:328`), glmark2 draws a few 4-vertex strips; saves vertex/index traffic and VS work | very high: fan is already silicon-proven, strip words are static-exact | strip reuse and culling parity on silicon | `topo f1`, `topo s1` |
| 4 | two-sided colour | low-medium: no default consumer (ioquake3, glmark2, glxgears), conformance and legacy demos | native oBFD low; two-pass culling split high and needs no UAPI change | none for the split; native selection likely absent on VX900 | `split s1` |

Recommended execution order differs slightly from the value ranking,
because two rungs run on the current kernel without any change:

1. `split s1..s4` (current UAPI) and `dxt p0` (current UAPI).
2. DXT single level (smallest kernel/Mesa patch), then DXT mips per the
   `p0` outcome.
3. Cube maps.
4. Strip/fan topology.

Not pursued here: MSAA, occlusion queries, 3D textures, polygon modes, point
and line primitives (see [chrome9-lines-points-twoside-re.md](chrome9-lines-points-twoside-re.md)),
stage B rungs B7/B8/B13/B14 and pixel-shader flow control.

## 2. Cube maps

### 2.1 Register contract

Type-02 subtype `n` image block, one texture unit:

| word | value | meaning | evidence |
|---|---|---|---|
| A00..A05 | `(face_root[f]) >> 8` | L0 base of faces +X, −X, +Y, −Y, +Z, −Z | manual M:6921..6980; static `dri:0x1b27ec..0x1b2879`; face order static `ogl:0x10148b7d..0x10148b99` (target − 0x8515, no permutation) |
| A18 | `18015355` (all faces SF) | `[11:8]=3` cube, bit 12 relative mip offsets, six location fields `[1:0] [3:2] [5:4] [7:6] [15:14] [17:16]` = 1 | manual M:6991 (two face labels misprinted); static `dri:0x1b263d..0x1b2647`, packing `0x1b27c5..0x1b2d38` |
| A20 | `20000000 \| H << 12 \| W` | texel size, square | static |
| A21 | `21000001` | unchanged from 2D | static |
| A22 | `22000000 \| log2 H << 4 \| log2 W` | POT bit clear, so the subtype-10 table is authoritative | static: the Linux builder keeps A22[15] only for compressed or volume surfaces (`dri:0x1b2381..0x1b2395`) and writes the cube table; Windows keeps POT for cube (`win:0x101d7cc6..0x101d7ce5`).  The earlier packet `22008055` is the Windows form; both agree when levels use the vendor layout (section 3.3) |
| A30 | format byte as for 2D | unchanged | static |
| subtype-10 L1..L8 | `0x40000000 \| (level_offset − L0_offset) >> 8`, shared by all faces | relative to each face root (A18 bit 12) | static `dri:0x1b2678..0x1b26c8` (same packer as 2D, `0x1b2adc..0x1b2b88`); 2D form silicon |
| A32 | `32000049` | S/T clamp-to-edge, R clamp (bit 6, already emitted) | manual (R-axis fields are volume-only); the vendor does not force clamp (`dri:0x1b17b0`) |

Closure (manual M:6939 "Program it back to 0" for A02): a 2D draw after a
cube draw must re-emit A18 `18001101` and write A01..A05 = 0.

Level limit: the vendor clamps cube to nine levels
(`dri:0x1b264a..0x1b2654`) because cube always uses the eight-entry
subtype-10 table.  The 2D L9..L11 mip tail rule does not apply.  Advertise
`max_texture_cube_levels = 9` (256×256 complete cube) (static; limit value
inference).  There is no seamless-cube bit in A30/A31/A32/A35/A36
(manual), so `seamless_cube_map` stays off.

### 2.2 Memory layout: one buffer object

The previous plan in
[chrome9-format-cube-bridge-closure.md](chrome9-format-cube-bridge-closure.md)
proposed six GEM handles because the vendor face stride was not identified.
It is now identified (static):

* `sfmiUpdateStructureTexture_inv` (`dri:0xdd360`, `0xdd395`, `0xdd503`) sets the
  root size to `sfmCalcMipSize_inv(root, levels) * 6` for cube surfaces.
* `sfmCalcMipSize_inv` (`dri:0xec4d0`) sums the level sizes and, for cube,
  rounds the sum up to 256 bytes (`0xec506..0xec515`).
* `sfmAllocateTextureMemory_inv` (`dri:0xddd80`) makes one allocation, and
  `sfmCalcCubeTextureOffset_inv` (`dri:0xec1d0`) places face `f` at
  `(root_size / 6) * f` plus the level offset inside the face.

Therefore:

```text
level size   = ALIGN(w * cpp, 32) * ALIGN(h, 8)      (uncompressed)
level offset = sum of the previous level sizes inside the face
face_stride  = ALIGN(sum of all level sizes, 256)
face_root[f] = base + f * face_stride
```

Every face root is 256-byte aligned by construction.  One handle is enough.

Worked values (BGRX, unit 0, guard page at offset 0, `B` = BO address):

| texture | face stride | face offsets | subtype-10 |
|---|---:|---|---|
| 32×32, 1 level | `0x1000` | `0x1000 + f*0x1000` | none |
| 32×32, 3 levels (L1 +0x1000 pitch 64, L2 +0x1400 pitch 32) | `0x1500` | `0x1000, 0x2500, 0x3a00, 0x4f00, 0x6400, 0x7900` | `40000010 40000014 0 0 0 0 0 0` |
| 256×256, 9 levels | `0x55800` | `0x1000 + f*0x55800` | `40000400 40000500 40000540 40000550 40000554 40000555 40000556 40000557` |

The 3-level sampler words are `30000080 31100001 32000049` (max LOD 2,
nearest/nearest-mip, clamp).

### 2.3 Pixel-shader coordinate and TAU operation

This corrects the earlier closure note, which required TAU operation 7
(TEXLDP) for cube.  A host run of the vendor pixel-shader compiler through
`chrome9_pps_probe` gives (dynamic):

| source | TAU |
|---|---|
| `dcl_2d s0; texld r0, t0, s0` | `301001b0` |
| `dcl_cube s0; texld r0, t0, s0` | `301001b0` |
| `texldp r0, t0, s0` | `307001b0` |

ALU words, A90..A9A and resource counts are byte-identical.  D3D `texCUBE`
is therefore ordinary TEXLD (operation 1); the cube lookup is selected by the
image state (A18 mode 3), not by the shader object.  Operation 7 is only what
the GL fixed-function builder emits for its four-lane STRQ coordinates
(static, [chrome9-format-cube-bridge-closure.md](chrome9-format-cube-bridge-closure.md)).

Two coordinate forms exist:

| form | TAU | FE A01 | FE A08 | lanes | status |
|---|---|---|---|---|---|
| B (first) | `301001b0` TEXLD | `01002522` | `08010201` | S, T, R | D3D compiler-exact; A01/A08 inference from the manual dimension field |
| A (fallback) | `307001b0` TEXLDP | `01002523` | `08010301` | S, T, R, Q=1 | GL fixed-function production-exact |

Form B fits the kernel as it is: `chrome9_fragment_consumer_width()`
(`chrome9_engine.c`, near line 2234) caps a textured consumer at three lanes.
Form A needs that cap lifted for the cube coordinate and a new operation
value at the TAU encoder (`chrome9_fragment.c:1031`, `0x20100000`, field
`[22:20]`).  Whether TEXLDP divides by Q for cube is unknown; with Q = 1 it
does not matter.  Face selection and major-axis projection are scale
invariant, so an unnormalized direction is acceptable (rung `c4`).

LOD: no cube-specific LOD state exists in the manual or the vendor code.
That the hardware computes LOD on the projected face coordinates is an
inference; rung `c2` forces LOD by min/max clamp and does not depend on it.

### 2.4 Implementation plan

Kernel:

1. UAPI, fragment texture state (fail closed, new capability):
   `reserved[0]` becomes `target` (0 = 2D, 1 = CUBE) and `reserved[1]`
   becomes `face_stride`.  `level_offset[]`/`level_pitch[]` describe face 0.
2. `chrome9_validate_fragment_texture()`: for CUBE require square POT
   width = height, `last_level ≤ 8` (256×256 for a complete chain),
   CLAMP_TO_EDGE on S and T,
   level offsets and pitches as today, `face_stride` 256-aligned and not
   smaller than `ALIGN(end_of_last_level − L0_offset, 256)`, and an object
   size of `L0_offset + 6 * face_stride` plus the guard page.  Reject the
   X8Z24 format for cube.
3. `chrome9_emit_fragment_texture()`: emit A00..A05 as six relocated face
   roots, A18 `18015355`, A22 without bit 15 (the 2D code sets it for every
   POT size), keep A20/A21/A30, and emit the existing subtype-10 table.
   For 2D emit A01..A05 = 0 and A18 `18001101` whenever the previous state
   of that unit was cube (or always, which costs five dwords).
4. The TEX instruction needs a three-lane coordinate for a cube sampler.
   Reject TEX of a cube sampler with a two-lane coordinate, and a 2D
   sampler used by a cube TEX.

Mesa:

1. `chrome9_resource.c`: accept `PIPE_TEXTURE_CUBE` (array_size 6) in
   `chrome9_is_surface_target()`/`chrome9_resource_layout()` with the face
   stride above; per-layer transfers address `face_root[layer]`.
   `chrome9_format_supported()` must accept CUBE for the sampled formats.
2. `chrome9_screen.c`: `max_texture_cube_levels = 9` only with the
   capability.
3. Generic TEX lowering (`chrome9_context.c:4372` and the other
   `GLSL_SAMPLER_DIM_2D` gates at 842, 878, 1347, 2145, 4936): accept
   `GLSL_SAMPLER_DIM_CUBE`, `!is_array`, `!is_shadow`, three coordinate
   components; pass the texture target to the kernel texture state.
4. Sampler state: clamp S/T for cube regardless of the GL wrap mode
   (GL ignores wrap for cube lookups).

### 2.5 Rungs

Build and command line are in section 7.  All draws are post-TNL,
nearest, into a 64×64 BGRA target, with guard pages on every object.

| rung | variable | expected |
|---|---|---|
| `cube c1` | one level, six faces in one BO (stride `0x1000`), 3×2 grid of cells, each sampling one face over s,t ∈ [0.1, 0.9] | 24 unique face/quadrant colours at the quadrant centres; a mismatch prints the observed face and quadrant, which identifies a permutation, rotation or mirror in one run |
| `cube c2` | three levels, LOD forced per level by min = max LOD | the level colour on every face (proves relative subtype-10 offsets per face root) |
| `cube c3` | `c1`, then face 0 as an ordinary 2D texture | 2D quadrants exact (A18/A01..A05 closure) |
| `cube c4` | `c1` with directions scaled by 3 | identical to `c1` (magnitude invariance) |

If `c1` shows a consistent transform, fix it in the kernel face order or
Mesa's coordinate lowering, not in the test.  If form B fails with no
sampled colour at all, retry with form A (TEXLDP, Q = 1) before anything
else.

## 3. DXT1, DXT3 and DXT5

### 3.1 Register contract

| field | value | evidence |
|---|---|---|
| A30 format byte | DXT1 `29` (RGB and RGBA), DXT2/3 `2a`, DXT4/5 `2b` | manual M:7138, M:7197..7201; static `INV_D3DFmt_DDPFToTX dri:0x1a6db0` (private 0x10400 → 29, 0x20807 → 2a, 0x20809 → 2b); GL table `dri:0x4298e0` maps both 0x83f0 and 0x83f1 to 0x10400 |
| other A30 bits | `[15]`, `[14]`, `[13:11]` zero; bit 8 only for sRGB | static |
| A20 | texel width/height, not blocks | static `dri:0x1b24aa..0x1b24d8` |
| A22 | `[3:0]` log2 W, `[7:4]` log2 H, bit 15 POT | static |
| A18 | `18001101` (2D, SF), no tiling | static `dri:0x1b2a8a` |
| filtering | nearest, linear and mip filtering allowed | manual restricts only float and Z formats |

DXT1 punch-through (three-colour mode, code 3 = transparent black) is always
decoded, because RGB and RGBA share selector `29`.  GL `DXT1_RGB` needs a
view swizzle A = 1 on top (inference; Mesa format table lanes
`{D0, D1, D2, ONE}`).  DXT2 and DXT4 (premultiplied) share the hardware
selectors and must not be advertised on that basis alone.  sRGB variants need
the de-gamma table from [fixed-function-texture-sync.md](fixed-function-texture-sync.md)
and stay disabled.

### 3.2 Memory layout

`sfmCalcTextureSizeAlignment_inv` (`dri:0xebb10`), `sfmCalcMipOffset_inv`
(`0xec2e0`), `sfmCalcMipSize_inv` (`0xec4d0`) and the upload path
`sfmWriteS3TC_invi` (`0xe3330` → `sfmCalculateLinearOffset_invi 0xdedf0`)
give (static):

```text
bpp            = 4 (DXT1) or 8 (DXT3/DXT5)
pitch(level)   = ALIGN(ALIGN(w, 4) * bpp / 2, 32)    bytes per 4-row block row
rows(level)    = ceil(h / 4)
size(level)    = pitch * rows                          (no 256 rounding)
offset(level)  = sum of the previous level sizes
texel (x, y)   = offset + (y / 4) * pitch + (x / 4) * (2 * bpp)
```

Every level narrower than 16 (DXT1) or 8 (DXT3/5) texels takes 32 bytes per
block row.  Offsets are 32-byte aligned only; for DXT1 32×32 the levels sit
at `0, 0x200, 0x280, 0x2c0, 0x2e0, 0x300`.

| texture | A20 | A22 | A30 | L0 off/pitch | L1 off/pitch | L2 off/pitch |
|---|---|---|---|---|---|---|
| DXT1 32×32 | `20020020` | `22008055` | `30290000` | 0 / 64 | `0x200` / 32 | `0x280` / 32 |
| DXT1 64×64 | `20040040` | `22008066` | `30290000` | 0 / 128 | `0x800` / 64 | `0xa00` / 32 |
| DXT5 32×32 | `20020020` | `22008055` | `302b0000` | 0 / 128 | `0x400` / 64 | `0x500` / 32 |
| DXT5 64×64 | `20040040` | `22008066` | `302b0000` | 0 / 256 | `0x1000` / 128 | `0x1400` / 64 |

DXT3 is DXT5 with A30 `302a0000`.  The single-level vectors in
[fixed-function-texture-sync.md](fixed-function-texture-sync.md) remain
correct (pitch 64/128, `ceil(h/4)` block rows).

### 3.3 The POT mip question (also affects existing 2D mips)

The subtype-10 table requires 256-byte aligned offsets (manual M:8463..8475),
but vendor compressed offsets are not 256-aligned.  The vendor resolves this
as follows (static):

* The POT flag is cleared for every texture that is neither compressed nor
  volume, unless an unidentified option (`+0x248`) is set
  (`dri:0x1b2381..0x1b2395`, `0x1b2a38..0x1b2a4e`).  Windows keeps POT only
  for tiled, compressed or cube (`win:0x101d7cc6..0x101d7ce5`).
* When the POT flag stays set, both vendors skip the subtype-10 table and
  dirty only A00/A18/A20..A22/A30 (`dri:0x1b2aa6` → `0x1b2d24`;
  `win:0x101d864b`).

The strong inference is that, with A22[15] set, the sampler derives level
addresses itself using the vendor layout.  The measured L9..L11 mip tail at
`+0x100` steps ([mip-tail-l9-l11-oracle.md](mip-tail-l9-l11-oracle.md))
already proves that the hardware derives levels beyond the eight table
entries, and `+0x100` is exactly the vendor size of a 1×1…4×1 32-bit level
(`pitch 32 × ALIGN(h, 8) = 256`).

The current kernel sets A22[15] for every POT texture and also emits the
table.  Mesa's layout (`ALIGN(w*cpp,32) * h`, each level 256-aligned)
matches the derived vendor layout for every level with `h ≥ 8` or
`pitch * 8 ≤ 256`, so existing silicon results cannot tell the two models
apart.  They diverge when a level other than the last has `h ≤ 4` and
`pitch ≥ 64`, that is 32-bit POT textures with an aspect ratio of at least
4:1 (8:1 at 16 bits, 16:1 at 8 bits).  Example: 256×32 32-bit, L3 32×4 is
1024 bytes in the vendor rule and 512 in Mesa's, so L4 moves.  If rung `dxt p0`
reports DERIVED, Mesa must use `rows = ALIGN(h, 8)` for POT uncompressed
levels as well; that is an existing latent defect, not only a DXT issue.

Plan by outcome of `p0`:

* **DERIVED** (expected): POT compressed textures use the vendor-packed
  offsets exactly, A22[15] set, and the kernel emits **no** subtype-10
  table for them.  NPOT compressed textures are single-level only (the
  vendor would write truncated table entries, a latent vendor defect).
* **TABLE**: compressed levels start 256-aligned (Mesa layout), the table
  is emitted as today, and rung `m3` replaces `m1`.

### 3.4 Implementation plan

Kernel:

1. UAPI: `DRM_CHROME9_FRAGMENT_TEXTURE_DXT1_RGB = 7`, `DXT1_RGBA = 8`,
   `DXT3 = 9`, `DXT5 = 10` behind a new capability.  All capability bits up
   to 63 are allocated or have gaps of unknown history (24..26, 31, 33,
   36..38); a second capability word (`DRM_CHROME9_PARAM_CAPABILITIES2`) is
   the safer allocation.
2. `chrome9_fragment_texture_format()`: 7, 8 → `0x29`; 9 → `0x2a`;
   10 → `0x2b`.
3. `chrome9_validate_fragment_texture()`: a block-format branch with
   `level_pitch[i] == ALIGN(ALIGN(w,4)*bpp/2, 32)`, `size = pitch * ceil(h/4)`;
   POT: offsets relative to `level_offset[first_level]` must equal the
   packed sums (DERIVED model); NPOT: `first_level == last_level`.
4. `chrome9_emit_fragment_texture()`: no subtype-10 block for POT
   compressed textures (DERIVED model).

Mesa:

1. `chrome9_format.c`: `PIPE_FORMAT_DXT1_RGB` (lanes `D0 D1 D2 ONE`),
   `DXT1_RGBA`, `DXT3_RGBA`, `DXT5_RGBA`; sampler binding only.
2. `chrome9_resource.c`: block-aware layout through
   `util_format_get_nblocksx/y()` and `util_format_get_blocksize()`, the
   vendor pitch and packing above; transfers in whole blocks.
3. Mesa then advertises `EXT_texture_compression_s3tc` from
   `is_format_supported()` (`st_extensions.c:858`).  Uploads of
   uncompressed data to a compressed internal format use Mesa's built-in
   S3TC encoder.

Independent quick path: advertising `PIPE_FORMAT_R8G8B8A8_UNORM` as a
sampler format (A30 `b9`, A8B8G8R8, static and live-listed in
[fixed-function-texture-sync.md](fixed-function-texture-sync.md)) would make
Mesa expose S3TC through its CPU decompression fallback
(`st_extensions.c:997`), without any compressed hardware path.  It gives the
extension, not the bandwidth saving.

### 3.5 Rungs

| rung | UAPI | variable | expected |
|---|---|---|---|
| `dxt p0` | current | BGRX 32×32, two levels; the table puts L1 at +0x2000 (blue), the derived position +0x1000 holds red; LOD forced to 1 | prints `DERIVED offsets` (red) or `TABLE used` (blue); any other colour fails |
| `dxt d1` | proposed | DXT1 32×32, quadrants red/green/blue/white, selector 0 | exact `ffff0000 ff00ff00 ff0000ff ffffffff` per quadrant |
| `dxt d2` | proposed | DXT1 four-colour mode, red/blue endpoints, texel selector `(x+y)&3` | every texel within ±2 of the S3TC reference; proves block-internal texel order |
| `dxt d3` | proposed | DXT1 three-colour mode (`c0 ≤ c1`), selectors 2/3 | midpoint ±1, code 3 = `00000000` (alpha 0 written to the BGRA target) |
| `dxt d4` | proposed | DXT3, alpha nibble = texel index | destination alpha `17 * i` exactly |
| `dxt d5` | proposed | DXT5 eight-alpha mode 255/0, selector `(x+2y)&7` | alpha within ±1 of `((8−s)·255)/7` |
| `dxt d6` | proposed | DXT5 six-alpha mode, codes 6/7 | alpha exactly 0/255 |
| `dxt m1` | proposed | DXT1 32×32 six levels, vendor-packed offsets, one colour per level, LOD forced per level | level colour at centre and corners for L0..L5 |
| `dxt m2` | proposed | `m1` with DXT5 | same |
| `dxt m3` | proposed | `m1` with 256-aligned levels | only if `p0` reported TABLE |

The alpha rungs write the sampled alpha straight into the BGRA8888 target;
no blending is involved.  Every DXT rung also checks that the source bytes
are unchanged.

## 4. Native triangle strips and fans

### 4.1 Register contract

A20 (manual M:9428..9500): `[21:16]` second/even single-vertex cycle and
`[13:8]` first/odd cycle, two bits per vertex a, b, c (00 new, 01 previous
a, 10 previous b, 11 previous c); bit 14 automatic fast cycle; bit 1 32-bit
index; bit 0 index mode.  Decoded vendor words
([chrome9-lines-points-twoside-re.md](chrome9-lines-points-twoside-re.md) §3.1):

| topology | A20 inline | A20 16/32-bit index | A24 | hardware triangle k |
|---|---|---|---|---|
| list | `20000000` | `20000001` / `20000003` | `3T` | `(3k, 3k+1, 3k+2)` |
| strip | `201c7800` | `201c7801` / `201c7803` | `T+2` | even `(k, k+1, k+2)`, odd `(k+1, k, k+2)` |
| fan | `201c5c00` | `201c5c01` / `201c5c03` | `T+2` | `(0, k+1, k+2)` |

Silicon: the fan is already proven twice.  The original fixed quad
(`chrome9_drv.c:771`, `201c5c00 23000f02 24000004`) and the `VS_FETCH`
fixed quad (`201c5c01`, A24 = 4, indices `0 1 2 3`) both pass 1024/1024
pixels ([chrome9-hw-tcl-re.md](chrome9-hw-tcl-re.md), `fetch` row).  The
strip word is static-exact but has not run.

Properties the implementation relies on:

* Vertex c is always the newest vertex, which is the GL provoking-last
  vertex for strips and fans, so FlatC flat shading is correct natively
  (inference from the cycle table; rungs `p1`/`p2`).
* The odd strip cycle swaps a and b, so the transmitted winding equals the
  GL winding and hardware culling stays correct (inference; rung `c1`).
* The A2C..A2E index-range check ignores **this and all following
  indices** (manual M:10064..10075); there is no primitive restart.  A
  restart must become a new A24 chunk or a new draw.
* Both vendors decompose strips and fans into lists when two-sided lighting
  is on and culling is off (static `dri:0x230ace`, tables `0x4297e0` /
  `0x429860`; `ogl:0x10389e68` / `0x10389ee0`).  Keep that rule.

### 4.2 Implementation plan

Kernel (vertex program ABI 2, one new capability):

1. `drm_chrome9_draw_vertex_program`: the first reserved u32 becomes
   `topology`, using the enum already proposed in
   [chrome9-lines-points-twoside-re.md](chrome9-lines-points-twoside-re.md)
   §6.2 and extended: `TRIANGLE_LIST = 0, POINT_LIST = 1, LINE_LIST = 2,
   LINE_STRIP = 3, TRIANGLE_STRIP = 4, TRIANGLE_FAN = 5`.
2. `chrome9_tcl_prepare()`: for strip/fan accept `vertex_count ≥ 3`
   (stage A) or `index_count ≥ 3` (stage B) instead of a multiple of three;
   reject `BFC_*` flags with strip/fan.
3. `chrome9_tcl_raster_state()` (the A20 word near `chrome9_engine.c:2397`):
   OR `0x1c7800` or `0x1c5c00` into A20.  A24 already carries the vertex or
   index count.  Every draw re-emits A20, so no reuse state leaks.
4. A strip is never split across segments.  Oversize requests fail with
   `-E2BIG` and Mesa splits them.

Mesa (`chrome9_tcl.c`, near `tcl_triangle()` at line 1141):

* Native `TRIANGLE_STRIP` and `TRIANGLE_FAN` when the capability exists, no
  primitive restart is active, and either smooth shading or flat with
  provoking-last.
* `QUAD_STRIP` maps to a native triangle strip for smooth shading (same
  vertex order).  Flat quad strips, quads, polygons with flat shading and
  provoking-first flat strips stay decomposed.
* Split long strips at an even triangle index (keeps parity) with an
  overlap of two vertices; split fans by repeating vertex 0 and the last
  vertex.

### 4.3 Rungs

`topo` compares each native draw pixel-exactly with the same triangles sent
as a list in hardware vertex order, with a distinct Gouraud colour per
vertex, and ends with a plain list draw into a fresh target.

| rung | variable | expected |
|---|---|---|
| `topo f1` | fan, 5 vertices inline | identical to the list reference |
| `topo s1` | strip, 5 vertices inline | identical |
| `topo s3` / `s4` | minimal 3-vertex strip / 6-vertex strip | identical |
| `topo s2` / `f2` | strip 5 / fan 6 through a VBO, 16-bit indices, reversed storage order | identical; VBO guard pages intact |
| `topo c1` / `c2` | strip with `CULL_CW` / fan with `CULL_CCW` | identical to the culled list (winding parity) |
| `topo p1` / `p2` | strip / fan with `FLAT` (FlatC) | identical (provoking vertex) |
| `topo l1` | strip then a list in one `DRAW_VERTEX_PROGRAMS` job | identical (A20 reuse-state closure) |

## 5. Two-sided colour

### 5.1 New evidence

1. **The VX900 manual dropped the selection rule** (manual; high).  The HC3
   and HCM manuals carry the A23 "Note to Driver" rule "if the FVF mask has
   back-face colour 0/1 and the triangle meets HBFace4BFD, then C0 = BFC0"
   (manual/HCM near line 9530, manual/HC3 near line 8205).  The HD manual
   copies the A23[7] field text word for word (M:9547..9549) but not the
   note.
2. **There is no transport slot for oBFD** (manual; high).  On HC3/HCM the
   vertex layout is a list of 6-bit FVF attribute ids that includes `31h`
   back-face colour 0 and `32h` back-face colour 1.  On VX900 those
   sub-addresses became input-stream declarations, and the post-VS layout is
   the A25/A26/A28/A29 bit masks.  Every one of their bits is assigned (X,
   Y, Z, W, point size, Cd, Cs, fog, then S/T/R/Q for coordinates A..J;
   M:9580..9700).  None is a back-face colour.
3. **The vendor A61[19] writer is chip-generic** (static; medium).
   `stmUpdateVS_inv` (`dri:0x17c5db..0x17c620`) packs A61[23] = VS version
   ≤ 8, A61[22:20] = `+0x08`, A61[19] = `+0x0c != 0` with no family branch.
   The vendor setting A61[19] on VX900 therefore does not show that the
   silicon honours it.  The parser also accounts 4 dwords per oD2/oD3 in
   `parsed+0x39f4`, which nothing reads.
4. **The pixel pipeline knows the winding, but only in the stencil unit**
   (silicon; high).  Two-sided stencil works (`chrome9_stencil_guard_smoke
   --faces`).  No pixel-shader face source exists: vFace and vPos compile to
   identical bytes
   ([chrome9-vface-normal-sign-boundary.md](chrome9-vface-normal-sign-boundary.md)).

Together with the silicon table in
[chrome9-twoside-enable-re.md](chrome9-twoside-enable-re.md) (every variant
front colour), native oBFD selection on VX900 is unlikely (inference,
medium).  A23[7] is most likely a leftover field.

### 5.2 Remaining native experiments (optional, stop if 1..3 fail)

Base: `chrome9_tcl_oracle twoside t1` words (oD0 white, oD2 = c0
`ff4080bf`, A61 `61880000`, BFC_CW).  "Select" means CW `ff4080bf`, CCW
`ffffffff`.

| # | change | expected if selection exists | risk |
|---:|---|---|---|
| 1 | vendor cull-front shape: `BFC_CW \| CULL_CCW`, VS writes only oPos and oD2 (no oD0/oD1/oD3), A25 `25f40000`, A28 `28fd0000` | CW `ff4080bf`, CCW blank | low |
| 2 | re-emit A61 `61880000` and A62 after the `31030000` upload and A60 (vendor order) | select | low |
| 3 | PS source class 1 id 2 (`00f04205`), no culling | `ff4080bf` on both windings would show oD2 reaches the pixel stage | medium (extrapolated encoding) |
| 4 | type-01 A01 bit 12 clear (vertex cache off), `01de1d40` → `01de0d40` | select | medium |
| 5 | A11 `11c00000`, then separately A2F `2f582000` | select | low-medium |
| 6 | A61 `61980000` | select | medium-high; last |

Never set A61[18] or A61[16].

### 5.3 Production path: split by hardware culling

This needs no kernel change.  It uses the silicon-proven culling (A01[7],
A23[8]; rungs t4/t4b) and the existing `DRAW_VERTEX_PROGRAMS` batch, which
already allows a different program and flags per draw with shared
destination and depth.

Mesa `chrome9_tcl.c`:

1. Replace `bool twoside` in `tcl_program()`, `struct chrome9_tcl_program`
   and `chrome9_tcl_compile()` by a mode `{ OFF, FRONT, BACK }`.  FRONT
   stores COL0 to PRIMARY (today's path); BACK stores BFC0 to PRIMARY.  If
   the VS does not write BFC0, use one FRONT pass (GL leaves the back colour
   undefined).
2. `chrome9_tcl_draw()` with `light_twoside`:
   * `cull_face == FRONT`: one BACK pass culling the front winding.  Exact
     and free; it replaces today's fallback for this case.
   * `cull_face == NONE`: two draws in one `DRAW_VERTEX_PROGRAMS` call:
     FRONT culling the back winding, then BACK culling the front winding,
     with identical buffers, indices, viewport, constants and fragment state.
   * `cull_face == BACK`: today's single FRONT pass.
3. The split reorders front faces against back faces.  Use it only when
   that is invisible: blending off or order-independent (for example ADD
   with ONE, ONE), depth write on with LESS/LEQUAL/GREATER/GEQUAL (exact
   depth ties between a front and a back face may flip, the same class as
   GL invariance slack), and stencil writes off or commutative.  Otherwise
   keep the draw-module fallback.
4. Remove the `ctx->tcl_twoside` gate; leave `BACK_PRIMARY` and
   `CAP_VERTEX_TWO_SIDE` in the UAPI, unadvertised.  If a native rung ever
   selects, prefer it over two passes.

Cost: VS, fetch and index work double for `cull_face == NONE`; raster work
is unchanged because culled triangles end in setup.  Strip/fan draws may
stay native under the split, because each pass has culling on.

### 5.4 Rungs (current UAPI)

`split` draws the `twoside` geometry (triangle A and its mirror B with the
opposite winding) with `hw_tcl=1`.

| rung | variable | expected |
|---|---|---|
| `split s1` | FRONT pass (`CULL_CW`, PRIMARY = white) then BACK pass (`CULL_CCW`, PRIMARY = c0), two ioctls | CW `ff4080bf`, CCW `ffffffff` |
| `split s2` | the same two draws in one `DRAW_VERTEX_PROGRAMS` job | same |
| `split s3` | `s2` with FLAT and per-vertex colours, back pass from a second colour stream | CW = third back colour `ff000080`, CCW = third front colour `ff0000ff` |
| `split s4` | `s2` with the passes in reverse order | same as `s2` |

Each rung ends with the qualified CULL_CW reference into a fresh target.

## 6. Side finding for existing mipmaps

Section 3.3 applies to every POT texture the kernel emits today.  If
`dxt p0` reports DERIVED, the subtype-10 table is dead state for POT
textures.  The safe change does not need to wait for the rung: every
uncompressed vendor level size `ALIGN(w*cpp, 32) * ALIGN(h, 8)` is a
multiple of 256, so the derived packing is also a valid set of 256-aligned,
non-overlapping table offsets.  Switching Mesa's `chrome9_resource_layout()`
to `rows = ALIGN(h, 8)` for POT levels is therefore correct under both
models, and the kernel can then require exactly these offsets for POT
textures.  Only the textures named in section 3.3 (a non-final level with
`h ≤ 4` and `pitch ≥ 64`) change today's layout.

## 7. Rung tool

Location (not in the source trees):

```text
~/tmp/agent-features/chrome9_next_oracle.c       modes, main
~/tmp/agent-features/chrome9_next_oracle_tex.c   dxt and cube modes (#included)
~/tmp/agent-features/include/chrome9_drm.h       copy of the current UAPI
~/tmp/agent-features/dxt/dxt_layout.py           vendor compressed layout
~/tmp/agent-features/cube/cube_layout.py         vendor cube layout
```

Build (host or target):

```sh
cd ~/tmp/agent-features
gcc -O2 -Wall -Wextra -Wno-sign-compare -I/usr/include/libdrm -Iinclude \
    -o chrome9_next_oracle chrome9_next_oracle.c
./chrome9_next_oracle host        # CPU references only: "0 failures"
```

The topo modes use the landed topology field (`topology`, kernel
`4f9c199`) and need `CAP2_VERTEX_TOPOLOGY` (`hw_tcl=1`).  The cube and DXT
modes use the named UAPI of the implementation patches and check
CAPABILITIES2 first (section 9.6).

Commands, one per run (`CHROME9_DEVICE` overrides the render node):

```text
./chrome9_next_oracle qualify                      run first: known-good draws
./chrome9_next_oracle split s1|s2|s3|s4            current UAPI, needs hw_tcl=1
./chrome9_next_oracle dxt p0                       current UAPI, POT derivation
./chrome9_next_oracle dxt d1|d2|d3|d4|d5|d6|m1|m2  dxt=1
./chrome9_next_oracle cube c1|c2|c3|c4
./chrome9_next_oracle topo f1|s1|s2|f2|s3|s4|c1|c2|p1|p2|l1
```

Exit status 0 means pass.  Every mode checks the guard pages of every
object it creates and prints the observed values next to the expected ones.

## 8. Confidence summary

| claim | confidence | basis |
|---|---|---|
| cube A18/A00..A05/face order | high | manual, static, cross-driver |
| cube single-BO face stride formula | high | static allocator chain |
| cube uses TEXLD (op 1), not TEXLDP | high (compiler) | dynamic |
| cube FE A01/A08 three-lane form | medium | inference from the manual dimension field |
| cube orientation matches GL | medium | static (no permutation), unmeasured |
| DXT A30 bytes, A20 texel units | high | manual, static |
| DXT pitch/rows/packing | high | static |
| POT levels derived by hardware, table ignored | high | silicon GL mip smoke, section 10 |
| strip/fan A20 words | high | static; fan silicon |
| strip winding and FlatC provoking vertex | medium | manual cycle decode; unmeasured |
| no oBFD transport on VX900 | medium | manual deltas, all silicon variants fail |
| two-pass split correctness under the listed conditions | high | GL ordering rules; culling silicon-proven |

## 9. Implementation patches (cube maps, DXTn)

Status: 2026-09-24, rebased onto the POT derived-level layout of section
10.  Prepared and host-checked, not applied to the source trees and not
run on silicon.  Everything stays behind the second capability word and
default-off kernel module parameters, so old userspace on a new kernel and
new userspace on an old kernel keep today's behaviour.

### 9.1 Files and base

```text
~/tmp/agent-cube/rebase/kernel-cube-dxt.patch  -p1 from linux-7.2/, includes the UAPI
~/tmp/agent-cube/rebase/mesa-cube-dxt.patch    -p1 from mesa-26.2.2/, includes include/drm-uapi
~/tmp/agent-cube/rebase/{kbase,knew}/          kernel base (HEAD) and patched copy
~/tmp/agent-cube/rebase/{mbase,mnew}/          Mesa base (HEAD) and patched copy
~/tmp/agent-cube/rebase/mkpatch.sh             patch generator (function names in hunks)
~/tmp/agent-cube/rebase/host/                  host harness of the kernel self-tests
~/tmp/agent-cube/rebase/rung/                  ioctl capture shim (fakedrm.c), captures
~/tmp/agent-cube/rebase/rung/orig/             rung tool before this revision
```

Base: kernel `drivers/gpu/drm/chrome9` HEAD `4f9c199` (after `9dd8151`
POT levels and the native strip/fan topology, which took
`CAP2_VERTEX_TOPOLOGY` = bit 5) with the tree's
`include/uapi/drm/chrome9_drm.h`; Mesa `src/gallium/drivers/chrome9` HEAD
`4e4d49d` (after `50b6120`, carrying `capabilities2` and
`chrome9_screen_has_capability2()`).  The former caps2 plumbing part is
dropped; each area is one patch.  Both pass `git apply --check` against
the current trees and reproduce the patched copies exactly when applied
to the HEAD copies.  The copies of the first rebase onto
`9dd8151`/`50b6120` are kept as `*-9dd8151`/`*-50b6120`.
Every touched file carries only `Copyright 2026 René Rebe
<rene@exactco.de>`.  The earlier two-part patches in `~/tmp/agent-cube/`
are superseded.

### 9.2 UAPI

| item | value |
|---|---|
| `DRM_CHROME9_CAP2_CUBE_MAP` | bit 1: texture state `target = DRM_CHROME9_FRAGMENT_TARGET_CUBE` (1) |
| `DRM_CHROME9_CAP2_DXT` | bit 2: formats `DXT1_RGB` 7, `DXT1_RGBA` 8, `DXT3` 9, `DXT5` 10, level zero |
| `DRM_CHROME9_CAP2_DXT_MIP` | bit 3: POT DXT mip chains in the derived layout (9.3) |
| bit 4 | unused; the former `DXT_MIP_ALIGNED` (table-driven DXT chains) is dropped |
| bit 5 | `CAP2_VERTEX_TOPOLOGY` (main tree, not this patch) |
| `drm_chrome9_fragment_texture_state` | `reserved[4]` becomes `target`, `face_stride`, `reserved[2]`; size stays 176 |

`level_offset[]`/`level_pitch[]` describe face 0 of a cube; face `f` of
level `L` is at `level_offset[L] + f * face_stride`.  DXT `width`/`height`
stay in texels, `level_pitch` is the byte pitch of one block row
`ALIGN(DIV_ROUND_UP(w, 4) * block, 32)` (vendor `ALIGN(ALIGN(w, 4) * bpp /
2, 32)`).  Bit 3 stays separate from bit 2 so that a kernel can keep level
zero if rung `dxt m1` rejects the chain layout.

### 9.3 Layout rules

| texture | level placement | level table (subtype 10) | A22 POT flag |
|---|---|---|---|
| 2D POT, uncompressed | `offset(L+1) = offset(L) + ALIGN(w*cpp, 32) * h` (section 10) | zero words | set |
| 2D POT, DXT | `offset(L+1) = offset(L) + blockrow_pitch * ceil(h / 4)`, no row padding | zero words | set |
| 2D NPOT, uncompressed | 256-byte aligned, ascending | offsets | clear |
| 2D NPOT, DXT | level zero only | none | clear |
| cube (square POT) | vendor face layout `pitch * ALIGN(h, 8)`, 256-byte aligned | offsets | clear |

The DXT row is the uncompressed POT rule with block rows as rows; it is the
only DXT mip layout (the `dxt_mip_layout` switch is gone).  **Open: whether
the sampler pads block rows of small levels (for example to two block rows,
the compressed analogue of `ALIGN(h, 8)`) is not measured; rung `dxt m1`
decides it before `dxt=1` becomes the default.**  A DXT1 32x32 chain lies
at `0, 0x200, 0x280, 0x2c0, 0x2e0, 0x300`; a padding model would move every
level after the 8x8 level.

Cube maps are exempt from the POT rule in validation and in
`chrome9_fragment_mip_state()`: they keep the table and the face stride of
section 2.2 until rung `cube c2` shows whether the sampler also derives cube
levels (then cube levels would follow the 2D POT rule per face).

### 9.4 Kernel

Module parameters, read-only and default off:

| parameter | effect |
|---|---|
| `cube_map=1` | advertises `CAP2_CUBE_MAP`, accepts cube states, adds the A01..A05 closure to every texture image packet |
| `dxt=1` | advertises `CAP2_DXT` and `CAP2_DXT_MIP`, accepts formats 7..10 |

* `chrome9_fragment_levels_derived()`: POT at `first_level` and not a
  cube; shared by `chrome9_fragment_mip_state()` (zero table words) and
  `chrome9_validate_fragment_texture()` (exact successor offsets).
* `chrome9_validate_fragment_texture()` takes the CAP2 feature set and
  returns per-texture emission flags (`CHROME9_TEXTURE_FACE_CLOSURE`),
  carried to `chrome9_emit_fragment_texture()` by the draw request.
* Cube: needs `CAP2_CUBE_MAP`; square POT, uncompressed, not X8Z24,
  `last_level - first_level <= 8`, CLAMP_TO_EDGE on S and T, `face_stride`
  non-zero, 256-aligned and at least `ALIGN(end of face 0 - L0, 256)`,
  object size `ALIGN(L0 + 6 * face_stride, PAGE) + PAGE`, levels 256-byte
  aligned.  Emission: A00 plus A01..A05 = `n << 24 | face_root[n] >> 8`,
  A18 `18015355`, A22 without the POT bit, table and sampler.
* 2D with `cube_map=1`: A01..A05 = `0n000000` and A18 `18001101` on every
  image packet (closure, manual M:6939).  With `cube_map=0` all existing
  packets are byte-identical to HEAD.
* DXT: format bytes `29` (both DXT1), `2a`, `2b`; A20 in texels; mip
  chains need `CAP2_DXT_MIP` and POT, placed by the derived rule.
* Fragment compiler: `chrome9_fragment_compile_targets()` with a
  `cube_mask`; a direct varying coordinate of a cube TEX notes lanes S, T,
  R (FE A01 `01002522`, A08 `08010201`, TAU `301001b0`, form B of 2.3).
* Self-tests: the section 2.2 cube vector (32x32 BGRX, face stride
  `0x1500`, table `40000010 40000014`), six cube rejections, a cube 8x8
  chain that the POT rule would place at `+0x180` but the cube rule needs
  at `+0x200` (table `40000001 40000002`), the 2D closure packet with zero
  table words, DXT1 32x32 six levels (`30290000`, rejected without
  `CAP2_DXT_MIP`, rejected with L1 at a 256-byte slot), NPOT DXT level zero
  accepted and two levels rejected, DXT5 64x64 (`302b0000`); the cube lane
  widening in `chrome9_fragment_cube_selftest()`.

Host verification: the out-of-tree `make M=... CONFIG_DRM_CHROME9=m W=1
modules` build of the patched copy is warning-free.
`chrome9_fragment_texture_selftest()` (extracted with its helpers into
`host/tex_extract.c`) and `chrome9_fragment_selftest()` (whole
`chrome9_fragment.c`) run on the host against a small kernel shim with
ASan/UBSan and return 0; four mutated expectations (cube table word, DXT
256-byte slot, DXT sampler word, DXT offsets) each fail, so the new vectors
execute.

### 9.5 Mesa

* `chrome9_format.c`: `DXT1_RGB` (lanes `D0 D1 D2 ONE`), `DXT1_RGBA`,
  `DXT3_RGBA`, `DXT5_RGBA` with the block size in `bytes_per_pixel`;
  `chrome9_format_is_dxt()`, `chrome9_format_block_dim()`,
  `chrome9_format_level_pitch()`, `chrome9_format_level_rows()`.
* `chrome9_resource.c`: `PIPE_TEXTURE_CUBE` (six layers, square POT, at
  most nine levels) in one guarded BO with the vendor face layout
  (`pitch * ALIGN(h, 8)` per level, `layer_stride = ALIGN(sum, 256)`).  DXT
  uses the HEAD 2D layout (POT levels back to back, `level_size = pitch *
  block rows`); DXT mip chains need `CAP2_DXT_MIP` and POT, NPOT DXT is
  level zero only.  S3TC is exposed through `is_format_supported()` with
  `CAP2_DXT` and `CAP2_DXT_MIP`.  Transfers address layers and whole
  blocks; CPU mipmap generation walks cube layers and leaves DXT to Mesa.
* `chrome9_screen.c`: `max_texture_cube_levels = 9` with `CAP2_CUBE_MAP`.
* `chrome9_context.c`: `GLSL_SAMPLER_DIM_CUBE` TEX lowers to a TEX reading
  XYZ; the shader records `semantic_cube_sampler_mask`; cube sampler views
  cover all six faces.
* `chrome9_draw.c`: cube and DXT textures only on the generic fragment
  path; `target`/`face_stride` in the texture state, CLAMP_TO_EDGE for
  cube, DXT1 RGB alpha = 1 through the fixed-swizzle composition.
* `chrome9_swfb.c`: the llvmpipe fallback copies cube layers and block
  formats.

Host verification: all 30 chrome9 objects of `objs-chrome9-llvm`
compile warning-free from the copy (`scripts/mesa_cc.py`);
`chrome9-format-test` (new: the DXT1 32x32 chain offsets of 9.3, mutation
fails) and `chrome9-fragment-test` pass.  `chrome9-mipmap-test` does not
link in this environment (`libLLVM-23.so` needs `cosh@GLIBC_2.44`).

### 9.6 Rung tool

`~/tmp/agent-features/chrome9_next_oracle{,_tex}.c` build against the
rebased UAPI (`include/chrome9_drm.h` = `knew`); originals in
`~/tmp/agent-cube/rebase/rung/orig/`.  Changes:

* `qualify` (new, run first): a constant-colour `DRAW_FRAGMENT_PROGRAM`
  triangle (as `tools/re/chrome9_fragment_smoke.c`) and, with `hw_tcl`, the
  split/twoside reference TCL triangle, each with implicit and explicit
  CPU access, printing ioctl results, inside/outside pixels and guards.
* Surface reads: `GEM_CPU_ACCESS WAIT | PREPARE_READ` once per surface
  after each draw (not per pixel) and its error is printed; surfaces use
  Mesa's explicit protocol (`GEM_CPU_EXPLICIT` after the CPU fill) unless
  `CHROME9_ORACLE_ACCESS=implicit`.
* Post-TNL rung vertices use `z = 0.5` like every guard smoke.
* `topo` uses the landed `topology` field and `CAP2_VERTEX_TOPOLOGY`
  (the proposed `reserved[0]`/`abi_version = 2` form is gone).
* `dxt p0` now places L1 at the end of L0 (the only layout the kernel
  accepts since `9dd8151`) and expects red; `dxt m1/m2` need `dxt=1`
  (`CAP2_DXT_MIP`); `m3` and the aligned layout are removed.

No-draw report (every draw left `deadbeef`, ioctls 0, also with the WAIT
added): review result.

1. The requests are correct.  An `LD_PRELOAD` capture
   (`rung/fakedrm.c`) of `split s1` and `tools/re/chrome9_tcl_oracle
   twoside t0` shows byte-identical `DRAW_VERTEX_PROGRAM` requests (all
   fragment, vertex, element, vertex-data and constant words; only user
   pointers differ), same object sizes (`0x6000`) and the same order.
   Width, height, pitch (256), destination size, scissor (disabled),
   blend/colour mask (zero, BLEND clear), depth flags, viewport and the
   clip-space vertex convention all equal the known-good tool;
   the `dxt p0` and `qualify` fragment requests pass every check of
   `chrome9_fragment_draw_prepare()` (by inspection).
2. The version without WAIT read the target directly after the ioctl.
   Since `85ad597` the last segment retires asynchronously and an implicit
   object is invalidated only at retirement, so those reads race the GPU
   and see the CPU fill.  `chrome9_tcl_oracle` reads the same way, so it
   has the same race on a kernel with `85ad597`; its recorded passes
   (twoside ladder, `618b776`) were on earlier modules.
3. The WAIT version ignored the ioctl result.  `WAIT` returns 0 only after
   the job's fence signaled, and retirement flushes implicit destinations
   before signaling, so a successful WAIT followed by `deadbeef` is not
   explained by the kernel sources reviewed.  A failed WAIT is: `-EINVAL`
   from a module without the `WAIT` flag (for example a working-tree build
   with async retirement from before `85ad597`) or `-ETIMEDOUT` for a job
   that did not retire.  The tool now prints this.  Unresolved until
   `qualify` runs on silicon (record the module Build ID and dmesg delta).

### 9.7 Silicon gates before enabling

| feature | rungs | must show | then |
|---|---|---|---|
| all | `qualify` | both fragment and TCL qualifiers draw, no CPU sync failure | only then interpret any rung |
| POT regression | `dxt p0` | DERIVED L1 (red) | |
| cube | `cube c1` | 24 face/quadrant colours exact: form B samples, face order and orientation match GL | a permutation or mirror is fixed in the kernel face order or Mesa's coordinate lowering; no colour means form B is wrong |
| cube | `cube c2` | level colour on every face with the shared table | if the table is ignored, cube levels move to the 2D POT rule per face |
| cube | `cube c3` | 2D quadrants exact after a cube draw (closure) | |
| cube | `cube c4` | identical to `c1` with scaled directions | make `cube_map` default on |
| DXT decode | `dxt d1..d6` | exact block decode, texel order, three-colour mode, DXT3/DXT5 alpha | make `dxt` default on for level zero |
| DXT mips | `dxt m1`, `m2` | level colours L0..L5 at the unpadded block-row offsets | keep `CAP2_DXT_MIP`; otherwise switch to the padding model the colours show |

### 9.8 Open questions

1. Cube form A (TEXLDP, Q = 1, four-lane consumer) is not implemented; if
   `c1` samples nothing, the consumer cap for the cube coordinate and TAU
   operation 7 need a follow-up patch.
2. With `cube_map=1` every 2D image packet grows by five dwords (the
   closure); `c3` shows whether the plain form suffices.
3. NPOT S3TC mip chains fail to allocate (`GL_OUT_OF_MEMORY`) instead of
   falling back; POT content (ioquake3) is unaffected.
4. Compressed cube maps are not accepted.
5. Cube LOD is assumed to use the projected face coordinates; the rungs
   force the LOD.
6. DXT block-row padding (9.3), decided by `dxt m1`.

## 10. Silicon result: POT level derivation (2026-09-24)

Measured through GL (`tools/re/chrome9_gl_mip_smoke.c`, 10 POT shapes from
64x64 to 256x16 and 8x128, every level): the sampler derives POT levels and
ignores the subtype-10 table (the old layout with a guard page after L0
read the guard page for L1).  The derived pitch is `ALIGN(w * cpp, 32)` and
the level height is the exact height, not `ALIGN(h, 8)`: with 8-row padding
every level after a 4-row level failed, with 4-row padding every level after
a 2-row level failed, with exact rows all 68 levels matched on both vertex
paths.  This is the DERIVED outcome of `dxt p0` with a correction to
section 3.3: POT levels are packed at 32-byte granularity with no row
padding, so they are not 256-byte aligned in general.  Kernel and Mesa now
use this layout for POT textures; NPOT textures keep the table.  Cube maps
still need their own rung (`cube c2`) before any layout is assumed.

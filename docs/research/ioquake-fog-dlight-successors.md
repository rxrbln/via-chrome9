# Chrome9 ioquake3 fog and dynamic-light successors

Status: implementation-grade source/state/packet audit, 2026-09-21. The
fragment programs, one-texture MODULATE packet, read-only D16 control and all
required blend factors are independently recovered; LESS/LEQUAL read-only is
live. The remaining common gate is D16 EQUAL. Capability bit 32 and the
EQUAL live sequence below are reserved design, not advertised implementation.

## Exact renderer passes

The GL1 backend emits two renderer-owned passes after an opaque surface:

| pass | texture and primary | depth | blend |
|---|---|---|---|
| additive dynamic light | `dlightImage * vertex light color` | EQUAL, read-only | ONE,ONE |
| filter dynamic light | `dlightImage * vertex light color` | EQUAL, read-only | DST_COLOR,ONE |
| ordinary fog | `fogImage * constant fog color` | LEQUAL, read-only | SRC_ALPHA,INV_SRC_ALPHA |
| `FP_EQUAL` fog | `fogImage * constant fog color` | EQUAL, read-only | SRC_ALPHA,INV_SRC_ALPHA |

This is direct source behavior, not an inferred material pattern.
`ProjectDlightTexture_scalar()` builds two-float S/T and RGBA vertex color,
binds `tr.dlightImage`, and selects the two dynamic-light rows at
`renderergl1/tr_shade.c:407-556`. `RB_FogPass()` builds two-float S/T, fills
every vertex with `fog->colorInt`, binds `tr.fogImage`, and selects the two fog
rows at `:578-604`. Neither pass sets `GLS_DEPTHMASK_TRUE`; ioquake's state
manager consequently leaves D16 writes disabled. `GLS_DEPTHFUNC_EQUAL`
selects EQUAL, while its absence retains the renderer's LEQUAL default.

The ordering relative to the captured world-lightmap key is exact.  All
material stages finish first.  `RB_StageIteratorLightmappedMultitexture()`
then disables texture unit 1 and reselects unit 0.  If the surface has a
matching `dlightBits` entry and sorts at or before opaque, it calls
`ProjectDlightTexture()` once per affecting dynamic light.  Fog, when
present, runs only after every dynamic-light pass.  The generic and
vertex-lit iterators use the same material -> dlights -> fog order:

```text
base/lightmap draw
  -> zero or more dlightImage * per-vertex-color EQUAL/read-only draws
  -> optional fogImage * fog-color LEQUAL-or-EQUAL/read-only draw
```

`ProjectDlightTexture_scalar()` does not geometrically clip triangles.  It
computes S/T for every original vertex, marks six outside half-spaces, and
keeps a triangle unless all three vertices share an outside bit.  Retained
vertices can therefore have finite S/T outside `[0,1]`; clamp-to-edge is part
of correctness.  It emits a new `hitIndexes` triangle list, not a strip or
fan, and every per-vertex color has alpha byte `ff`.  The existing Mesa draw
module may deindex and submit individual triangles, while a future run ioctl
must preserve independent three-vertex list assembly.

The generated resources are unusually convenient bounded inputs:

* `R_CreateDlightImage()` creates a 16x16 clamp-to-edge, non-mip image with
  RGB falloff and alpha 255. `Upload32()` therefore chooses an RGB internal
  format on the default non-greyscale/non-S3TC Chrome9 path, so Gallium sees
  XRGB and sampled alpha is one.
* `R_CreateFogImage()` creates a 256x32 clamp-to-edge, non-mip image with
  white RGB and varying fog-factor alpha. `Upload32()` necessarily chooses
  RGBA, so Gallium sees BGRA and must preserve sampled alpha.
* both images use LINEAR minification and magnification because they have no
  mip flag. Their POT dimensions, canonical level-zero pitch, clamp wrap and
  level-zero-only views already fit the bounded texture resource contract.

The fog primary alpha is exactly one: BSP setup uses
`ColorBytes4(fog.rgb * identityLight, 1.0)`. Dynamic-light color alpha is
explicitly 255. Thus both paths use normalized D1 and the existing
perspective-correct MODULATE raster state; no fragment constant is involved.

## Exact NIR keys and Q boundary

Mesa fixed-function linking retains the projector even though ioquake passes
two-component coordinates. The two expected closed forms are:

```text
dynamic light (RGB texture base format):
  t = tex(unit 0, coord=TEX0.xy, projector=TEX0.w)
  p = COL0
  out = vec4(t.rgb * p.rgb, p.a)

fog (RGBA texture base format):
  t = tex(unit 0, coord=TEX0.xy, projector=TEX0.w)
  p = COL0
  out = t.rgba * p.rgba
```

They map respectively to the already-defined
`CHROME9_FS_TEXTURE_PROJECTED_PRIMARY_RGB_ALPHA` and
`CHROME9_FS_TEXTURE_PROJECTED_MODULATE` keys. The first is exact through the
native full-RGBA MODULATE object only because the dlight view is XRGB and
therefore supplies sampled alpha one. The second must accept BGRA and retain
its alpha for source-over fog. Both lower to
`DRM_CHROME9_TEXTURE_PROGRAM_MODULATE`; A01 is `01ce1548` for read-only D16,
A02 remains normalized `020041d1`, and the PS is the live two-ALU object:

```text
A92 92010002
TAU 301001b0
ALU 0140b6d0 30040053 00f20005 0000007f
    01c00000 301c0000 00f20105 000d083f
```

No native projective state is needed for ioquake GL1. Every relevant source
call uses `qglTexCoordPointer(2,...)`, `qglTexCoord2fv`, or
`qglMultiTexCoord2fARB`; there is no three-/four-component coordinate call in
the renderer. OpenGL fills missing R/Q as 0/1. The fog-coordinate source even
states that true projected coordinates would be more correct for a clipped
fog plane but deliberately computes an approximate two-float S/T instead.

Mesa must nevertheless inspect post-TNL Q and require raw
`0x3f800000` before dropping it. A non-unit Q never falls into this ioquake
fast path. It remains rejected until the separately documented STRQ/FVF/VS
and TEXLDP packet is live; changing TAU alone is unsafe. S/T may extend beyond
`[0,1]` for partially intersecting dynamic-light triangles, so validate them
as finite and safe but do not impose a normalized-range restriction. The
clamp sampler provides the intended edge behavior.

## Exact immediate vertex and shader object

The dlight and fog passes reuse the current one-texture post-TNL layout.  No
new vertex-program selector is needed.  The packet must retain this complete
object:

```text
config       00000011 01002521 08010101 05000000 0d810000
FVF          00000308 010000c2 020041d1 030052d0
             04006342 1200000f 61800000 623081ff
             20000000 23004f02 24000003 25f6cc00
             28ffcc00 40800000 41400000 4200493f
             43800000 44400000 4500493f 46800000
             47400000 4800003f 49000000 50000000
             51000000 52147fff
VS           00380150 00000000 00000000 0008010f
             0008014c 00100150 00384154 0004021f
             0008414c 00000000 00000000 0008422f
             0008814c 00000000 00000000 0008423f
             0008c14c 00000000 00000000 0008028f
             0009014c 00000000 00000000 0008029f
VS control   60800004
```

Each immediate vertex is eight dwords: transformed D0 `(x,y,z,rhw)`,
normalized packed D1 primary color, packed D2 specular, and D3 `(s,t)`.
`A02=020041d1` is mandatory; D0 would feed unnormalized 0..255 dlight color
and saturate modulation.  D3 has only S/T because OpenGL fills missing R/Q
with 0/1 and Mesa proves Q one before removing the NIR projector.

The fragment object is equally closed:

```text
A90..A9a 90018000 91000001 92010002 93000001
          94000000 95000000 96000000 97000000
          98000000 99000000 9a000000
TAU       301001b0
ALU       0140b6d0 30040053 00f20005 0000007f
          01c00000 301c0000 00f20105 000d083f
```

The image A30 format, not another shader object, supplies the base-format
alpha rule: dlight format 98 makes full RGBA MODULATE equivalent to the live
`vec4((texture*primary).rgb,primary.a)` NIR; fog format 99 preserves sampled
fog alpha.  After a two-texture world draw, the command-12 packet's one image,
one sampler, A93 `...1`, one TAU and two-ALU object must replace the complete
two-unit state.  Validate transition `two-texture -> dlight -> two-texture`
without a context reset.

## Exact state words

Both use the 236-dword one-texture MODULATE form of private command `0x12`.
The blend/D16 layout is already complete: A33 is body 19, A00 is body 47,
A01 body 48, D16 A10--A20 body 51--55, PS/TAU/ALU remain in their established
blocks, and the immediate vertex H2 starts at body 208. Only blend words and
A13 vary.

| pass | A34 | A35 | A36 | A37 | A13 |
|---|---:|---:|---:|---:|---:|
| additive dynamic light | `34000150` | `35000154` | `36000590` | `37000264` | `13020000` |
| filter dynamic light | `34000010` | `35000154` | `36000420` | `37000264` | `13020000` |
| ordinary fog | `34000020` | `35000148` | `36000410` | `37000244` | `13030000` |
| `FP_EQUAL` fog | `34000020` | `35000148` | `36000410` | `37000244` | `13020000` |

For all rows A53 is `53010c0f`, blend-enabled A00 is `00000011`, A01 is
`01ce1548`, fixed alpha test is disabled with `A33=33007000`, and destination
A52 independently selects XRGB 98 or BGRA 99. The table preserves ioquake's
single `glBlendFunc` for RGB and alpha. Do not substitute canonical
separate-alpha source-over: on BGRA, fog alpha is
`src.a*src.a + dst.a*(1-src.a)`.

The filter dynamic-light row uses extended factor value 8 and therefore
requires cap 29. Additive and fog factors are in the baseline set. All rows
require cap 27 for command 12, cap 23 for read-only and cap 15 for explicit
depth-function flags. Partial scissor remains orthogonal and uses cap 11;
alpha test is disabled in these renderer-owned passes.

### Complete bounded command-12 request

The exact dlight request uses private command `0x12` with:

```text
program             DRM_CHROME9_TEXTURE_PROGRAM_MODULATE
flags               DRM_CHROME9_DEPTH_FUNC(EQUAL) |
                    DRM_CHROME9_DEPTH_READ_ONLY        # 00000a00
texture             16x16, pitch 64, XRGB8888
min/mag             LINEAR / LINEAR
wrap S/T            CLAMP_TO_EDGE / CLAMP_TO_EDGE
destination         guarded XRGB8888 or actual guarded BGRA8888
depth               distinct guarded D16, canonical pitch
blend.flags         DRM_CHROME9_BLEND_ENABLE
blend.color_mask    RGBA
alpha test          disabled
vertices            three independent post-TNL records
```

The additive request selects factors ONE/ONE for both RGB and alpha.  The
filter request selects RGB `DST_COLOR,ONE`; its alpha factors are
`DST_ALPHA,ONE`, which are the normalized result of ioquake's single
`glBlendFunc`.  Do not replace the latter with destination-color factors in
the alpha equation or with `DST_COLOR,ZERO`, which belongs to the material
lightmap/filter stage.

No new shader-program capability or ioctl is justified.  Required gates are
cap 27 (command 12), cap 23 (read-only), cap 15 plus cap 32 (explicit EQUAL),
cap 29 for the filter dlight factors, cap 9 when the destination is BGRA, and
cap 11 only when scissor is enabled.  Kernel vertex validation should retain
finite S/T without a `[0,1]` restriction; the current structured one-texture
path already has that property.

## Minimal EQUAL capability

Reserve the next free semantic capability without adding an ioctl or changing
any request layout:

```c
#define DRM_CHROME9_CAP_DEPTH_FUNC_EQUAL (1ULL << 32)
```

The request spelling is already public:

```text
flags = DRM_CHROME9_DEPTH_FUNC(DRM_CHROME9_COMPARE_EQUAL)
      | DRM_CHROME9_DEPTH_READ_ONLY
      = 00000a00
```

Capability 15 continues to mean the depth-function field exists and currently
covers LESS/LEQUAL; bit 32 says EQUAL is additionally accepted. This keeps old
kernels distinguishable without another command or an optimistic ioctl probe.

The kernel patch is deliberately small:

1. add bit 32 to all three UAPI copies and the GET_PARAM capability mask;
2. make `chrome9_validate_depth_flags()` accept `COMPARE_EQUAL` in addition
   to LESS/LEQUAL, preserving zero as legacy LESS and every unknown flag as
   invalid;
3. retain all existing per-ioctl requirements and object handling; builders
   already emit `0x13000000 | func<<16`, so EQUAL becomes `13020000` with no
   index or length change;
4. extend every structured D16 path through the common validator, rather than
   creating a command-12-only compare dialect; and
5. keep the D16 reservation/fence WRITE even when bit 11 selects read-only.

The Mesa patch adds an `equal_depth_func_supported` Boolean from cap 32,
admits `PIPE_FUNC_EQUAL` only with that Boolean, and reuses the existing
generic depth-flag packer. No shader, sampler, vertex or ioctl dispatch change
is required. Dynamic light additionally needs the command-12 blend path and,
for DST_COLOR/ONE, extended factors; fog LEQUAL/read-only can run before the
EQUAL tranche and is the lowest-risk live precursor.

## Guarded live order

1. **Raw EQUAL compare.** Prefill D16 everywhere with `8000`; draw three
   disjoint read-only regions at incoming `4000/8000/c000` with distinct
   colors. Only the equal region changes color and every depth word stays
   `8000`. Run `LEQUAL -> EQUAL -> LEQUAL` without reset and check target/depth
   guards, cookie, CR PAUSE, work mask and MMIO fault state.
2. **Fog LEQUAL precursor.** BGRA texel `80ffffff` times white-alpha primary
   RGB `204080` over XRGB `00c08040` with SRC_ALPHA/INV_SRC_ALPHA must yield
   approximately `00706060` (one LSB tolerance), while equal/near depth passes
   and far depth fails. D16 remains byte-identical.
3. **Fog EQUAL.** Repeat at incoming `4000/8000/c000`; only equal contributes.
   Transition to ordinary LEQUAL fog and back to prove A13 closure.
4. **Additive light.** With XRGB white texture, primary `ff402010`, target
   `00201008` and equal depth, ONE/ONE must produce `00603018`; near and far
   remain unchanged and D16 stays `8000`.
5. **Filter light.** With source `00808080` and destination `00204080`,
   DST_COLOR/ONE must produce approximately `003060c0`. Follow with ONE/ONE
   and then repeat, proving cap-29 factor and A34 closure.
6. **Q rejection.** The same Mesa shaders with one post-TNL Q bit-pattern
   changed to 2.0 must increment rejected without an ioctl. Restore Q=1 and
   require the next draw to submit. Do not use this as a native TEXLDP test.

All texture, target and depth BOs are distinct and guarded. A timeout
quarantines all three mappings and the ring segment. Every stage ends with a
legacy opaque one-texture draw so EQUAL, read-only, blend and BGRA sampler
state cannot remain latent.

## Independent cross-checks

Chrome9 EQUAL is directly encoded as A13[18:16]=2 by Linux 5.75
`STM_ZFunc_INV`, Windows D3D `0x101faa10`, and the byte-equivalent Windows
OpenGL depth builder at `0x1024e6d0`; the complete compare table is recorded
in `fixed-depth-cull-stencil-contract.md`. Windows D3D blend builder
`0x101fd9e0`, OpenGL `0x10251220`, and Linux `STM_AlphaBlend_INV:0x196e30`
independently produce the four blend rows above. Mesa 7.11.2 UniChrome
`via_state.c` confirms the API compare/factor semantics and read-only policy,
but is not used as a Chrome9 packet source.

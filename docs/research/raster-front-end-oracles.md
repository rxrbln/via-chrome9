# VX900 raster and vertex-fetch front-end oracles

Status: static reverse engineering.  The packet deltas in this note are based
on the silicon-verified 188-dword H6 draw in
[vp-stall-analysis.md](vp-stall-analysis.md), but none of the new cull, depth
bias, edge-AA, clipping, or vertex-buffer/index deltas has yet been executed.
They must remain diagnostic oracles, not advertised Mesa capabilities, until
each passes guarded readback and idle checks.

The primary executable is `via_chrome9_dri.so`, build ID
`2652605384b383d1377e040b8f8173b856e9a067`.  Addresses below are ELF virtual
addresses in that image.  The Windows comparisons use the hashed DLLs listed
in [windows-draw-path.md](windows-draw-path.md).

## Culling and winding

The manual and `STM_UpdateCull_INV:0x196a70` agree on both participating
words:

| word | bit | meaning |
|---:|---:|---|
| type-01 A01 | 7 | enable back-face culling |
| type-04 A23 | 8 | culled winding: `0=CW`, `1=CCW` |
| type-04 A23 | 7 | winding which selects the VS `oBFD` back-face color; this is not the cull enable |

The routine changes hardware-image byte `+0x04[7]` for A01 and bytes
`+0x2148/+0x2149` for A23.  Its internal cull modes are exact: mode 1 clears
A01 bit 7; mode 2 sets A01 bit 7 and clears A23 bit 8; mode 3 sets both.
It updates A23 bit 7 from the API front-face field even while culling is off.

The verified draw has `01c01540` (culling off) and `23000f02` (CCW is the
selected cull winding but inactive).  Therefore the two one-bit cull states
which preserve every unrelated field are:

```text
# cull CCW
01c01540 -> 01c015c0
23000f02                  # unchanged; A23[8]=1

# cull CW
01c01540 -> 01c015c0
23000f02 -> 23000e02      # A23[8]=0
```

The first guarded discriminator should use the 184-dword Gouraud triangle
from [windows-draw-path.md](windows-draw-path.md), enable one winding, then
repeat with vertices 1 and 2 exchanged.  Exactly one image must survive.
This avoids assigning the hardware's screen-space CW name to the API's
front-face convention before the Y-axis direction and viewport sign are
tested.  Do not change A23 bit 7 in this test; that selects a color output and
would confound culling with varying linkage.

## Perspective interpolation and reciprocal W

Type-01 A01 has three independent correction enables: bit 17 diffuse color,
bit 18 specular color, and bit 19 fog.  They are not speculative manual-only
fields.  The proprietary Linux GL initializer
`stmInitChipImage_inv:0x1787c4-0x1787da` constructs A01 by ORing payload bytes
`ce`, `15`, and `40`, producing:

```text
01ce1540
```

Thus the GL default enables all three perspective bits; Xorg's affine
`01c01540` clears them.  A first generalized textured path should use the
vendor-exact GL word rather than changing only one bit.  Bit-17-only
`01c21540` is a valid field-table derivation but has not been vendor- or
silicon-qualified as a standalone combination.

Texture-coordinate correction is separate.  In type-02/FE A01
`01002521`, texture-0 bits 6:5 are `01`, perspective enabled; bits 4:2 select
texture A and bits 1:0 select two-dimensional S/T.  Gallium `draw` writes
`position[3] = 1 / clip_w` after viewport mapping, and the old Unichrome
driver likewise requests a four-component viewport position plus W whenever
texture or fog is present.  The H6 D0.w dword must therefore carry that finite
reciprocal W.  The current structured untextured ioctl discards W, which is
harmless only for untextured or equal-W affine tests.

The smallest discriminator uses the proven texture packet and an image with
distinct texels, but assigns different reciprocal-W values to the three
vertices.  Several interior pixels must match
`sum(lambda_i * attribute_i * rhw_i) / sum(lambda_i * rhw_i)`, not affine
`sum(lambda_i * attribute_i)`.  Equal-W R/G/B/W quadrants cannot distinguish
the two paths.  This change is state/vertex-only but does write the bounded
guarded render target, so it belongs after the exact affine texture oracle.

### Bit-exact varying-RHW REPLACE discriminator

The following direct structured-ioctl case reuses the already-live
top-right-half triangle and changes no packet address or format class.  Use a
64 by 64 BGRX8888 destination and an 8 by 1 BGRX8888 texture with canonical
32-byte pitch, nearest filtering, clamp-to-edge, and `REPLACE`.  Fill its
eight texels with distinct values; in particular use `00ffffff` at index 3
and `00ffff00` at index 6.  Submit these post-viewport vertices:

```text
       x_16_16  y_16_16  z_16_16  rhw bits  color     specular  s bits    t bits
v0 =   ffff8000 ffff8000 00000000 3f800000  ffffffff  ffffffff  00000000 3f000000
v1 =   003f8000 ffff8000 00000000 3e800000  ffffffff  ffffffff  3f800000 3f000000
v2 =   003f8000 003f8000 00000000 3e800000  ffffffff  ffffffff  3f800000 3f000000
```

Thus the positions are `(-0.5,-0.5)`, `(63.5,-0.5)`, and
`(63.5,63.5)`; reciprocal W is `1, 1/4, 1/4`; S is `0,1,1`; and T is
one half at every vertex.  At destination pixel `(48,16)`, treating integer
pixel coordinates as the sample points used by the established `-0.5`
rectangle convention, the barycentric weights are exactly

```text
lambda0 = 31/128, lambda1 = 64/128, lambda2 = 33/128.
```

Affine interpolation gives `S = 97/128 = 0.7578125`, hence nearest texel 6.
Projective interpolation gives

```text
S = ((64+33)/128 * 1/4) /
    (31/128 * 1 + (64+33)/128 * 1/4)
  = 97/221 = 0.438914..., hence nearest texel 3.
```

The expected pixel is therefore `00ffffff`; `00ffff00` diagnoses an affine
path or lost D0.w.  The result is robust to an alternative half-pixel sample
interpretation: it shifts the two S values to about `0.7656` and `0.4495`,
which still select indices 6 and 3.  Check several covered pixels at x=48,
plus every destination/source guard and source preservation.  Keeping v1 and
v2 at the same reciprocal W makes the expected S independent of y along this
vertical sample line.

This REPLACE test specifically proves D0.w and the texture-coordinate
perspective selector in type-02/FE A01 `01002521`.  It cannot distinguish
type-01 A01 `01c01540` from `01ce1540`, because REPLACE does not consume the
diffuse, specular, or fog interpolants controlled by the latter word.

To isolate type-01 A01 bit 17, repeat the same geometry and reciprocal-W
values with a uniform-white texture and `MODULATE`.  Set v0 primary color to
opaque black `ff000000` and v1/v2 to opaque red `ffff0000`.  At `(48,16)` the
perspective-correct red component is `255 * 97/221 = 111.9` (normally byte
`70`), whereas affine interpolation is `255 * 97/128 = 193.2` (normally byte
`c1`).  Allowing for subpixel and color-rounding details, a red byte near
`6f..74` proves diffuse perspective correction; `c0..c4` diagnoses affine
diffuse.  This second case tests type-01 diffuse correction, while the
REPLACE case above independently tests the type-02 texture-coordinate path.

## Edge anti-aliasing is not multisampling

`STM_EdgeAntialias_INV:0x194400` is a single-bit routine: it clears or sets
hardware-image A01 bit 5 and dirties that word.  With the verified A01 value,
the complete enable delta is:

```text
01c01540 -> 01c01560
```

A23 bits 11:9 independently select which triangle edges participate.  The
verified `23000f02` already has all three edge bits set, so it is a useful
edge-AA experiment without another state change.  A triangle should be drawn
over a nonmatching initialized target and edge pixels compared with AA off.

This facility must not be exposed as MSAA.  The VX900 manual contains no
sample-count, sample-layout, sample-mask, multisample-storage, or resolve
registers.  The Linux ICD advertises neither `GL_ARB_multisample` nor
`GL_EXT_framebuffer_multisample` and creates visuals with zero sample buffers.
Its `sfmSetSurfaceMultiSampleTech:0x0dcd40` only stores an opaque value at
surface `+0x1cc`; `sfmMultisampleOn:0x0b3c90` is a software predicate over
surface/manager flags and does not program sample state.

Windows provides an independent negative result.  `SetRenderState` subtracts
one from the public state number and indexes the byte dispatcher at
`VTGUModeDX32A:0x100caf00`.  Entries 160 and 161, corresponding to D3D9
`MULTISAMPLEANTIALIAS` (161) and `MULTISAMPLEMASK` (162), both contain dispatch
code `03`; jump-table entry 3 is `0x100cada9`, the common no-op/return path.
Initial Gallium support must therefore expose only sample count one.  A01 bit
5 may later become line/edge smoothing support after its coverage behavior is
measured, but it is not evidence for multisample resources.

## Clipping, color window, and scissor

There are three distinct mechanisms which must not be collapsed into one
Gallium scissor flag:

| mechanism | state | role |
|---|---|---|
| clip engine | A01 bit 11 | enables homogeneous primitive clipping |
| clip modifiers | A23 bits 23:22 | near-plane triangle-point and `W=0` behavior |
| integer windows | type-01 A80..A83 | color window and scissor rectangles |

The verified pretransformed XRender draw has A01 bit 11 clear.  Its exact
window words are:

```text
80000020 81000020 82000020 83000020
```

For every word, the high 12-bit field is top/left and the low 12-bit field is
bottom/right.  The Windows builder passes D3D RECT right/bottom values through
as exclusive ends: the words above mean `[0,32) x [0,32)`, not coordinates
ending at 31.  The manual limits each endpoint to 0..2048 even though the
storage field is 12 bits.  New state code must intersect the API scissor with
the drawable and viewport, clamp to that documented range, and reject an
empty intersection in software until empty-window behavior is measured.

A minimal guarded scissor discriminator keeps A80/A81 at the full 32x32
color window and changes only A82/A83 to `[8,24) x [8,24)`:

```text
82000020 -> 82008018
83000020 -> 83008018
```

Use the already-proven top-right-half triangle.  Pixel `(20,12)` is inside
both the primitive and scissor and must change.  Pixels `(28,4)` and `(28,12)`
are inside the primitive but outside the top and right scissor edges and must
remain at the prefill value.  Avoid the triangle diagonal and every rectangle
boundary in the checks.  This isolates the exclusive right/bottom convention
without changing viewport, clip-engine, cull, or shader state.

For a first normalized ABI, accept `minx,miny,maxx,maxy` as unsigned 32-bit
exclusive-end coordinates, intersect them with `[0,width) x [0,height)` in
the kernel, reject values above 2048, and turn an empty intersection into a
software no-op.  Userspace must not supply packed A82/A83 words.  Until this
discriminator passes, Mesa should continue its current full-target scissor
restriction rather than advertise general scissoring.

### Size-preserving scissor ABI route

Scissor is core state and has no Gallium screen capability which can hide it.
The current driver stores one `pipe_scissor_state`, but rejects every enabled
rectangle smaller than the whole target.  Mesa 26.2 has already normalized
the coordinates before that callback: `st_update_scissor()` intersects the GL
box with the framebuffer, uses exclusive `maxx/maxy`, converts Y to Gallium's
top-origin convention when the framebuffer orientation requires it, and
represents an empty box as four zeroes.  The Chrome9 driver must not apply a
second GL-origin inversion.

Every current immediate-draw UAPI happens to retain a 16-byte zero tail.  The
three legacy ioctls use `__u64 reserved[2]`; each blend successor uses a
four-byte `destination_format` followed by three reserved dwords.  A common
12-byte state therefore fits without changing any ioctl number or size:

```c
#define DRM_CHROME9_SCISSOR_ENABLE (1U << 0)

struct drm_chrome9_scissor_state {
        __u32 flags;
        __u16 minx;
        __u16 miny;
        __u16 maxx;
        __u16 maxy;
};
```

Use `scissor + __u32 reserved` for the original color, D16, and textured
tails.  Use `destination_format + scissor` for each blend successor.  This
also leaves `drm_chrome9_blend_state::reserved[2]` available for the planned
alpha-test state.  Old userspace supplies an all-zero tail and retains the
full-target default; new userspace must not set the enable flag unless a new
`DRM_CHROME9_CAP_DRAW_SCISSOR` bit says that **all** exposed immediate-draw
ioctls accept the state.  Per-ioctl partial support is not sufficient for a
core Gallium scissor contract.

Kernel validation should accept only the enable bit, require every endpoint
at most both the corresponding target extent and 2048, reject `min > max`,
and return successful no-op completion for either equal pair.  For a nonempty
enabled box synthesize only:

```text
A82 = 82000000 | (miny << 12) | maxy
A83 = 83000000 | (minx << 12) | maxx
```

Keep A80/A81 at the full color window.  When disabled, retain the existing
full-target A82/A83.  Mesa can copy the bounded state to each argument and
skip empty triangles before submission.  A first smoke should run the exact
`[8,24) x [8,24)` discriminator above through every available ioctl family,
then immediately run a disabled/full-target closure draw; this catches both
coordinate packing and stale state between successor and legacy paths.

On H5/H6 the exported Linux callbacks `STM_Clipping_H5:0x1bab30`,
`STM_ClipPlaneEnable_H5:0x1bab40`, and `STM_SetClipPlane_H5:0x1bab50` are all
immediate returns.  This is positive evidence that the driver does not obtain
user clip planes by toggling undocumented fixed-function state.  User planes
should be lowered into the VS/clip-distance path only after the shader ABI for
those outputs is recovered.  Merely changing the verified A01 word to
`01c01d40` is not a safe clipping oracle: the XRender VS and its unusual
C-to-screen constants were constructed for the clipping-disabled,
pretransformed path.

## Viewport and C-to-screen packing

Type-04 A40..A48 store three full IEEE-754 scale/offset pairs split across
24-bit words.  For float bit patterns `xs,xo,ys,yo,zs,zo`, construction is:

```text
A40 = 0x40000000 | (xs & 0x00ffffff)
A41 = 0x41000000 | (xo & 0x00ffffff)
A42 = 0x42000000 | ((xo >> 16) & 0x0000ff00) | (xs >> 24)
A43 = 0x43000000 | (ys & 0x00ffffff)
A44 = 0x44000000 | (yo & 0x00ffffff)
A45 = 0x45000000 | ((yo >> 16) & 0x0000ff00) | (ys >> 24)
A46 = 0x46000000 | (zs & 0x00ffffff)
A47 = 0x47000000 | (zo & 0x00ffffff)
A48 = 0x48000000 | (area_exp << 16)
                       | ((zo >> 16) & 0x0000ff00) | (zs >> 24)
```

`STM_ViewPort_INV:0x1bab80` independently performs exactly these split writes
at hardware-image offsets `+0x21bc..+0x21dc` and dirties A40 through A48.  In
the ordinary API path it reads viewport `x,y,width,height` from context
`+0x1e7c..+0x1e88` and depth range from `+0x1e8c/+0x1e90`.  It starts the X
scale calculation with `width * 0.5`; several render-target orientation,
YUV, shader-class, half-pixel, and depth-range branches then alter the final
six values.

The manual requires the same scale vector in VS constant c256 and offset
vector in c257.  Consequently A40..A48 cannot be changed alone.  The exact
XRender tuple remains the only silicon-proven one:

```text
40800000 41400000 4200493f
43800000 44400000 4500493f
46800000 47400000 4800003f

c256 = 3f800000 3f800000 3f800000 3f800000
c257 = 49400000 49400000 00000000 00000000
```

It decodes to scale `(1,1,1)` and offset `(786432,786432,0x00400000)`, not a
normal GL viewport.  The large XY offset is shader/linkage-specific.  A normal
GL viewport capture, including c256/c257, is still required before deriving a
general state formula; substituting the textbook viewport transform into only
the A40 block is unsafe.

A50/A51 plus A52 hold screen-coordinate upper/lower clamps and threshold.
The verified `50000000 51000000 52147fff` reconstructs upper `0x7f000000`,
lower `0xff000000`, and threshold byte `0x14`.  Preserve this tuple until an
isolated clamp test exists.

There is a separate pre-depth-test pixel-Z clip interval in type-01 A1E--A20.
Both proprietary viewport builders use the wide-open tuple:

```text
1E000000 1F000000 20007FFF
```

Here A1E/A20[15:8] reconstruct far `0x7f000000`, while A1F/A20[7:0]
reconstruct near `0xff000000`.  Linux emits it at
`via_chrome9_dri.so:0x1bb1e0..0x1bb292`; Windows emits it at
`VTGUModeDX32A.dll:0x101f8bb4..0x101f8c0e`.  The indexed-reset tuple has A20
`20000000`, hence near=far=zero.  It admits a Z=0 diagnostic but discards any
positive nonzero Z before the depth comparison.  Treat A20 `20007fff` as a
mandatory first-depth-draw prerequisite, not as an optional clipping feature.

The 2026-09-23 VX900 command-22 silicon check extends that requirement to
draws with depth testing disabled. A guarded 64x64 BGRA8888 target and the
one-ALU `MOV oC0,c0` program retired but changed zero pixels for a triangle at
Z=0.5 while the no-depth path omitted A1E--A20. Restoring the three exact
viewport-builder words on every generic draw produced 1176 changed pixels,
including the expected `ffff0000` interior; the exterior and guards remained
intact. The command used the compiler's counted H3 constant and ALU stores,
so this separately qualifies the ALU-only H3 upload. A later varying-source
draw still timed out and must be diagnosed independently.

Two subsequent single-change checks narrowed, but did not solve, the
direct-T0 failure. The VX900 manual defines FE A08 bits 3:0 as the number of
VS texture-coordinate outputs and bits 9:8 as T0's dimension. The original
generic packet declared zero outputs and 2D T0 despite exporting float4 T0.
Changing only the count to one (`08010101`), then changing both count and
dimension to the matching float4 tuple (`08010301`), each still timed out on
the same guarded `MOV oC0,T0` draw at the post-draw H2 flush. The ALU and
A90--A92 were byte-exact Linux production-compiler output; the color target
and vertex guards were not used to infer a successful draw. These two A08
contradictions are real state defects, but neither alone nor together is a
sufficient explanation for the direct-varying hang. The engine was
quarantined after each timeout and reset before the next test. Further GPU
submissions require an independently recovered producer/consumer linkage
tuple, not another unstructured field sweep.

That linkage is now recovered from the programmable Windows D3D path. In
`VTGUModeDX32A.dll:0x1015b255..0x1015b26b`, the draw update reads compiled
instance metadata `+0x34`, shifts it left four, and replaces FE A00 bits 7:4
in the hardware image at `+0x1c90`. The image initializer at
`0x101549a8..0x101549c0` proves that `+0x1c90` begins the consecutive FE
A00--A13 image. The same draw update reads the resulting A00 bits 7:4 at
`0x1015b3b4..0x1015b42e` and loops over that many coordinate dimensions to
rebuild the vertex-output masks.

Linux 5.75 production-compiler differentials identify `+0x34` exactly. It is
zero for `MOV oC0,c0` and `MOV oC0,v0`, one for direct `MOV oC0,T0`, one for
logical `MOV oC0,T5` after compaction to physical coordinate zero, and one
for `TEX(s0,T0); MOV oC0,r0`. The `T5` object's separate logical-use metadata
at `+0x40` is `0x20`, proving that the value copied into A00 is the compact
physical coordinate high-water rather than a logical mask, TAU count, or
bound-sampler count. A valid PS 2.0 `dcl T0` produces the same native ALU,
controls and `+0x34=1`; its declaration metadata is not a hidden program-store
word.

Therefore the generic direct-T0 packet's A00=`00000001` was incomplete even
though TAU count and sampler count were both zero. For dense generic linkage,
the source-backed rule is
`A00[7:4] = max(fragment_texture_count, program->varying_count)`, preserving
bit zero independently. One direct T0 requires A00=`00000011`. The runtime
does not synthesize an image, sampler, TAU instruction, or dummy texture BO;
only the FE coordinate count is extended. This explains why both A08-only
tests still hung and provides one bounded next correction, not a new register
matrix. The full differential and disassembly are recorded in
`chrome9-direct-t0-fe-count.md`.

The bounded correction passed on VX900 silicon on 2026-09-23.  With A00's
count changed to `max(texture_count,varying_count)`, the same guarded
64-by-64 semantic varying-zero/native-T0 MOV draw at Z=0.5 retired normally
and changed exactly 1176 pixels to the expected red, including the interior;
the exterior and both BO guards remained intact.  No image, sampler, TAU, or
texture BO was present.  This promotes the A00 coordinate-high-water rule
from binary proof to a live direct-fragment-input requirement.  It does not
by itself promote arbitrary varying counts or widths: A09/A0a state and the
three-through-eight input closures remain separate gates.

### Three-coordinate silent-black boundary

A guarded three-coordinate test on 2026-09-23 closes the next failure more
narrowly, but does not yet identify a state-field correction.  The semantic
fragment program was deliberately source-corrected so that every declared
input is consumed:

```text
MOV TEMP0, v0          # v0 = (0,0,0,0)
ADD TEMP1, TEMP0, v1   # v1 = (0,0,0,0)
ADD COLOR, TEMP1, v2   # v2 = (1,0,0,1)
```

All three inputs were float4.  The submitted image had coordinate high-water
three in A00, float4 T0/T1 dimensions in A08, and float4 T2 in A09.  The ioctl
and engine completed normally and both destination guards remained intact,
but the entire destination stayed black (`changed=0`).  This is a silent
no-color-write result, not the earlier direct-T0 timeout.

The Linux 5.75 production compiler, run offline on the equivalent PS 2.0
tokens with post-processing configuration 2, emits exactly:

```text
01c00530 301c0053 00f2c005 000d0020
02c10530 30140924 00f20005 00000060
02c20530 30140924 00f20105 00000820

90058007 91000000 92000003 93000000
94000000 95000000 96000000 97000000
98000000 99000000 9a000000 9b000000
```

The active-resource counts are `3 2 3 0`; compiled metadata `+0x34=3`,
`+0x40=7`, `+0x44=3`, `+0x6c=7`, and `+0x98=7` agrees that all three compact
coordinate inputs are active.  The current semantic compiler produces the
same three ALU records and A90--A9b controls byte for byte, including the
last-use bits for v0 in the first record, TEMP0/v1 in the second, and
TEMP1/v2 in the third.  This rules out an obvious source-register,
swizzle, lifetime-finalization, ALU-count, or program-control mismatch.

The submitted producer/raster closure was:

```text
# FE coordinate state
A00=00000031 A01=01002521 A08=08030303 A09=09000003 A0a=0a000000

# D3/D4/D5 float4 -> T0/T1/T2
A04=040063c2 A05=0500a4c2 A06=0600e5c2
A12=1200003f A25=25f6fff0 A26=26000000
A28=28fffff0 A29=29000000 A62=625081ff

# VS copies D3/D4/D5 to oT0/oT1/oT2; control declares six records
0008c14c 00000000 00000000 0008028f
0009014c 00000000 00000000 0008029f
0009414c 00000000 00000000 000802af
60800006
```

Those width/mask extensions follow the manual's type-04 definitions and the
same vendor FVF construction already recovered for five and eight coordinate
outputs.  Together with the byte-exact fragment object, no concrete mismatch
is presently visible in A00/A08/A09, the A25/A28 masks, the VS output copies,
the fragment source IDs, or A90/A92.

The static audit initially left multi-record H3 execution as an unqualified
boundary, although its `FE03043c`, count=`alu_count * 4`, `20030000`,
`cc000000` transaction already matched all three proprietary builders.  The
corresponding constant-only discriminator subsequently passed on VX900 silicon.
The exact three-record chain `MOV r0,c0; MOV r1,r0; MOV oC0,r1` produced
exactly 1,176 guarded opaque-red pixels with correct interior/exterior and no
engine error.  This eliminates counted-H3 count three, A92 count three, two
TEMP forwards, lifetime releases, and terminal COLOR execution as the cause
of the three-varying black target.

A direct `MOV oC0,T2` is not the correct next isolation: the proprietary and
current compilers compact a lone logical T2 use to physical coordinate zero.
The source-backed one-record discriminator which retains physical T0, T1 and
T2 simultaneously is instead:

```text
MAD oC0, T0, T1, T2

# Linux production configuration-2 output
02c20530 302c1053 00f2c005 00000820
A90..A92 = 90058007 91000000 92000001
```

Set T0 and T1 to zero and T2 to opaque red.  This preserves the exact
three-float4 FE/FVF/VS closure from the silent-black draw while reducing the
fragment program to one ALU and no TEMP.  A red result qualifies physical T2
producer/consumer linkage and narrows the original failure to the interaction
between multiple varying-consuming ALUs and their release schedule; a black
result isolates multi-coordinate/T2 linkage without H3 or TEMP ambiguity.
This next oracle is compiler-exact but has not been run, and no additional
GPU submission was made during this documentation update.

## Polygon offset / depth bias

The manual gives the exact equation:

```text
Zbias = max(Zdx, Zdy) * scale + offset
```

`STM_DepthBias_INV:0x1959d0` proves the bit packing.  A16 contains the low 24
bits of IEEE float `offset`; A18 contains the low 24 bits of IEEE float
`scale`; A17 contains offset's high byte in `[7:0]` and scale's high byte in
`[23:16]`:

```text
A16 = 0x16000000 | (offset_bits & 0x00ffffff)
A17 = 0x17000000 | ((scale_bits >> 8) & 0x00ff0000)
                       | (offset_bits >> 24)
A18 = 0x18000000 | (scale_bits & 0x00ffffff)
```

The API `factor` at context `+0x1de4` is copied bit-for-bit to `scale`.
`units` at `+0x1de8` is multiplied by `0x38000080` when the bound resource is
16 bits per pixel and by `0x34000001` otherwise before becoming `offset`.
The branch at `STM_DepthBias_INV:0x195a10` tests resource field
`+0x158 == 0x10`.  Its meaning is independently proved by
`sfmGetPatchColorKey:0xb6afc..0xb6b52`: values `8`, `16`, and `32` select a
byte, word, or dword load from the surface, respectively.  Thus D16 uses
`0x38000080` (approximately `3.0518044e-5`) and the 32-bpp D24S8/D24X8/D32
allocations use `0x34000001` (approximately `1.1920930e-7`).
Fill/line/point enables are independently selected by context
`+0x3c40/+0x3c44/+0x3c48` and current polygon mode `+0x1dfc`.

The chip initializer proves the adjacent clamp defaults:

```text
19800000 1a800000 1b803fbf
```

A19 and A1B low byte reconstruct `-1.0` (`bf800000`); A1A and A1B next byte
reconstruct `+1.0` (`3f800000`); A1B bit 23 enables enhanced Z precision.
These words are context prerequisites, not part of the three-word enable
delta.

For a direct hardware-packing discriminator independent of the unproved API
`units` conversion, `offset=-0.25`, `scale=0` is exactly:

```text
16800000 170000be 18000000
19800000 1a800000 1b803fbf
```

Use it only after the plain guarded D16 oracle succeeds.  Draw red at Z=0.5,
then an exactly overlapping green triangle at Z=0.5 with LESS and the negative
bias enabled only for green.  Green should replace red; without bias it must
not.  Starting at Z=0 is a bad discriminator because A12's negative-Z clamp
can erase the intended offset.

## Vertex streams and immediate indices

VX900 vertex-buffer draws use two independent packet classes:

1. H3 type-03 subtype `30` supplies up to 16 stream base/pitch dwords.
2. H2 type-04 A00..A10 describes up to 16 elements and chooses the memory
   domain; type-00 then carries immediate indices, not an index-buffer address.

For stream `s`, the subtype-30 palette entry is:

```text
stream[s] = (byte_address & 0xfffffc00) | (byte_pitch >> 2)
```

The base must be 1-KiB aligned and pitch is a nine-bit dword count.  Therefore
the directly encodable pitch range is 0..2044 bytes in multiples of four.
`stmUpdateVertexInput_H5:0x17cbb0` constructs this exact value.  Each element
A01..A10 stores stream ID in `[23:20]`, address bits 9:2 as dword offset in
`[19:12]`, VS input register in `[11:8]`, and the format in `[7:0]`.  A00 bits
16:15 select the global vertex memory domain; the Linux PCIe branch writes
`01` for surface memory type 4, i.e. SF.  A00 `[11:8]` is last used element
number and `[6:0]` is total vertex size in dwords.

The established declaration words already describe the verified six-dword
vertex record:

```text
00000206  # command mode: elements D0..D2, six dwords total
010000c2  # D0: stream 0, +0, IR0, float4
020041d0  # D1: stream 0, +16, IR1, packed color, unnormalized
030052d0  # D2: stream 0, +20, IR2, packed color, unnormalized
```

These are the byte-exact Xorg oracle words, not the correct general API color
declaration.  Format bit 0 is Normalize; the VX900 manual explicitly maps
D3DCOLOR to low byte `d1`.  A live source-alpha sweep confirmed the distinction:
with `d0`, alpha 00 remained zero but 40, 7f, and 80 all saturated to 1.0 in
the blend path.  Use `020041d1` and `030052d1` for normalized post-TNL COLOR0
and COLOR1.  The earlier opaque-primary tests used only channel values 00/ff
and therefore could not detect the mistake.

For an SF vertex buffer, only A00's domain field changes:

```text
00000206 -> 00008206
```

The stream-0 packet for a 24-byte stride and 1-KiB-aligned SF VA is:

```text
fe03043c 00000001 30030000 cc000000
<vb_sf_va | 00000006> cc000000 cc000000 cc000000
```

The low ten address bits of `vb_sf_va` must be zero.  For nonzero element
offsets across a 1-KiB boundary, the driver folds the high part into the stream
base and retains only bits 9:2 in the element; validation must reproduce that
address arithmetic rather than truncate it.

### Exact index mode and packing

`STM_SetIndices_INV:0x1bc250` writes A20 low bits as:

| input index size | A20 `[1:0]` | meaning |
|---:|---:|---|
| none | `00` | command/immediate-vertex mode |
| 2 bytes | `01` | indexed, 16-bit |
| 4 bytes | `11` | indexed, 32-bit |

It writes the 32-bit maximum and minimum permitted values to A2C/A2D low 24
bits and their high bytes to A2E `[15:8]/[7:0]`, respectively:

```text
A2C = 0x2c000000 | (max_index & 0x00ffffff)
A2D = 0x2d000000 | (min_index & 0x00ffffff)
A2E = 0x2e000000 | ((max_index >> 16) & 0x0000ff00)
                       | (min_index >> 24)
```

The H6 indexed triangle-list emitter
`__glS3InvDrawIndexedPrimitiveTriangleList_H6:0x21b020` proves the payload
ABI.  It emits A24 with the exact index count, then an H3 type-00 packet whose
dword count is `ceil(n/2)` for 16-bit indices or `n` for 32-bit indices, and
copies the API index bytes verbatim.  Thus two 16-bit indices occupy one
little-endian dword; there is no GPU index-buffer address.

For indices `{0,1,2}` the exact aligned bounds packet and 16-bit index tail
are:

```text
fe020440 0000043c 00040000 cc000000
2c000002 2d000000 2e000000 cc000000
fe03043c 00000002 00000000 cc000000
00010000 00000002 cc000000 cc000000
```

The second payload dword's high half is outside A24's three-index count and is
zero here deliberately.  The required state deltas from the 184-dword
Gouraud triangle are:

```text
00000206 -> 00008206      # vertex fetch from SF
201c5c00 -> 201c5c01      # fan, indexed, 16-bit
2c000002 2d000000 2e000000
```

Add the subtype-30 stream packet above, place the same three six-dword vertex
records in the guarded SF vertex BO, and replace the 24-dword immediate-vertex
tail with the eight-dword index packet while also adding the eight-dword
bounds packet.  This keeps the complete body length at 184 dwords while
changing the fetch mechanism.  A24 remains `24000003` in the existing type-04
state group.  The exact packet shape is statically established but not
silicon-tested.

This is also the kernel-validation boundary.  For every referenced element,
derive the byte range from stream base, element offset and width, pitch,
minimum/maximum validated indices, instance range, and primitive count.  Do
not trust A2C/A2D supplied by users, and do not model the inline type-00 bytes
as a relocatable index BO.

## Render-target and depth formats

The exact render-target byte enumeration, public-D3D-to-private-to-hardware
chains, RT address/pitch layout, channel order, and write-mask semantics are
in [windows-draw-path.md](windows-draw-path.md).  The common safe targets are
linear `98=X8R8G8B8` and `99=A8R8G8B8`; code `98` stores logical red as
`00ff0000` and logical blue as `000000ff`.

The same document proves the depth format chain and A10..A13 fields:

| format | private | A12 `[18:16]` | memory interpretation |
|---|---:|---:|---|
| D16/D16_LOCKABLE | `101d` | `0` | 16-bit fixed Z |
| D32 | `203f` | `4` | 32-bit fixed Z |
| D32F_LOCKABLE | `2040` | `5` | 32-bit `s[8].23` floating Z |
| D24X8 | `2032` | `6` | Z24 in bits 31:8 |
| D24S8 | `2033` | `6` | Z24 in bits 31:8, stencil in 7:0 |

Only private `2033` enables stencil in both proprietary drivers.  The first
driver milestone should stay with linear D16 and the packet-exact guarded
test in [depth-blend-oracles.md](depth-blend-oracles.md); tiling, compression,
coarse Z, separate stencil, and GPU clears remain independent validation
gates.

## Recommended order on silicon

1. Re-run the unchanged 188-dword draw and the 184-dword Gouraud triangle.
2. Test CW/CCW culling with the paired vertex-order discriminator.
3. Test scissor only by shrinking A82/A83 inside the already-proven color
   window; preserve A80/A81 and the entire viewport tuple.
4. Run plain linear D16, then the direct negative-offset discriminator.
5. Run SF vertex fetch with three records and immediate 16-bit indices.
6. Test edge AA last; it has no bearing on MSAA capability.

At every stage preserve color/depth/vertex guard pages, require `040c=0` and
the complete engine-idle mask, and restore the known PAUSE state.  No failed
front-end test justifies freeing still-reachable storage.

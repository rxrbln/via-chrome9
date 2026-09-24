# Chrome9 ideas logo-shadow linked compiler closure

Status: source-exact linked NIR and production compiler object; no new GPU
execution in this audit.

This note closes the static whole-program object for the default glmark2
`ideas-logo-shadow.frag` draw.  It extends
`chrome9-vpos-texture-coordinate-oracle.md`, whose object deliberately omitted
the state tracker's framebuffer-dependent Y transform.  Native vPos integer
centres, computed-coordinate nearest/repeat sampling, and finite sampled-A8
TEXKILL have each passed guarded VX900 tests independently; their complete
linked composition remains the final gate.

## Source and current linked NIR

The glmark2 2023.01 source is:

```glsl
vec2 curPos = gl_FragCoord.xy / 32.0;
vec4 color = texture2D(tex, curPos);
if (color.w < 0.5)
    discard;
gl_FragColor = color;
```

Its SHA-256 is
`30c264927febb9d8e114fc1df4b9a5e76f0d12785f1bcc4f6dd6eec12511a9d4`.
The captured linked Chrome9 NIR is GLSL19 in
`/tmp/chrome9-glmark-nir-v4/nir/ideas.err`, whose whole-file SHA-256 is
`cbed9235096f844954b20b1cf5015ba47d6eaa33980ec3e9bb7c2011db2c8d70`
and whose source BLAKE3 words are:

```text
3ccce051 2c76c93a 52364f8c ff0550c7
6a12ca4b 8c073ae8 f5186f67 a89aa67c
```

Mesa's state tracker makes the implicit coordinate convention explicit:

```text
y'       = gl_FragCoord.y * gl_FbWposYTransform.x
           + gl_FbWposYTransform.y
coord.xy = vec2(gl_FragCoord.x, y') * vec2(1/32, 1/32)
sample   = TEX2D(s0, coord)
terminate_if(sample.a < 0.5)
oC0      = sample
```

This is a straight-line data-flow graph with one ordered side effect.  The
Y-transform uniform must remain draw-time state because window and FBO
orientation differ.

## Exact production semantic object

The following PS 2.0 stream is semantically identical to the linked NIR.  It
keeps partial vector construction explicit so the production register
manager and MISC relocation see the real pressure:

```text
ffff0200
04000005 00020000 10551000 20000000  # MUL r0.y,vPos.y,c0.x
04000002 00020000 00550000 20550000  # ADD r0.y,r0.y,c0.y
03000001 00010001 10001000           # MOV r1.x,vPos.x
03000001 00020001 00550000           # MOV r1.y,r0.y
04000005 00030002 00440001 20440001  # MUL r2.xy,r1.xyxy,c1.xyxy
04000042 000f0003 00440002 20e40800  # TEXLD r3,r2.xyxy,s0
04000003 000f0004 00ff0003 20000002  # SUB r4,r3.aaaa,c2.xxxx
01000041 000f0004                    # TEXKILL r4
03000001 000f0800 00e40003           # MOV oC0,r3
0000ffff
```

Here c0 is `gl_FbWposYTransform`, c1.xy is `1/32`, and c2.x is `0.5`.
The token SHA-256 is
`e0607f33d5efb6c298737bb4ba4ec5ec1f1d179720b840eb7f9f2b385d73eba9`.
Linux 5.75.32.87a production compilation followed by configuration-two
finalization returns active-resource tuple `(2,5,0,1)` and exactly:

```text
ALU
0140b6d0 d0080a2d 0020c7a6 00000020
0080a6d0 d0140924 002000a6 00000020
01c00530 d01c0053 0012c716 000d0060
01c00530 d01c0053 002200a6 000d0060
0140b6d0 d008106d 00320106 000000a0
01c00530 101c0053 00f20204 000d0420
80036db0 00140924 00f08200 00000120
01c00530 301c0053 00f20405 000d0460
01c00530 301c0053 00f20305 000d0820

TAU
301301b0 314011b1

A90--A9b
90048080 91000003 92020009 93000003
94000000 950000a0 96000000 97000000
98000000 99000000 9a000000 9b000000
```

Packed little-endian SHA-256 values are:

| stream | SHA-256 |
|---|---|
| 9 ALU records | `740d9191beb4189604e27b97434c820592c7c497ce936a9f6f43611e0d2cb0b8` |
| 2 TAU records | `0cb9c560b2df79990f5587e85b989e5ac4ba831b4e85540e43dd95f843a0bea3` |
| A90--A9b | `da9796568ff004edbf9d3736fba98841ab8bf1c6f87ea56719e5a6fd3f2fe6c3` |
| ALU then TAU then controls | `b353a35efd4f917e2b91893720a8e15970b918414d1564cc930a195792607571` |

The tuple is materially different from the simplified vPos/32 object.  MISC
adds one to the first resource dimension; adjusted `(3,5)` selects production
bucket/relocation id 7.  A90 therefore carries MISC usage bit 7 (`80`), and
the position operands in the first and third ALUs encode native class-3 id 7.
Do not hard-code the smaller program's id 3.

A91 bits 0 and 1 name the internally generated sample coordinate and KILL
scratch address.  A95 bits 5 and 7 switch after the coordinate bridge and
the sampled-alpha subtraction.  A93 bits 0 and 1 return after TEXLD and
TEXKILL.  The KILL operation is thus ordered between the sampled comparison
and terminal color MOV; it is not a Boolean ALU or fixed alpha-test alias.
Equality survives because SUB produces zero and finite TEXKILL rejects only
negative lanes.

## Exact texture and draw-state qualification

The installed scene source removes two possible ambiguities.  `logo.cc`
creates a 32x32 unsized `GL_ALPHA` image with `GL_UNSIGNED_BYTE`, selects
NEAREST minification/magnification, and sets both axes to REPEAT.  The current
state tracker chooses `PIPE_FORMAT_A8_UNORM`; its canonical sampler view is
identity and specifically has `swizzle_a=PIPE_SWIZZLE_W`.  Chrome9 native A8
format `A30=301b0000` already decodes as `(0,0,0,A)`, so the semantic TEX
result remains XYZW and the alpha predicate reads W.  Applying an additional
alpha replication at the view boundary would be wrong.  For this single-level
NEAREST/REPEAT object the sampler image is:

```text
A30=30000000 A31=31100000 A32=32000052
```

Here A32 contains repeat value two in both S and T.  The image-format A30 and
sampler A30 live in different HParaType selector spaces.

`scene-ideas.cpp` enables culling but explicitly disables depth testing before
the `LOGO_SHADOW` draw, then re-enables depth only for the later lamp and
normal-logo draws.  Consequently the exact shadow submission has no depth
handle, no `DRM_CHROME9_FRAGMENT_DEPTH` flag, and no fragment-depth output.
The recovered KILL schedule is still TEX, replicated-alpha SUB, KILL bridge,
TEXKILL, then terminal color MOV.  The guarded A8 KILL test with D16 proves
the stronger color-plus-depth suppression contract, but KILL/depth-write
interaction is not an additional prerequisite for this installed draw.

The exact A8 decoder, repeat sampler bits, POSITION/MISC linkage, KILL
transition maps, and color-only destination packet are all qualified fields.
The only unqualified item is their complete linked execution on silicon; no
unknown register or packet field has been isolated inside that composition.

## Current implementation boundary

The current clean-room tree already contains every semantic piece:

* `chrome9_ps_leaf()` lowers `load_frag_coord` to a POSITION source;
* the generic lowerer handles the MUL/ADD and partial vec construction;
* `chrome9_ps_lower_alpha_terminate()` accepts only the exact
  `terminate_if(flt(tex2d(s0).a, 0.5))` shape and emits ordered SUB/KILL;
* the UAPI/kernel validates POSITION XY, performs resource-dependent MISC
  relocation, and schedules TEMP-coordinate bridges and KILL;
* A8 sampling/swizzling, NEAREST/REPEAT, vPos integer centres, computed
  coordinates, and finite below/equal/above TEXKILL each have guarded live
  evidence.

Compilation is still blocked by policy in `chrome9_context.c`: the call to
the exact alpha-terminate lowerer requires both the kernel KILL capability
and `CHROME9_EXPERIMENTAL_TEXKILL=1`.  The kernel advertises
`DRM_CHROME9_CAP_FRAGMENT_KILL` and `DRM_CHROME9_CAP_FRAGMENT_POSITION`.
Consequently the explicit experimental option, rather than a missing opcode
or resource field, is the first remaining gate for this shader.

## Smallest guarded production gate

The smallest generic change is to admit the existing exact sampled-alpha
lowering whenever the kernel KILL capability is present.  Do not generalize
discard, comparisons, samplers, or arbitrary predicates.  Keep every check
inside `chrome9_ps_lower_alpha_terminate()` and continue rejecting a second
side effect, a non-s0 sample, non-2-D target, non-alpha lane, non-0.5
reference, demote, or control flow.

Before removing the option, add one offline captured-shape test which asserts:

```text
POSITION source present; no fabricated VS varying
one s0 TEX and one KILL
ordered semantic shape ending in the same sample MOV
constant set contains framebuffer transform, 1/32 and 0.5
all instruction/resource/UAPI bounds accepted
any predicate/lane/reference mutation rejected
```

Then run one guarded composition test, because independent primitive success
does not prove their combined schedule:

1. Use a guarded 32x32 A8 stipple texture, NEAREST/REPEAT, guarded BGRA color
   and D16, and a nonzero viewport origin.
2. Exercise both window-style and FBO-style Y transforms.  Use an asymmetric
   texture so X/Y exchange, missing inversion, half-pixel bias, or hard-coded
   transform values are distinguishable.
3. Require below-half texels to preserve cleared color and D16, and
   above/equal-half texels to update both.  Check all guards, source snapshot,
   timeout/reset counters, and a known-good draw before and after.

Passing that test closes the default logo-shadow fragment program only.  It
does not authorize the lit logo/lamp normalization, FEQ/bcsel `unitvec`, POW,
or three-light loop shaders elsewhere in `ideas`.

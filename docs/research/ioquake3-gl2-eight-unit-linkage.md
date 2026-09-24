# Chrome9 GL2 sampler limits and eight-unit linkage

Status: ioquake3 source-exact startup audit, VX900-manual packet derivation,
and fresh Windows/Linux compiler replay, 2026-09-21; current Mesa compiler
boundary refreshed 2026-09-24.  Texture stages zero through two have bounded
structured-path evidence.  Stages three through seven and the five-TAU object
below are not silicon-proven.  The current production limit of three remains
truthful; the existing environment override is diagnostic only.

## What ioquake3 actually needs to start

The audited renderer is ioquake3 commit
`93abc60a5b79e4b8cc1ab985a03c272cdb0d0bb1`.  Default cvars enable normal,
specular and deluxe mapping, disable cube mapping and use sunlight mode one.
The lightall initializer visits permutation indices in increasing order.

The first shader exceeding Chrome9's current three-fragment-sampler cap is
`LIGHTDEF_USE_LIGHTMAP`, index one.  Its active uniforms are:

```text
u_DiffuseMap   logical unit 0
u_LightMap     logical unit 1
u_NormalMap    logical unit 2
u_DeluxeMap    logical unit 3
u_SpecularMap  logical unit 4
```

It therefore needs five active samplers.  A later lightmap plus
`LIGHTDEF_USE_SHADOWMAP` permutation adds `u_ShadowMap` at unit five and is
the default startup maximum of six simultaneously active samplers.

There is a separate logical-index constraint.  `shadowmask_fp.glsl` has only
four active samplers, but ioquake assigns them to units 5, 3, 1 and 6.
Consequently default startup needs at least seven logical fragment texture
units, indices 0..6, even though no default shader has seven simultaneous
samples.  Cube mapping is default-off and is disabled by ioquake below GL 3;
unit six would otherwise also hold `u_CubeMap`.  Default startup does not use
unit seven or eight simultaneous samplers.

For diagnosis, set the already implemented
`CHROME9_DEBUG_MAX_FS_SAMPLERS=7`.  It raises both Gallium
`max_texture_samplers` and `max_sampler_views`; seven is the smallest value
which admits all default uniform assignments.  Setting eight is useful only
for the stage-seven lab oracle.

This override is not a production capability.  Advertising eight says every
valid shader within that resource limit executes correctly.  A closed
classifier which silently drops other valid three-to-eight-sampler draws does
not satisfy that contract.  Promote the default only after state, lifetime,
compiler and representative program coverage exist; otherwise keep this an
explicit diagnostic probe.

## Current first-default-permutation compiler boundary

The source-order index-one program above is also the first useful default
world permutation, not merely a sampler-cap counterexample.  The available
local ioquake3 tree is shallow commit `5883936`; its lightall sources and
feature construction match the source lines audited at `93abc60`, but the
older commit object is not present locally.  The byte identities below are
therefore explicitly for `5883936` and must not be relabelled as a capture of
`93abc60`.

The probe used the default semantic defines:

```text
USE_LIGHT USE_LIGHTMAP USE_DELUXEMAP USE_NORMALMAP USE_SPECULARMAP
r_deluxeSpecular=0.300000 GLOSS_IS_SMOOTHNESS
```

The raw source SHA-256 values are:

```text
lightall_vp.glsl  a59b8a25c7f85be6d90076c0248c278cc984813ec9325bdb192ec2074233e1dc
lightall_fp.glsl  46b1467726d01d81ddb1f217f519aa7851790831b10715076fed8f8df4aa58a3
```

The linked fragment object has five dense samplers, logical units zero
through four, and six smooth vec4 declarations.  Only three lanes of VAR5 are
live, so the exact smooth interface is 23 components:

```text
VAR0  packed diffuse/lightmap coordinates, xy/zw
VAR1  vertex color
VAR2  normal.xyz and view.x
VAR3  tangent.xyz and view.y
VAR4  bitangent.xyz and view.z
VAR5  light direction.xyz
FACE  gl_FrontFacing system value, flat per primitive
```

The non-sampler store uses three vec4s (`u_EnableTextures`, `u_NormalScale`,
`u_SpecularScale`) plus the integer alpha mode: 13 components.  This fits the
32-vector constant store.  The current link fails first at five samplers
versus the advertised three.  A diagnostic sampler override can pass that
link check, but does not qualify five descriptor stages or the six-varying
draw layout.

After mechanically applying the semantically exact alpha-mode-zero variant,
the linked fragment NIR has source BLAKE3 words:

```text
b74fa228 0463e837 19ff85e8 95d43a83
eeab1fd8 4f324902 fb56f997 33ce7ba0
```

Its operation inventory is 95 ALU nodes plus five ordinary 2D TEX:

```text
32 MUL, 18 ADD, 8 DP3, 7 NEG, 7 MAX, 6 MIN, 4 RSQ,
3 MOV, 2 RCP, 2 vec2, and one each b2b1, BCSEL, POW,
SQRT, vec3 and vec4
```

This is already close to the 96-ALU store before native expansion.  POW
becomes LOG2/MUL/EXP2, adding two records relative to its NIR node; SQRT still
needs a conformant lowering; constructors may fold into swizzles or require
MOVs.  An exact final native count and twelve-register physical-temp high-
water cannot be claimed until those semantic blockers lower successfully.

The first current backend rejection after mode-zero specialization is not an
unknown lighting opcode.  `gl_FrontFacing` appears as a FACE `load_input`,
then drives `bcsel(face, -normal, normal)`.  `chrome9_compile_ps_nir()` admits
smooth interpolated inputs but not the FACE system load, so it rejects before
emitting a semantic object.  Even if the value were exposed as an ordinary
source, the only current BCSEL lowering is a bounded floating `fge` select on
a proven unit interval; it does not implement a boolean front-face select.
The next independent arithmetic reject is the unlowered SQRT used to
reconstruct normal Z.  The current compiler result is therefore exact and
empty: `supported=0`, `semantic=0`, zero semantic records, zero ALU and zero
TAU.  No partial native object is evidence of fit.

The exact production-compiler vFace differential and the bounded synthetic
face-sign resource experiment are documented in
`chrome9-vface-normal-sign-boundary.md`.  In particular, vFace and vPos
compile byte-identically even in the actual vec3 CMP shape, so POSITION/MISC
must not be exposed as FACE.

The resource experiment is now closed through the actual state-tracker
lowering sequence, rather than the earlier raw linked-NIR approximation.  It
uses these deliberately non-production sources:

```text
synthetic-face VS SHA-256
3afed6042934313a00bfd8416d42e76f321b5809574dd1e5952b765371edb793
synthetic-face FS SHA-256
dc623f4baf85ef408aabe8a856867688dc636753054eaac85928184f91a8fdc7
linked FS NIR BLAKE3
0dadc9ae c424498f f40534ee 1d6d0277
bc825969 60085bbb 1eb193de b51a203a
```

The VS carries a caller-supplied `+1/-1` face sign in the otherwise dead
`var_LightDir.w`; the FS computes
`surfNormal = -var_LightDir.w * var_Normal.xyz`.  This remains only a
resource discriminator because an ordinary VS cannot know post-clipping
primitive facing.  The probe then runs the same `nir_lower_io`,
`nir_lower_uniforms_to_ubo` and `gl_nir_lower_samplers` sequence as
`m9_fs_compile_test.cpp::lower_like_st()`, enables the already qualified
nonnegative `fsqrt -> frsq+frcp` lowering, and invokes the current Chrome9
semantic compiler.

The exact semantic result is:

```text
100 ordered records = 95 ALU + 5 TEX
varying mask          0x003f (six generic varyings)
sampler mask          0x1f   (five dense samplers)
uniform mask          0x00000007
constant mask         0xe0000007
virtual SSA temps     96
```

Therefore 96 ALU and 32 TEX are sufficient for this transformed permutation;
the instruction stores are not its next resource gate.  Native allocation is.
Replaying the kernel's allocate-before-release interval rule over that exact
semantic stream reaches all twelve physical temporaries and fails while
allocating record 34; the complete unscheduled SSA order has a peak live set
of 15 at record 67.  Moreover six active direct varying coordinates select
the kernel's largest resource bucket, which permits at most six physical
temporaries.

Thus merely extending the native encoder from 12 registers would still not
make the object submit-safe.  A generic scheduler must sink definitions,
shorten live ranges, or recompute/spill values until the physical high-water
is at most six, while preserving all 95 ALU and five texture results.

This does not remove the earlier API gates: production still truthfully
advertises three fragment inputs and three sampler slots.  The discriminator
only proves that, after those route and descriptor paths are generalized,
raising the ALU cap is not the required fix.

### Scheduling and channel-allocation discriminator

Mesa's generic `nir_schedule()` is not a fix at the point where the driver
currently calls the semantic compiler.  Normal and fallback scheduling at
pressure thresholds 4, 6, 8, 12 and 32 all reproduce the same 100-record
semantic order, allocator failure and peak.  `chrome9_compile_ps_nir()` walks
the output DAG recursively and emits its own postorder, so it discards the
NIR block order.  Scheduling must happen while emitting that DAG or over the
completed semantic IR.

The first failure does have a zero-cost local improvement.  Records 23..29
form the roughness denominator chain; their first late consumers are records
66, 75 and 80.  Sinking the chain immediately before record 66 reduces the
live set at record 34 from 13 vectors to 11 without adding or deleting an
operation.  It does not lower the later peak because `t29` and `t35` then
remain live across those three consumers.

A dependency-checked, deliberately offline schedule gives a stronger upper
bound.  It sinks the independent lightmap/normalization and reflectance
subtrees, moves the diffuse fetch to its first consumer, and duplicates one
ADD so the early `t12` lifetime need not cross the late normalization.  The
result is 101 semantic records, exactly 96 ALU plus five TEX, with a peak of
ten virtual vectors and 21 written channels.  It has no undefined temporary.
This exhausts the ALU store but demonstrates that spilling is not the first
required mechanism.

Twenty-one live channels fit within the raw 24-channel capacity of six vec4
registers, but that count is not yet a successful hardware coloring.  One
vector destination's written channels must remain co-resident in one physical
register, and every A/C/B source-port rule must still hold.  The proprietary
Linux compiler provides direct guidance rather than a reason to guess:

```text
001c1f00 T TryAllocChn_invi
001c2090 T RenameSrcChannel_invi
00161a40 T stmPPS_ShrinkSrcChn_invi
```

`TryAllocChn_invi` selects one candidate slot for a requested channel group
while checking the named channel lifetimes independently.  Its disassembly
has 16 candidate slots for classes zero and one and four for class eight.
`RenameSrcChannel_invi` separately rewrites source channels.  The generic
next step is therefore a grouped per-channel interval allocator plus semantic
source-swizzle rewriting, validated against port rules.  Only that allocator
can decide whether the 21-channel schedule really colors into the six-temp
resource bucket; the current whole-vector allocator cannot.

The generic implementation ladder is:

1. retain alpha mode in the draw-time variant key and prove mode zero removes
   all three `terminate_if` side effects;
2. expose per-primitive FACE semantics without an application key, and lower
   the sign selection exactly for both front and back triangles;
3. lower finite nonnegative SQRT through a qualified sequence and reject
   exceptional domains until characterized;
4. schedule the exact semantic DAG and require final ALU `<=96`, TEX `=5`,
   physical high-water `<=6` for the six-coordinate resource bucket, varying
   mask `0x3f`, sampler mask `0x1f`, and no unmodelled spill;
5. only then promote five dense descriptors and six float varyings, followed
   by distinct-color front/back pixel oracles and stale-state closure.

Do not solve this boundary by forcing all triangles front-facing, dropping
the normal/deluxe/specular samples, or selecting the reduced two-texture
object.  Those change the default shader rather than implement it.

## Hardware namespace and image state

Both proprietary OpenGL ICDs advertise eight texture units.  The VX900 manual
defines FE general state for texture stages 0..7 and says A00 supports at most
eight.  Native TAU sampler ids are four bits; the proprietary compiler accepts
s0..s15, but initial API exposure must stop at s7.

For stage `n`, 0 through 7, the complete level-zero image/sampler pair uses:

```text
image H2 selector    (n << 24) | 00020000
image payload        base>>8, 18001101, A20 dimensions,
                     21000001, A22 layout/log, A30 format

sampler H2 selector  ((0x20 + n) << 24) | 00020000
sampler payload      31100000, 32000052, pad, pad
```

Thus stage seven uses selectors `07020000` and `27020000`.  Each image block
is 12 dwords including header/padding; each sampler block is eight.  Formats,
dimensions, wrap, filter, guarded layouts and level-zero restrictions remain
the already validated per-texture rules.  An eventual implementation loops
the existing validator rather than accepting raw A18..A32 values.

The exact FE state for five active, independently perspective-corrected ST
coordinates A..E is:

```text
A00 = 00000051       # five active textures plus cache-clear pulse
A01 = 01292521       # stages 0/1/2 use coordinates A/B/C, perspective ST
A02 = 0200312d       # stages 3/4 use D/E; stage 5 disabled
A03 = 03000000       # stages 6/7 disabled
A04 = 04000000       # no screen-coordinate replacement
A05 = 05000000       # no user clip texture
A08 = 08010105       # A/B are ST; five coordinate output registers
A09 = 09010101       # C/D/E are ST
A0a = 0a000000       # F/G/H disabled
A0b = 0b000000       # no A-D wrap correction
A0c = 0c000000       # no E-H wrap correction
A0d = 0d810000       # vendor closure, no coordinate modulus enables
```

The exact eight-stage counterpart is:

```text
A00 = 00000081
A01 = 01292521
A02 = 0235312d
A03 = 03003d39
A04 = 04000000
A05 = 05000000
A08 = 08010108
A09 = 09010101
A0a = 0a010101
A0b = 0b000000
A0c = 0c000000
A0d = 0d810000
```

These values follow the manual fields exactly.  Every stage field selects the
same-letter coordinate A..H, dimension ST and perspective mode one.  A01
packs stages 0..2, A02 stages 3..5 and A03 stages 6..7; A08..A0a declare the
eight VS coordinate outputs.  The current two-texture packet emits only the
nonzero subset and therefore cannot be extended by changing A00/A08 alone.

## Exact FVF and fixed VS growth

Five textures use a 16-dword immediate vertex:

```text
D0 float4 position/RHW, D1 normalized diffuse, D2 specular,
D3..D7 five float2 coordinates
```

The H6 input declarations begin:

```text
00000710 010000c2 020041d1 030052d0
04006342 05008442 0600a542 0700c642 0800e742
```

They require both halves of the input/output FVF mask:

```text
A25=25f6cccc A26=2600c000
A28=28ffcccc A29=2900c000
A62=627081ff A60=60800008
```

The position/D1/D2 base has four active VS records.  Append:

```text
0008c14c 00000000 00000000 0008028f   # D3 -> oT0
0009014c 00000000 00000000 0008029f   # D4 -> oT1
0009414c 00000000 00000000 000802af   # D5 -> oT2
0009814c 00000000 00000000 000802bf   # D6 -> oT3
0009c14c 00000000 00000000 000802cf   # D7 -> oT4
```

Eight textures use a 22-dword vertex and continue:

```text
00000a16  ...existing D0..D7...
09010842 0a012942 0b014a42             # D8, D9, D10 float2

A25=25f6cccc A26=2600cccc
A28=28ffcccc A29=2900cccc
A62=62a081ff A60=6080000b

000a014c 00000000 00000000 000802df   # D8  -> oT5
000a414c 00000000 00000000 000802ef   # D9  -> oT6
000a814c 00000000 00000000 000802ff   # D10 -> oT7
```

`A62` follows the already recovered `(texture_count + 2) << 20` front-end
resource rule.  The first declaration word packs last input register and
stride exactly as the live one-/two-unit `00000308/0000040a` pair.  A25/A26
and A28/A29 come directly from the manual's S/T mask positions; omitting A26
or A29 would substitute default coordinates for E..H.

## Fresh compiler objects

The probes used the exact D3D UMD and Linux 5.75 compiler named in
`ioquake3-gl2-ps-next-rungs.md`, with Linux production configuration two.

One direct stage-seven sample:

```text
tokens      texld r0,t7,s7; mov oC0,r0
token hash  13c1642669efff0488cde7b0bf7332069eb6615f4bc2d7cc6c11891a06d5f576

A90..A93    90000000 91000001 92010001 93000001
TAU         b71301b7
ALU         01c00530 301c0053 00f20005 000d0820
TAU hash    04ef0cf3bcdb59b7c0e0e5119fc4f2a91de51dcc2973af253e1e5ce2472ef554
```

Five independent samples whose output is the fifth sample:

```text
tokens      texld r0,t0,s0; ...; texld r4,t4,s4; mov oC0,r4
token hash  677444fb46d561a1c316f172648a664c6415314021ba2b3a946391ff79deffb2

A90..A94    90000000 91000001 92050001 93000010 94000000
TAU         a01001b0 a11001b1 a21001b2 a31001b3 b41001b4
ALU         01c00530 001c0053 00f03f00 000d0820
ALU hash    6c6789510b6db0fa37264639d84edce9655347a65bed5abe5b99656c27db8732
TAU hash    64ae5de4e06a6036ce11779896914b18cdb3481f2aa2a51ba414e94a975bad10
```

The final TAU's phase/dependency bits differ from the first four.  Do not
generate this object by incrementing sampler ids in the one-unit word.  The
control tuple, five TAUs and consuming ALU are one indivisible semantic
program.

## Packet sizes and indices

Use the state-complete destination envelope: 20 dwords, then 20 dwords per
texture, then the 12-dword common raster group.  Five stages require the full
16-dword FE group above and a 36-dword FVF group.  For the five-sample object,
with no D16 insertion, exact group starts are:

```text
destination               0
image/sampler stage 0     20 / 32
image/sampler stage 1     40 / 52
image/sampler stage 2     60 / 72
image/sampler stage 3     80 / 92
image/sampler stage 4    100 / 112
common raster            120
full FE texture state    132
FVF                      148
c0 closure               184
VS float/int/clip        192 / 204 / 212
nine-record VS           220
VS activation            260
PS controls              268
five-TAU H2              288
one-ALU H2               300
vertex H2/payload        308 / 312
body length              360 dwords
```

A D16 insertion adds four before FE/FVF and shifts every later group by four;
the exact body becomes 364.  Source-over and other normalized blend state fit
inside the existing 20-dword destination envelope.

The direct stage-seven diagnostic has eight 20-dword image/sampler pairs, a
40-dword FVF group and a 52-dword twelve-record VS group.  Its no-depth map
ends with PS controls at 344, TAU at 364, ALU at 372, vertex header at 380,
and 66 vertex dwords at 384..449.  Pad body 450..451 canonically: total 452
dwords, or 456 with D16.

Both fit comfortably in the existing ring staging page.  The risk is object
lifetime and state completeness, not command size.

## Safe BO and promotion sequence

The first higher-stage oracle need not create nine distinct texture objects.
Permit read-only texture descriptors to alias one another, while requiring
every texture object distinct from target and optional depth.  Bind one
guarded 1x1 white dummy to unused lower stages and a distinct asymmetric
guarded image to the highest tested stage.  Deduplicate handles before the
single `drm_exec` reservation and GART map transaction; fence unique textures
READ, target/depth WRITE, and check every unique guard after retirement.

Run in this order, each under timeout with MMIO/error/guard checks and a
following known-good draw:

1. Three active stages, direct output from stage two.  Red/green/blue/white
   quadrants and independent T2 coordinates prove A01/A08, D5 and oT2.
2. Five active stages, direct output from stage four.  This proves A02/A09,
   D7/oT4, stage-four image/sampler selectors and the fifth fetch result.
3. The exact five-TAU object above, with the same stage-four image and white
   lower stages.  Its result must equal step two; this isolates the longer
   schedule from image/FVF changes.
4. Seven active stages, direct output from stage six.  This first exercises
   A03/A0a, A26/A29 and the second-half coordinate masks.
5. Eight active stages, direct output from stage seven using `b71301b7`.
   Test distinct S/T corners, not a uniform image.  This is the last and
   highest-risk stage-ceiling test.
6. Repeat stage 0 immediately after stage 7, then 7 after 0.  Explicitly clear
   A02/A03/A09/A0a and reduce A00/A08/A62/A60/FVF masks on the one-unit draw;
   stale high-stage state must not affect either result.

Negative tests reject a sparse descriptor without canonical dummy filling,
target/depth alias, noncanonical layout, unsupported format/mip/cube/shadow,
missing coordinate, non-finite S/T/RHW, unknown program, wrong TAU count,
wrong A93 switch bit and every unknown flag before GEM mapping.

Only after these pass may a higher-stage semantic ioctl be implemented.
Production `max_texture_samplers=8` still requires enough closed compiler
coverage or a correct fallback for valid shaders within the advertised
limit.  Passing stage seven alone establishes physical state/linkage, not
general GLSL conformance.

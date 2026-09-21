# Chrome9 two-texture scene coverage and D16 successor

This note distinguishes applications that can use the live fixed
`texture0.rgba * texture1.rgba` program from superficially similar multi-sample
shaders.  It also records the minimum four-object D16 successor needed by an
opaque ioquake3 lightmap pass.  The packet values are the already recovered
and scheduled values in
[xorg-one-texture-oracle.md](xorg-one-texture-oracle.md); no programmable
shader claim follows from them.

## Scene classification

| workload | fragment operation | live two-texture key? | first missing block |
|---|---|---:|---|
| ioquake3 opaque base lightmap | `tex0.rgba * tex1.rgba` | yes | combine with D16 |
| desktop shadow | one texture REPLACE, then source-over | not two-texture | already separate bounded keys |
| glmark2 shadow | project/divide, sample depth, compare and branch | no | projective sample plus compare/control flow |
| glmark2 refract | three dependent samples plus normalize/refract/pow/add | no | general PS ALU/TEX |
| glmark2 terrain | vertex sampling plus at least five fragment sources | no | VTF, sampler count, general PS and FBO chain |
| glmark2 jellyfish | two samples plus varying ADD/MUL/POW | no | general PS ALU after two samples |

The glmark2 shadow program is particularly easy to misclassify.  It samples
only one `ShadowMap`; its second apparent source is interpolated `Color`, and
the result is selected by a sampled-depth comparison.  It is neither
`tex0*tex1` nor `tex0*primary`.  The refract shader binds `DistanceMap`,
`NormalMap`, and `ImageMap`, with each later coordinate dependent on earlier
results.  Terrain truthfully remains skipped while vertex texture samplers
are advertised as zero.

For ioquake3, accept only the exact linked NIR already used by the live
two-texture path: one ordinary non-array/non-shadow 2D sample from unit 0,
one from unit 1, distinct matching coordinate varyings, identity RGBA `fmul`,
and one complete color store.  Fixed-function projector operands are allowed
only when Q is the compile-time/default one already recognized by the current
classifier.  Vertex primary modulation, RGB/alpha scale, fog, alpha test,
framebuffer blend, mip selection and a third texture factor remain different
keys.

## Exact D16 packet delta

The live no-depth triangle body is 262 dwords.  D16 replaces the common-state
pad and adds four dwords; the resulting body is exactly 266 dwords.  Texture
images, samplers, FVF, VS, PS, TAU, RGBA MUL and ten-dword vertex records are
unchanged.

```text
body[56..59] common H2 header
body[60]     860000ff
body[61]     870000ff
body[62]     53010c0f
body[63]     00000011
body[64]     01c0154c                     A01 D16 test/write
body[65]     88001ed0
body[66]     89000800
body[67]     10000000 | (depth_base >> 8) A10
body[68]     11000001 | ((pitch/32)<<13)  A11 linear
body[69]     12380000                     A12 D16
body[70]     13000000 | (func<<16)        A13 LESS/LEQUAL
body[71]     20007fff                     A20 wide Z clip

body[97]     20000000                     independent triangle list
body[98]     23004f02                     Gouraud
body[99]     24000003                     three vertices
body[221]    30202053                     r0.xyzw * r2.xyzw
body[232]    FE000000                     immediate vertex H2
body[236..265] three records:
               X,Y,Z,RHW,D1,D2,S0,T0,S1,T1
```

Closure requires a following no-depth two-texture draw to restore
`A01=01c01540`.  A stale A10--A13 address is harmless only after the D16
enable/write bits are cleared; test both transition directions without an
engine reset.

## Narrow UAPI contract

Use a new ioctl rather than changing the 240-byte no-depth structure.  A
256-byte request has a simple fixed layout:

```c
struct drm_chrome9_draw_two_textured_triangle_d16 {
        __u32 destination_handle;
        __u32 depth_handle;
        __u32 flags;              /* depth func only */
        __u32 width;
        __u32 height;
        __u32 destination_pitch;
        __u32 depth_pitch;
        __u32 reserved0;
        struct drm_chrome9_texture_state textures[2];
        struct drm_chrome9_two_textured_vertex vertices[3];
        struct drm_chrome9_scissor_state scissor;
        __u32 reserved[5];
};
```

Require every reserved word zero.  Accept flags zero as legacy LESS or the
existing explicit LESS/LEQUAL depth-function encoding, and reject all other
bits.  Initially require XRGB destination, canonical D16 pitch, two level-zero
guarded 32-bit sources, the existing filter/wrap set, finite positive RHW,
finite S/T in `[0,1]`, nondegenerate in-range vertices, and four distinct
same-device native shmem handles.  The only operation is RGBA MODULATE plus
D16 test/write; there is no program, blend, alpha-test, mip, scale, or format
selector.

## Four-object lifetime

Do not extend the current three-object aliasing trick by silently giving one
field a third meaning.  Resolve the roles explicitly as destination, texture
0, texture 1 and depth.  The submit transaction must:

1. look up and validate all four objects before allocation or reservation;
2. reserve all four together and wait for prior readers and writers, because
   guard initialization writes every BO;
3. acquire four SG tables and concatenate PTEs in the same explicit order;
4. map all four and prepare both the canonical inner guards and final guards
   on larger mip-capable texture allocations;
5. publish one SF node and attach one fence as WRITE to target/depth and READ
   to both textures;
6. on normal retirement return DMA ownership, flush, verify eight guards,
   resync all four objects, unbind the complete node, then unmap and put every
   object;
7. on any post-publication uncertainty quarantine the one indivisible job
   with all four references, maps, SG tables, PTEs and the SF node.

An array of four role descriptors is safer than adding another set of
`texture3_*` cleanup branches to the existing three-object helper: every
error-unwind loop then has the same proven order and cannot omit the fourth
BO.  If the current job representation is retained for ABI stability, add a
fourth explicit object/map/SGT/page-count/mapped flag and audit every normal,
prepublication, timeout and teardown path as one patch.

## Guarded oracle

Use four native guarded BOs: 64x64 XRGB target, two 32x32 XRGB sources, and
64x64 D16.  Fill target with `005a5a5a`, depth with `ffff`, and texture
quadrants as RGBW times red/red/blue/yellow.

1. At Z=`4000`, draw the covered region.  Interior quadrant samples must be
   red, black, blue and yellow; D16 must be approximately `4000`.
2. At Z=`c000`, draw different products over the same region.  LESS must
   preserve every first-pass color and depth sample.
3. Require uncovered target/depth sentinels, byte-identical source pixels,
   and all eight guards.
4. Submit the old no-depth two-texture oracle, then repeat the D16 oracle.
   Finally submit D16 then no-depth once more.  This catches stale A01 and
   address state in both directions.

Mesa may expose this only when the new capability and the existing depth-func
capability are both present.  The draw gate is exact `T0|T1`, two bound
samplers/views, Z16 LESS or LEQUAL with writes, no stencil/depth bounds,
XRGB target, and no blend/alpha test.  It marshals the existing ten-dword
post-TNL vertices unchanged and selects the new ioctl only when depth is
active; the current no-depth ioctl remains the fallback for the identical
shader with depth disabled.

# Chrome9 packed varying to ALU and TAU linkage

Status: exact hardware-free Linux production-compiler result, independent
Windows raw-TAU cross-check, exact Linux vertex-compiler output, and a
source-backed one-word raster discriminator.  The complete three-component
linkage has not retired on VX900 silicon.

This note closes a generic compiler case in which one smooth varying carries a
scalar in X and a two-dimensional texture coordinate in YZ.  It is not a scene
classifier.  The byte-exact data is in
`chrome9-packed-varying-linkage-corpus.json` and is checked by
`tools/re/chrome9_packed_varying_linkage_check.py`.

## Pixel object

The exact D3D semantics are:

```text
r0 = TEX(s0, T0.yz)
r1 = r0 * T0.x
oC0 = r1
```

The source token uses `T0.yzww`; the unused Z/W selectors make the complete
source token canonical without increasing the two-dimensional texture input.
Linux production configuration two emits:

```text
ALU 0140b6d0 302c0000 00f20005 00000060
    01c00530 301c0053 00f20105 000d0820
TAU 301006f0

A90..A9b
90018000 91000001 92010002 93000001
94000000 95000000 96000000 97000000
98000000 99000000 9a000000 9b000000
```

The active tuple is `(1,2,1,1)`.  Windows independently emits raw TAU
`201006f0`; configuration two owns the Linux bit-28 lifetime delta.

TAU swizzles are four two-bit selectors:

```text
payload = sx << 10 | sy << 8 | sz << 6 | sw << 4
identity xyzw = 01b0
yzww          = 06f0
```

Thus TAU `301006f0` is sampler zero, TEXLD, destination TEMP zero, physical
coordinate zero and swizzle YZWW.  The first ALU reads the same interpolant as
class 3, id 0, swizzle XXXX in dword one `302c0000`.  There is no
ALU-to-coordinate bridge and no duplicate physical coordinate.  A bridge is
required only when ALU arithmetic creates a new coordinate value; selecting
lanes from one direct varying remains a TAU source swizzle.

## Generic fragment lowering rule

For every direct fragment input, retain one semantic varying object with a
per-consumer swizzle.  A texture source may be encoded directly in TAU when:

1. it is a channel-only swizzle of one smooth T varying;
2. the target is ordinary non-array 1D--3D and the required coordinate lanes
   all come from that varying;
3. no arithmetic, constant addition, projection divide, or cross-varying
   construction occurs before sampling.

Do not clone the varying merely because ALU and TAU select different lanes.
Map its logical T id once to one compact physical coordinate id.  Encode ALU
source swizzles normally and TAU selectors independently.  If any texture
coordinate lane is computed, lower through an ordinary TEMP and the proven
class-1 bridge MOV before TAU.

For dimension `N`, only TAU output selectors X through component `N-1` impose
vertex-link requirements.  Canonical filler selectors in unused TAU lanes do
not.  This case uses TAU Y and Z plus ALU X, so the vertex output must contain
XYZ: three components, not four.

## Vertex and immediate linkage

The Linux VS 2.0 production path for

```text
oPos = v0
oT0.xyz = v1.xyz
```

reports output metadata `00010104`, seven uploaded records and five active
records.  Its two semantic records are:

```text
0008014c 00000000 00000000 00080a0f
0008414c 00000000 00000000 00080287
```

The remaining five exact finalizer/sentinel records are in the JSON corpus.
Output dimension metadata value one means three components, giving
A25=`25f0e000`, A26=`26000000`, A62 low nine bits one, and A60=`60800004`.

For a dense immediate input consisting only of float4 position followed by
the packed float3 varying, the exact H6 declaration is:

```text
A00 00000107
D0  010000c2   # offset 0, v0, float4 position
D1  02004182   # offset 4, v1, float3 packed varying
```

The immediate stride is seven dwords.  Format bytes `42/82/c2` are float2,
float3 and float4.  A software post-transform successor may instead append a
float3 D3 to the current position/color declaration and change the fixed
`D3 -> oT0` MOV destination mask from `0008028f` to `00080287`; that packet
shape still needs a separate guarded silicon test and must not be enabled by
the compiler-only evidence here.

The fragment validator must require all of the following as one closure:

- VS output T0 dimension at least three;
- matching H6 input declaration and actual stride;
- PS direct-input use mask containing T0;
- TAU physical coordinate zero and A91 bit zero;
- stage-zero image/sampler state and BO lifetime;
- general stage-zero perspective linkage (`A01=01002522`,
  `A08=08010201`) for a two-dimensional consumer and float3 producer;
- finite position/RHW and every existing render/cache guard.

Flat, noperspective, centroid/sample-qualified, integer, and dynamically
indexed varyings remain rejected.  Do not represent T0.x as packed D3DCOLOR;
all three lanes are raw float32.

## Implementation sequence

1. Run NIR IO lowering and assign each fragment varying one logical T slot.
2. Union lanes read by ALU with the meaningful coordinate lanes read by each
   TAU; reject more than four lanes or unsupported interpolation.
3. Allocate direct logical T slots through the compact coordinate allocator.
4. Emit a VS output whose component count is one plus the highest required
   lane, and derive A25/A26 from that count.
5. Emit one immediate attribute of the same float dimension and assert its
   stride/offset before drawing.
6. Encode ALU and TAU consumer swizzles independently; insert a bridge only
   for computed coordinates.
7. Finalize physical lifetimes and transition maps after allocation.

The first live oracle should use an asymmetric 2D texture and vertices whose
X, Y and Z lanes all differ.  Compare `texture(T0.yz) * T0.x` against a CPU
reference at interior pixels, then alternate with ordinary `texture(T0.xy)`
to prove stale A25/FVF/linkage state is fully replaced.

## Remaining timeout: the programmable dimension path

The guarded packed draw still timed out after the PS object, A00 resource
count and A08 producer width were made byte-exact.  A previous analysis
incorrectly treated the nominal 2-D texture target as the input to the
proprietary programmable-pixel-shader path and proposed A01=`01002521`,
A28=`28ffc000`.  That is the fixed-function result, not the result for this
programmable shader.  The runtime does derive A25 and A28 in separate passes,
but both passes retain three components in this case:

1. `stmUpdateFVFOut_H5i:0x001ba550..0x001baa87` converts the VS output
   metadata to A25/A26.  A float3 T0 therefore gives A25 nibble `e`.  This
   routine changes only A28 color bits 18:17 and preserves its texture-mask
   fields.
2. `stmComputeTexDim_invi:0x00176050..0x001760b7` first returns four when
   `ctx->...->+0x3a4` is null.  Otherwise it tests `ctx+0x4fcf`; when that flag
   is nonzero it returns three immediately at `0x00176071..0x0017607a`, before
   reading the bound texture's nominal dimension.  Only the zero-flag path
   reaches the ordinary 2-D result at `0x0017607c..0x001760b6`.
   `stmInitServerGlobals:0x000ee821..0x000ee83b` sets `+0x4fcf` exactly when
   the current programmable pixel-shader pointer at `+0x4f2c` is non-null.
   This packed draw has such a program, so the helper returns three (assuming
   the live texture-state object is present), not two.
3. In `stmUpdateTCI_invi`, `0x0017efc2..0x0017f012` takes the minimum of that
   helper result and the selected VS coordinate width and stores it at
   context `+0x3768`; `0x0017f018..0x0017f0aa` writes the matching
   dimension-minus-one into A01.  Width three capped by three stays three, so
   A01's dimension code is two.
4. `stmUpdatePS_inv:0x0017f9e7..0x0017fa22` saves A28 and clears its low 16
   texture-mask bits.  The loop at `0x0017fa3d..0x0017fab5`, with exact cases
   at `0x00180148` and `0x00180158`, maps effective widths 1/2/3/4 to mask
   nibbles `8/c/e/f`.  `0x0017fb11..0x0017fb5b` installs that independently
   constructed image in A28/A29.

For this exact programmable program, the vendor chain produces:

```text
A25 = 25f6e000   # immediate/VS producer contains T0.xyz
A08 = 08010201   # VS producer T0 is float3
A01 = 01002522   # programmable mapped-coordinate width is three
A28 = 28ffe000   # effective pixel-stage mask retains T0.xyz
```

The Windows D3D UMD independently confirms both branches.  Its helper at
`VTGUModeDX32A.dll:0x10157430..0x10157499` returns three when the analogous
programmable flag `ctx+0x616a` is nonzero; only its zero-flag branch reads the
ordinary texture dimension.  The stage-zero block at
`0x10157a90..0x10157b13` takes the minimum with the selected coordinate width,
stores it at context `+0x2d80`, and writes the dimension-minus-one field.

Thus A25 and A28 are independently constructed registers, but the packed
float3 programmable case legitimately has nibble `e` in both.  The observed
A01=`01002521`, A28=`28ffe000` packet mixes the fixed-function dimension cap
with the programmable A28 result and is not a proprietary-runtime tuple.
The exact production tuple is A01=`01002522`, A08=`08010201`,
A25=`25f6e000`, A28=`28ffe000`.  An earlier guarded A01=`...22` attempt also
timed out, so restoring this tuple is required for conformance but is not a
new explanation of the hang.  A28=`28ffc000` must not be tested as the next
discriminator for this shader.

### Float3 and shared-coordinate discriminators

Before retaining the sampler, the Linux production compiler gives an ALU-only
float3 front-end gate:

```text
oC0 = T0.xyzx

ALU 01c00530 001c0053 00f2c005 000d0820
A90..A92 90058001 91000000 92000001
active tuple (1,0,1,0)
```

The XYZX swizzle consumes X, Y and Z but supplies output alpha from X, so no
undefined fourth producer component is read.  This gate keeps the packed
float3 FVF/VS/A08/A25/A28 declaration while removing all image, sampler, TAU,
and ALU/TAU sharing.  If it does not retire, the mixed shader is not yet the
right isolation level.

A fresh hardware-free run of the Linux 5.75 production compiler and
configuration-two finalizer provides a smaller isolation object without
changing the packed producer/consumer relationship:

```text
r0 = TEX(s0, T0.yzww)
oC0 = r0 * T0.xxxx

ALU 0140b6d0 302c0000 00f20005 00000820
TAU 301006f0
A90..A93 90018000 91000001 92010001 93000001
active tuple (1,1,1,1)
```

The exact tokens and object hashes are stored in
`chrome9-packed-varying-linkage-corpus.json`.  The second case removes both TEMP
destinations, the partial XYZ write, literal ONE, the terminal MOV, and every
multi-ALU dependency while retaining the same float3 T0 producer, ALU XXXX
read, TAU YZWW read, sampler zero, and physical-coordinate overlap.  It
therefore follows the ALU-only float3 gate after restoring the exact
A01=`01002522` tuple.  If ALU-only float3 retires but this case times out, the
failure is isolated to the shared ALU/TAU coordinate path.  If both retire,
the remaining boundary is the multi-record/TEMP schedule.  These are proposed
guarded oracles, not silicon results.

The guarded cmd22 implementation is the `--one-alu` mode of
`tools/re/chrome9_packed_varying_guard_smoke.c`; omitting the option retains
the original two-ALU regression.  It uses four asymmetric BGRA texels, gray
0.5 and six float3 vertices, then checks all four interior quadrants, the
untouched exterior, both destination/source guard pages and the immutable
source image.  Because the direct-output shader multiplies all four lanes,
its expected alpha is half of the poisoned source alpha (within the same
one-UNORM-step tolerance as RGB).  A normal completion with four correct
quadrants qualifies the shared T0 ALU/TAU path.  An ioctl timeout or lost SSH
requires the usual engine quarantine and no retry; a normal but incorrect
image is a numerical/linkage failure, not a retirement success.

The one-ALU discriminator was run once on VX900 silicon on 2026-09-23 after
the programmable A01 correction.  The submitted ring contained the exact
ALU `0140b6d0 302c0000 00f20005 00000820`, TAU `301006f0`, and
A01=`01002522`.  It timed out on the first draw at the post-draw cookie
(`seq=17`, `kind=22`, current `0x7c00`, pause `0x7bf0`, stop `0x7c70`,
CR=`80000000`, engine=`00002f11`).  The engine was quarantined and no retry
was made.  This rules out TEMP allocation, partial writes, literal ONE,
multi-record H3 framing and an ALU dependency chain as necessary causes.  It
does not yet distinguish float3 FE/FVF transport from simultaneous ALU and
TAU consumption of the same physical coordinate.

The `varying_xyzx` mode of `tools/re/chrome9_fragment_smoke.c` implements the
preceding ALU-only float3 gate.  It submits constant float3
`(1,0.5,0.25)`, expects 1,176 covered pixels near packed BGRA `ffff8040`,
requires the exterior and both guard pages to remain unchanged, and carries
no texture or TAU state.  If it times out, the source-backed fault boundary is
the float3 declaration/producer path.  If it retires with the expected image,
the failed one-ALU texture draw isolates the remaining boundary to shared
ALU/TAU coordinate consumption and its runtime linkage state.

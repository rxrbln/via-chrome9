# Chrome9 packed varying to ALU and TAU linkage

Status: exact hardware-free Linux production-compiler result, independent
Windows raw-TAU cross-check, and exact Linux vertex-compiler output.  The new
three-component linkage has not executed on VX900 silicon.

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
- general stage-zero perspective linkage (`A01=01002521`,
  `A08=08010101`) for smooth interpolation;
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

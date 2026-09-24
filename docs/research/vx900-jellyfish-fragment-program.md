# VX900 jellyfish bounded fragment-program successor

## Decision

The highest-value fragment successor after one-record MOV/MUL/ADD is not a
standalone MIN/MAX opcode.  The captured glmark2 jellyfish fragment shader
reduces to two ordinary texture samples plus ADD/MUL; `pow(fresnel, 2)` is
optimized to a multiply.  It therefore exercises the compiler architecture
needed by later scenes without requiring native POW, reciprocal square root,
dynamic control flow, or projective sampling.

Do not pack these values into D1: D1 is normalized 8-bit D3DCOLOR and would
quantize and clamp a general GL varying.  The correctness baseline also keeps
the original shader arithmetic in its original stage rather than hoisting an
ADD across interpolation.  It uses T0.xy for the color coordinate, T1.xy for
the caustic coordinate, T2.xyz for ambient, and T3.xyzw for diffuse plus
fresnel.  A later optimized compiler may prove a five-ALU packed variant, but
it is not the initial general-NIR contract.

## Proprietary Linux production encoding

Linux and Windows production compilers byte-match this clean SSA main program
with two TAU records and six ALU records:

```text
00c30550 50140924 0072c205 000000a0
02010550 50140924 00720205 000000e0
0140b6d0 50000055 00720305 00000820
0140b6d0 d02c376d 0010c376 00000120
020076d0 d0140924 00120416 00000160
0080b680 80140924 008205b6 00000820

TAU:      301001b0 311111b1
control:  9000800c 91000003 92020006 93000002
A94..A9A: 00000000
```

The active-resource tuple is `(4,6,4,2)`.  With no color outputs, the four
varying formats produce A25 `25f0ccef` and A26 `26000000`; A28 and the
compiler-derived A62 must be preserved by the normal linkage builder.  The
final ALU stream SHA-256 is
`0ec25d4179a0a449421d6aa0941879ae0e271b6022b7e4621a1a3e768b6dff32`.

Semantically it computes:

```text
RGB = (T2.rgb + T3.rgb + tex1.rgb) * tex0.rgb
A   = tex0.a + T3.a * T3.a + c0.x     where c0.x = -0.3
```

The recovered gradient prepass that packs UV into D1 uses five ALU records
and no TAU:

```text
0140b6d0 d02c036d 0010c016 00000020
80810550 00140924 00708005 00000060
0140b6d0 50201055 00720000 000000a0
02020550 50140924 00708005 00000820
01c00530 b01c0053 008082b6 000d0820

control: 90058001 91000000 92000005
```

## Required compiler and ABI rung

The bounded successor needs:

- multiple ordered ADD/MUL records rather than one ALU instruction;
- identity and scalar-replicate source swizzles;
- partial RGB/alpha destination masks and temporary-register lifetimes;
- two validated, ordinary 2D TAU samples;
- generic full-float oT0..oT3 linkage and direct class-3 ALU reads from
  T2/T3; T0/T1 are live-proven as TAU coordinates, but T2/T3 direct ALU input
  is currently compiler-exact rather than silicon-proven;
- exact control-record counts rebuilt by the kernel;
- a fixed upper bound of this recovered program shape before generalization.

Userspace must submit semantic instructions and texture sources, never these
native words.  The kernel must rebuild every record, validate register and
last-use fields, and advertise a new capability only after an offline
byte-exact compiler test and a live guarded two-texture readback pass.  The
encodings are strong cross-driver evidence but remain pre-ABI until those
gates complete.

## Current default scene and computed-coordinate oracle

A 2026-09-24 NIR capture of the current default `jellyfish` run narrows the
remaining fragment requirement further.  The main fragment shader has three
full-float interpolators, two ordinary 2-D implicit-LOD samples, and this
straight-line graph:

```text
du = uCurrentTime * 0.05
dv = uCurrentTime * 0.025
tex0 = texture(s0, VAR0.xy)
ambient = vec3(VAR1.w, VAR2.x, VAR2.y)
tex1 = texture(s1, VAR0.zw + vec2(du, dv))
rgb = (ambient + VAR1.xyz + tex1.xyz) * tex0.xyz
alpha = tex0.w - 0.3 + VAR2.z * VAR2.z
```

There is no POW, normalize, comparison, discard, or control flow in this
program.  Unlike the clean four-varying object above, the second sample is a
dependent sample whose coordinate is written by ALU.  This is the one new
scheduler edge that a default-scene gate must cover.  The capture file had
SHA-256
`28ad25c161c534d44f31e7216fe1405eec8e6c0a11abf9379179b443f4a22921`.

`tools/chrome9_shader_corpus.py --case jellyfish_computed_coordinate`
contains a PS 2.0 program with the same graph and packing.  Constants are
`c0.x=uCurrentTime` and `c1=(0.05,0.025,-0.3,unused)`.  It needs eleven
semantic ALU operations and two TEX operations.  The production Linux 5.75
compiler inserts one additional TEMP-to-coordinate bridge and finalizes this
exact 12-ALU/two-TAU object:

```text
ALU
0140b6d0 d008116d 00108016 00000020
000016d0 d0140924 0010c056 00000060
0140b6d0 d008136d 00108016 00000020
0200a2d0 d0140924 0022c0ae 00000060
01c00530 101c0053 00f20104 000d04e0  # r1.xy -> coordinate slot 3
00c116d0 d0140924 0010c176 00000120
02c1a550 d0140924 0060c2a0 00000120
02030550 50140924 00720405 00000160
0140b6d0 50002055 00720505 00000820
0140b6d0 d02c256d 0010c256 000001a0
008156d0 d0140924 00120276 000001e0
0206b680 80140924 008207b6 00000820

TAU
30120110                              # TEX0, direct T0.xy
311331b1                              # TEX1, computed coordinate slot 3

A90..A9a
90040007 91000009 9202000c 93000003
94000000 95000018 96000000 97000000
98000000 99000000 9a000000
```

The pre-finalizer active-resource tuple is `(4,8,3,2)`.  A90 low bits 0--2
name the three externally supplied T registers, while A91 bits zero and three
name the direct T0 coordinate slot and the allocated computed-coordinate
slot.  The transition maps prove the interleaving rather than merely the
record counts:

```text
ALU 0..3 -> TAU 0 -> bridge ALU 4 -> TAU 1 -> ALU 5..11
```

A95 bits three and four are the two ALU-to-TAU transitions; A93 bits zero and
one are the matching TAU-to-ALU transitions.  Both TAUs carry their final
coordinate-read marker.  Therefore it is incorrect to upload all ALUs and
then both TAUs merely because the physical stores are separate, and it is
incorrect to derive A91 from sampler count.  The second coordinate must be
allocated after the three direct T inputs, bridged before TAU1, and retired
according to the complete interleaved schedule.

The packed final ALU, TAU, and combined-stream SHA-256 values are,
respectively,
`83f4249eec0ad9e61fd998020cf5bc568149886b50bed54d182c380ef314f4cc`,
`c445a141572f784710f5881a72f94381bae8f05d79ca28a35ca7ae76a2240ada`,
and
`4a435d6ee02dc928ee7cb2dddbf655b3f7707a0033979b6a42ebdabba8453795`.
The token blob SHA-256 is
`78e802dd7a5e4e8e8adbf81e69501fc8d36ee1880d11102644f4d64a7e334b29`.

This result used the normal production callbacks selected by
`stmPPSSetFunctionsPS20_invi` at Linux text `0x001b5a10` and
`stmPPSSetRegManager_invi` at `0x001b5ad0`, followed by
`stmPPS_PostTranslationSetting_invi(..., 2)` at `0x0019aba0`.  The resource
and relocation helpers are `stmGetActiveTexTmpNum` at `0x0019a410`,
`stmUpdateConfig` at `0x00198fe0`, and
`stmReplaceLocationTexIndexAINST` at `0x001990e0`.  The source DRI SHA-256 is
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
The smaller six-ALU main object above already byte-matches Linux and Windows;
this complete computed-coordinate object is currently a Linux production
oracle and is not claimed as a Windows byte-match.

The bounded live gate, when scheduled, should use two distinct 2x2 textures,
a nonzero time constant, independently distinct VAR0.zw, VAR1 and VAR2 lane
values, D16, exterior/BO guards, and known-good draws before and after.  A
direct-coordinate two-texture pass is not enough: success specifically
requires the bridge/TAU1 dependency above.  No target submission was made
while recovering this object.

## Current Mesa 19-record comparison

The current target dump at
`/root/tmp/chrome9-live/fract-select-dag/jellyfish-semantic.log` reports 19
ordered semantic records.  Two are TEX, so this is **17 semantic ALUs plus
two TEX**, not 19 ALUs plus two TEX.  The six-ALU difference from the minimal
faithful program above is entirely explicit vector assembly:

- two MOVs build packed ambient `(VAR1.w,VAR2.x,VAR2.y)` before its ADD;
- two MOVs build the computed XY coordinate before TEX1; and
- two partial MOVs plus a final MOV assemble the split RGB/alpha result,
  replacing the minimal program's two direct partial color writes.

All partial definitions are closed before use.  TEMP3 receives disjoint X and
YZ writes before an XYZ ADD; TEMP8 receives disjoint X and Y writes and its
TEX source is normalized to `XY01`; TEMP14 receives disjoint XYZ and W writes
before the final full color MOV.  No source selects an undefined lane.  The
constant bank is also legal: c0 is the time uniform and c29/c30/c31 are
`-0.3`, `0.025`, and `0.05`; no instruction names three distinct constants.

The kernel's destination-before-source-release allocator compacts the 15
virtual TEMP names to four physical registers.  In semantic order the map is:

```text
t3=p0, t2=p1, t5=p0, t4=p2, t7=p0, t6=p3, t8=p0,
t9=p2, t1=p0, t10=p1, t0=p2, t12=p0, t13=p1,
t11=p3, t14=p0
```

Re-expressing the exact current operation order with those four physical
names and passing it through the production Linux compiler/finalizer produces
the following oracle:

```text
ALU
01c00530 d01c0053 0010c176 000d0020
01c00530 d01c0053 0060c2a0 000d0020
02c10550 50140924 00700005 00000060
0140b6d0 d00af16d 00108016 00000020
000016d0 d0140924 0010c056 000000a0
0140b6d0 d00ae16d 00108016 00000020
000016d0 d0140924 0012c076 000000e0
01c00530 d01c0053 00100216 000d0020
01c00530 d01c0053 002003a2 000d0020
01c00530 101c0053 00f00004 000d04e0  # bridge p0.xyxy -> coord3
00020550 50140924 00700105 00000020
0140b6d0 50001055 00700005 000000a0
00ad16d0 d0140924 00100176 00000020
0140b6d0 d02c256d 0010c256 00000060
020116d0 d0140924 00100016 000000e0
01c00530 501c0053 00720205 000d0020
01c00530 801c0053 008203b6 000d0020
01c00530 301c0053 00f20005 000d0820

TAU     311231b1 301101f0
A90..   90050007 91000009 92020012 93000003
         94000000 95000600 96000000 97000000
         98000000 99000000 9a000000
```

This independently proves the allocator/resource result: physical high-water
four selects bucket `0x10000`, giving A90 `90050007`; A91 still names direct
coordinate zero and computed coordinate three.  The current order is

```text
ALU 0..9 -> TAU1 -> ALU 10 -> TAU0 -> ALU 11..17
```

so A95 bits nine and ten are `0x600`, while A93 bits zero and one remain
`0x3`.  Sampler order is intentionally reversed from the minimal oracle:
computed sampler 1 is `311231b1`, followed by direct sampler 0
`301101f0`.  This is valid and follows the current recursive NIR lowering.

There is one deliberate byte differential in the bridge's unused lanes.
PS 2.0 cannot express literal ZERO/ONE selectors in an ordinary source token,
so the proprietary oracle uses `p0.xyxy`.  Current Mesa submits `p0.xy01` to
satisfy defined-lane validation.  Applying the documented native source
packing changes only bridge record nine to:

```text
01c00530 c01c0053 00f00006 000d04e0  # current kernel XY01 form
```

Only X/Y feed the 2-D TAU.  The XY01 normalization has separate live silicon
coverage from the computed-coordinate effect2d path.  Therefore the current
jellyfish semantic stream has no static ISA, defined-lane, allocation,
resource-bucket, or phase-map mismatch against production.  Its one-frame
submission success is consistent with this result.  glmark2 reporting
`Validation: Unknown` is not itself a pixel failure and should not be treated
as evidence that this shader object is wrong; a deterministic guarded
two-texture oracle is still needed for numerical scene validation.

## Guarded numerical gate

`tools/re/chrome9_jellyfish_guard_smoke.c` now supplies that gate without
matching on a scene or shader name. It submits the exact current 19-record
semantic order above: 17 ALUs, sampler 1 from a TEMP-computed coordinate,
sampler 0 from direct T0, disjoint partial definitions, split RGB/alpha, and
the terminal color move. Constants are compacted to c0--c3, which does not
alter temporary lifetime or ALU/TAU phase ordering.

The two TEX source classes have different UAPI normalization contracts.  A
TEMP-computed coordinate may use `XY01`; the compiler inserts the proven
bridge and only X/Y feed the 2-D TAU.  A direct VARYING coordinate may name
only X/Y/Z/W components: `chrome9_fragment_validate()` rejects selectors
greater than W in that path before allocation.  Mesa consequently submits
the direct sampler-0 source as `T0.xyww`, not `T0.xy01`.  The first raw-oracle
attempt accidentally used `XY01` for both forms and therefore returned
`EINVAL` before execution, with no kernel fault and known-good draws passing
before and after.  This was a test-ABI error, not evidence against the
19-record shader, and the guarded tool now keeps `XY01` only on the computed
TEMP path and uses `XYWW` for direct T0.

Every vertex carries the same three float4 varyings. Direct sampler 0 reads
T0.xy=(.25,.25). The computed path forms
T0.zw + 12*(.05,.025)=(.85,.55), so nearest/repeat sampler 1 must select its
bottom-right texel. Distinct 2x2 BGRA source images make direct/computed
coordinate or sampler-slot swaps observable. The resulting arithmetic is:

```text
ambient + diffuse = (.1,.2,.3) + (.1,.2,.3) = (.2,.4,.6)
+ caustic          = (.3,.6,.9)
* color-map RGB    = (.15,.15,.1125)
alpha              = .75 - .3 + .4*.4 = .61
```

Including the source texels' UNORM8 quantization, the nearest host BGRA word
is `9b27261d`; the checker allows two bytes of output-conversion tolerance
per lane. The tool also requires D16 near `8000`, untouched
triangle exterior, intact leading/trailing guards on color, depth, and both
sources, and byte-identical source BOs. It builds with strict host warnings.
The corrected `XYWW` packet was accepted and retired on VX900.  Its first
checked interior pixel was `726d3d08`, however, rather than the CPU reference
`9b27261d`; D16 was `8000`, no kernel fault was logged, and known-good draws
passed both before and after.  This closes packet safety and the direct-source
validator issue, but it does **not** validate the shader numerically.  Until
the source of the color differential is isolated, the test remains an
unresolved ALU/varying/computed-coordinate linkage oracle rather than evidence
that jellyfish rendering is correct.

### T2.z discriminator

A second bounded packet replaced the alpha construction with a direct
`MOV temp14.w, V2.zzzz` while retaining the same three-varying/two-texture
envelope.  V2.z was `0.4`, so BGRA8 alpha should have been `0x66`.  The draw
retired but returned alpha `0x00`; RGB remained the same unresolved `6d3d08`
and D16 remained `8000`.  Known-good draws passed before and after, with no
new kernel fault.

A follow-up vector discriminator used `rgb = temp0.rgb + V2.xyz` and
`alpha = V2.z`.  Relative to the prior `006d3d08` pixel it produced
`00ad3d08`: only logical R increased by `0x40`; logical G, B, and A were
unchanged, despite nominal V2=`(.2,.3,.4)`.  The result therefore must not be
summarized as total T2-slot loss: at least the first component influences the
ALU result, while the later components do not arrive with their declared
float3 meaning.  This isolates a width/component-routing differential below
the CPU arithmetic reference.  It does not yet distinguish FE producer
width/link state from A90/resource routing or the native source selector, so
none of those layers should be changed from this result alone.

### Minimal float3-versus-float4 T2 route discriminator

A fresh manual, Linux VS-compiler, and Linux PS-compiler cross-check finds no
static mismatch in the float3 T2 image submitted above.  For dense full-width
T0/T1 followed by float3 T2, the complete width-sensitive producer state is:

```text
# post-transform H6 input: D3=float4 T0, D4=float4 T1, D5=float3 T2
FVF A00=00000511 A04=040063c2 A05=0500a4c2 A06=0600e582
A12=1200003f A25=25f6ffe0 A26=26000000
A28=28ffffe0 A29=29000000 A62=625081ff

# three outputs; T2 writes XYZ
FE A08=08030303 A09=09000002 A0a=0a000000
VS T2=0009414c 00000000 00000000 000802a7
VS A60=60800006
```

The `82`, `e`, `2`, and `7` fields independently mean float3/XYZ in the H6
declaration, FVF mask, FE dimension, and native VS destination.  The VX900
manual places T2's dimension in A09 bits 1:0 and T2's S/T/R/Q masks in A25
and A28 bits 7:4.  Thus moving the `2` to another A09 byte or changing only
one mask is not source-backed.

The smallest compiler-exact consumer which keeps physical T0, T1, and T2 is:

```text
MAD oC0, T0, T1, T2.xyzx

02c20500 302c1053 00f2c005 00000820
A90=90058007 A91=91000000 A92=92000001
active-resource-counts=(1,0,1,0)
```

Set T0 and T1 to zero and T2 to `(.2,.3,.4)`.  The expected color is then
`(.2,.3,.4,.2)`, approximately host BGRA word `33334d66`.  This removes both
textures, every temporary, the computed-coordinate bridge, and all but one
ALU while retaining all three physical coordinate slots and direct A90 bits
0--2.  In particular, type-FE A00 must be `00000011`: it is the compiler's
one active texture-temporary resource, not three varyings.

Run one otherwise identical float4 companion with T2.w=`.8`.  Its only
width-closure changes are:

```text
FVF A00=00000512 A06=0600e5c2
FE A09=09000003
A25=25f6fff0 A28=28fffff0
VS T2 destination=000802af
PS ALU=02c20530 302c1053 00f2c005 00000820
expected host BGRA=cc334d66
```

If float4 restores Y/Z while float3 does not, the remaining fault is in the
float3 width/link closure.  If both retain only T2.x, the failure is below the
width encoding and instead implicates physical slot-two routing/A90 or the
native class-3 source path.  `tools/re/chrome9_t2_route_guard_smoke.c`
implements the pair as `--float3` and `--float4`, including BGRA8, D16,
exterior, and guard checks.  It passes a strict host build.  This is a
proposed guarded cold-boot pair, not a silicon result; no GPU submission was
made during this audit.

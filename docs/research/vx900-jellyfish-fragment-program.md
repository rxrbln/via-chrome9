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

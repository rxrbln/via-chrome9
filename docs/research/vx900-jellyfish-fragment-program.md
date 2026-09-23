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

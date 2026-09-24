# Chrome9 ideas lighting compiler and capacity boundary

Status: source-exact current NIR, hardware-free Linux production compiler
objects, and an implementation boundary.  No GPU was used.

This note covers the main lit logo and three-light lamp fragment programs in
glmark2 `ideas`.  It does not treat the scene name or a shader hash as a
compiler key.  The finite-domain objects below are resource and instruction
oracles; the linked NIR's exceptional-value contract still prevents their
unqualified use as production lowering.

## Current post-option NIR

The glmark2 2023.01 fragment sources and SHA-256 values are:

| source | SHA-256 |
|---|---|
| `ideas-logo.frag` | `9bcd554caa675176f3ec19f6a0bf59c82e533b515343386f102a4db3eb5adab6` |
| `ideas-lamp-lit.frag` | `dc38503c9d11e0173e5ca068fdce05b627b990e62f7eee8914bfcb7eedb94b35` |

A host-only Mesa linker harness applied the current Chrome9 NIR options,
including `max_unroll_iterations=31`, then the same vector shrinking and DCE
used immediately before the semantic backend.  The complete printed logo and
lamp NIR SHA-256 values are respectively
`ad2c2733b2197551b61a7f0e230817cf0c355bb2c76baf610fb7c8b723266207`
and
`2a0b7b0ace57f4269b9179c55cf61d181802dbfcf0418ae2d2d950a6f4252b35`.
The harness is an analysis tool, not a production-tree change.

The fixed three-light loop is completely unrolled: no loop, phi, branch, or
integer induction operation remains.  Its post-option operation inventory is:

| NIR operation | logo | lamp |
|---|---:|---:|
| `bcsel` | 3 | 9 |
| `fadd` | 5 | 15 |
| `fdot3` | 5 | 13 |
| `feq` | 2 | 4 |
| `fmax` | 2 | 6 |
| `fmul` | 7 | 20 |
| `fneg` | 4 | 8 |
| `fpow` | 1 | 3 |
| `frcp` | 2 | 4 |
| `frsq` | 2 | 4 |
| `iand` | 1 | 3 |
| mixed-source `vec3` | 1 | 1 |
| **NIR ALU nodes** | **35** | **90** |

The mixed `vec3(eye.x, eye.y, eye.z)` needs two partial MOV records, so it
costs one more native record than its one NIR node.  The existing exact
integer-power lowering replaces POW30 by seven MULs and each POW20 by five
MULs.  Therefore, before any equality expansion:

```text
logo = 35 + 1 vec overhead + (7 - 1) = 42 ALU
lamp = 90 + 1 vec overhead + 3 * (5 - 1) = 103 ALU
```

The lamp already exceeds `CHROME9_PS_MAX_ALU=96`; comparison lowering cannot
fix that.  All four logo and all eight lamp `fneg` values are ALU sources.  A
Linux-only source-NEG schedule gives a compiled 38-record logo and a projected
95-record lamp respectively, leaving the projected lamp only one ALU record
of margin and no TEX bridge allowance (the shader has no TEX).  This is
capacity evidence, not an
implementation permission: Windows and Linux disagree for TEMP NEG, MUL
C-slot NEG, ADD/SUB NEG, and DP3 NEG, and the runtime NEG allowlist is empty.
`chrome9-swizzle-neg-boundary.md` and `chrome9-ps-temp-swizzle-neg.md` retain
the exact mismatch corpus.

## Exact current rejection

The logo's first recursively reached unsupported definition is `%22`:

```text
eq_v = feq(vertex_position.w, 0) // preserve:inf,nan
eq_l = feq(light0Position.w, 0)  // preserve:inf,nan
...
%20 = bcsel(eq_l, light.xyz, general)
%21 = bcsel(eq_v, -vertex.xyz, %20)
%22 = bcsel(iand(eq_v, eq_l), light.xyz - vertex.xyz, %21)
```

After unrolling, the lamp's corresponding first-light result is `%33` with
the same tree.  `chrome9_ps_lower_def()` sends every `bcsel` to
`chrome9_ps_lower_unit_interval_select()` at `chrome9_context.c:3164`.
That lowerer rejects at `:2919` because it accepts only a scalar result and
then only `fge` over the proved FRC `[0,1]` domain.  These unit-vector selects
are three-component FEQ trees.  The failure therefore occurs before a generic
FEQ opcode lookup and before the 96-record cap.

Do not discard the local `preserve:inf,nan` flags.  The kernel rejects every
non-finite uploaded fragment constant at `chrome9_fragment.c:1175` and every
non-finite submitted vertex varying lane at `chrome9_engine.c:7433`.  That is
useful containment but not a generic FEQ proof:

* finite inputs may be zero, subnormal, or become exceptional during
  interpolation or eager reciprocal evaluation;
* the linked NIR does not prove either W source is restricted to exact zero or
  one;
* the `unitvec` DAG eagerly computes reciprocals for arms that a later select
  may discard, so zero W can create infinity or NaN intermediates;
* native ABS/CMP is live-qualified for ordinary finite nonzero values and CMP
  signed zero, but not subnormal, infinity, or NaN behavior.

The finite source validation is therefore not permission to replace ordered
FEQ with an unqualified sign test.

## Finite-domain Linux unit-vector object

For resource sizing only, ordered equality to zero can be represented on a
domain independently proved finite, normal-or-zero, and non-NaN by
`CMP(-ABS(w), true, false)`.  Reusing each ABS and expanding the conjunction as
nested selection gives this exact 12-record straight-line unit-vector object:

```text
r0  = RCP v2.wwww
r1  = MUL v2, r0.xxxx
r2  = RCP v1.wwww
r3  = MUL v1, r2.xxxx
r4  = SUB r1, r3                 # general v2/w2 - v1/w1
r5  = SUB v2, v1                 # both-zero arm
r6  = ABS v2.wwww
r7  = ABS v1.wwww
r8  = CMP -r6, v2, r4            # w2 == 0
r9  = CMP -r7, -v1, r8           # w1 == 0
r10 = CMP -r6, r5, r9            # inner both-zero select
out = CMP -r7, r10, r9
```

Here `v1` is direct T0 and `v2` is c0.  Linux 5.75 production compilation and
configuration-two finalization returns active tuple `(1,11,1,0)`, 12 ALU
records, and controls:

```text
90040001 91000000 9200000c 93000000 94000000 95000000
96000000 97000000 98000000 99000000 9a000000
```

The PS 2.0 token SHA-256 is
`eb23f63f8a54d0c51790fff9ef06433fae160c01fe8dfbf5ca8db9e984453d4b`;
the native ALU SHA-256 is
`144c6f5d3a3b54ccb8d86c7dbe151391518652d2b35a95e3164c62ae3f676281`;
the controls SHA-256 is
`4a6e3d592ae2db564e53ed496e8dbdc9919971dbe15b3fee5a4551dc09736b51`.

This is not a production FEQ or NEG authorization.  It fixes the Linux
finite-domain schedule, source modifier placement, and temporary high-water
only.  Windows may require a different NEG expansion.

## Full finite-domain Linux logo object

Appending the exact linked logo arithmetic to that prefix gives a complete
38-record object.  It normalizes the selected light vector and packed normal,
forms reflection, constructs the packed eye vector with two partial MOVs,
uses the seven-MUL POW30 chain, and combines specular/ambient/diffuse terms.
Constants are c1=`2`, c2=`0`, c4=`(0,0,0,1)`, and
c5=`(0.5,0.4,0.7,1)`; c0 remains `light0Position`.  Direct T0--T2 are the
three linked varyings.

Linux production compilation returns active tuple `(3,11,3,0)` and:

```text
01c00530 b01c0053 00f080b6 00060020
0140b6d0 30000000 00f08005 00000060
01c00530 b01c0053 00f0c0b6 000600a0
0140b6d0 30002000 00f0c005 000000e0
80010530 30140924 00f00305 00000120
80800530 30140924 00f0c005 00000160
00806db8 b00806db 00f0806d 000901a0
00c06db8 b00c06db 00f0c06d 000901e0
00040530 38080053 00f00605 00090220
82080530 382c0053 00f00705 00090260
00090530 38005053 00f20605 000902a0
02090530 3800a053 00f20705 000902a0
01c00530 5000a055 00f00a05 00020020
01c00530 801c0053 00f000b6 00070060
0140b6d0 30001000 00f00a05 000002a0
01c00530 500c1055 00f0c105 00020020
01c00530 801c0053 00f000b6 00070060
0140b6d0 30001000 00f0c105 000000a0
81c00530 5000a055 00f00205 00020020
0140b6d0 00081000 00f00000 00000060
0140b6d0 00002053 00f00100 000000e0
00030538 38140924 00f00a05 00000120
01c00530 d01c0053 0012c176 000d0160
01c00530 d01c0053 0062c2a0 000d0160
01c00530 50005055 00f00405 00020020
01c00530 00000000 00f08200 000b0020
0140b6d0 00000000 00f00000 00000060
0140b6d0 00000000 00f00100 000000e0
0140b6d0 00003000 00f00300 00000060
0140b6d0 00000000 00f00100 000000e0
0140b6d0 00003000 00f00300 00000060
0140b6d0 00000000 00f00100 000000e0
0140b6d0 00003000 00f20300 00000060
00840530 00140924 00f20100 00000120
01c00530 5020a055 00f20205 00020020
01c00530 00000000 00f08200 000b0020
0140b6d0 30200000 00f08505 00000160
02050530 30140924 00f20405 00000820
```

The finalized controls are:

```text
90040007 91000000 92000026 93000000 94000000 95000000
96000000 97000000 98000000 99000000 9a000000
```

Packed little-endian SHA-256 values are:

| stream | SHA-256 |
|---|---|
| PS 2.0 tokens | `ec1080860a4747c4e970a1947855e1f464955ab6930b71fddb2ad5b2f8352124` |
| 38 native ALU records | `41df744b4720f6e2a516f82297ff54f6c8bcfd9e42fc01b926a7e3d8c8f5fa3b` |
| controls | `74e8659a4748103af831df950d3acecf4c1bb448fbc2c7993fe972519282b876` |
| ALU then controls | `4d7ee04c1ce097f1c69131bc408adefe34c1639693d6892700800d058605dc4c` |

A90 low bits `0x07` account for all three direct varyings.  Active temporary
high-water 11 remains inside the twelve-register physical file, but it leaves
only one physical slot; a future lowering must still pass generic liveness
allocation and must not copy this logical register assignment.  Because this
object uses NEG in several source positions, its bytes are explicitly
Linux-only capacity evidence.

## Smallest safe implementation gate

The first source-modifier-free improvement is generic SUB fusion:

```text
fadd(a, fneg(b)) -> fsub(a, b)
```

Apply it only when the `fneg` local FP controls are preserved by the SUB form,
and DCE the negate only after all of its users have fused.  It removes the
logo's divided-vector negate and reflected-term negate, reducing its
pre-FEQ-expansion count from 42 to 40.  In the lamp it removes the one
divided-vertex negate shared by all three lights plus the three reflected-term
negates, reducing that pre-FEQ count from 103 to 99.  This is exact ADD/SUB
algebra and does not select any disputed native source modifier.  An offline
test should assert those exact four NIR rewrites and reject a negate that has
any non-fusible user or incompatible local FP control.

SUB fusion is independently useful but does not make the lamp fit 96.  There
are two honest successors:

* qualify a canonical NEG encoding on silicon in every needed TEMP/A/C/B and
  DP3 position, explicitly resolving the Windows/Linux mismatch; together
  with the finite unit-vector lowering, the Linux schedule projects to 95
  records; or
* add generic vector SLP for the three independent equal-exponent POW20
  chains.  Packing three scalar bases, executing one five-MUL vec3 chain, and
  extracting lanes saves seven records after the three partial pack MOVs,
  preserving zero and per-lane multiplication order.  It is a broader
  allocator/pressure gate, not an `ideas` key.

Until either successor is proved, 99 is only the source-modifier-free
pre-FEQ count; it is not an executable whole-program bound.  The three-light
lamp remains over capacity before a conformant FEQ expansion is even assigned.
The fact that the semantic ABI accepts `DRM_CHROME9_FRAGMENT_MOD_NEG` does not
override the proprietary compiler disagreement or missing numerical silicon
gate.

FEQ/unit-vector selection is the subsequent guarded gate.  It requires either
a source-backed range proof that both W inputs are normal-or-zero and non-NaN
for the variant, or live qualification of ABS/CMP for subnormal, infinity, and
NaN.  Only then may the compiler fuse the exact nested select tree; arbitrary
FEQ, boolean materialization, and local FP-control erasure remain rejected.
